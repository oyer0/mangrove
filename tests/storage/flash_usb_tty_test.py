#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real controlling-terminal confirmation through the production flashing CLI.

Only Linux device inventory, block identity and block ioctls are simulated.
Terminal opens/buffering, snapshots, MGFS checking, transfer and GPT readback
use real unprivileged descriptors. No physical device or sudo is used.
"""
import argparse
import errno
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import pty
import select
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import termios
import time
from types import SimpleNamespace
from unittest.mock import patch

import flash_usb_test as fixtures

ROOT = fixtures.ROOT


def load_source(path):
    spec = importlib.util.spec_from_file_location('flash_usb_tty_production', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    fixtures.flash = module
    return module


def fingerprint(path, compact_bytes):
    """All possible transfer regions plus a stale gap marker, without a 15GB read."""
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for offset, size in ((0, compact_bytes), (compact_bytes+1048576, 512),
                             (path.stat().st_size-16896, 16896)):
            source.seek(offset)
            while size:
                data = source.read(min(size, 1048576))
                assert data
                digest.update(data); size -= len(data)
    info = path.stat()
    return digest.hexdigest(), info.st_size, info.st_mtime_ns, info.st_ctime_ns


def child(args):
    flash = load_source(args.source)
    work = args.work
    env = fixtures.FakeLinux(work/'linux')
    size = 30629376*512
    with env.path.open('r+b') as disk:
        disk.truncate(size)
        for index, offset in enumerate((0, 2048*512, 133120*512,
                                       flash.COMPACT_BYTES-16896,
                                       flash.COMPACT_BYTES+1048576, size-16896)):
            os.pwrite(disk.fileno(), bytes([65+index])*512, offset)
    before = fingerprint(env.path, flash.COMPACT_BYTES)
    real_open, real_stat = os.open, os.fstat
    real_read, real_write = os.pread, os.pwrite
    real_inspect = flash.inspect_device
    active = {'fd':None}
    counts = {'opens':[], 'pwrite_calls':0, 'written':0, 'read':0,
              'invalidations':[], 'inspections':0}

    def opened(path, flags, *a, **kw):
        fd = real_open(path, flags, *a, **kw)
        if Path(path) == env.path:
            counts['opens'].append(flags); active['fd'] = fd
        elif fd == active['fd']:
            active['fd'] = None
        return fd

    def fd_stat(fd):
        value = real_stat(fd)
        if fd == active['fd']:
            return SimpleNamespace(st_mode=stat.S_IFBLK|0o600, st_rdev=os.makedev(8,0))
        return value

    def inspect(*a, **kw):
        counts['inspections'] += 1
        value = real_inspect(env.path, expect_model='Cruzer Blade', sys_root=env.sys,
                             proc_root=env.proc, stat_path=env.stat)
        if args.child == 'identity-change' and counts['inspections'] == 3:
            raise flash.FlashError('injected identity change after confirmation')
        return value

    def read(fd, size, offset):
        data = real_read(fd, size, offset)
        if fd == active['fd']: counts['read'] += len(data)
        return data

    def write(fd, data, offset):
        assert fd == active['fd'], 'unexpected non-target positioned write'
        counts['pwrite_calls'] += 1
        count = real_write(fd, data, offset)
        counts['written'] += count
        return count

    def ioctl(fd, command):
        assert fd == active['fd'] and command == 0x1261
        counts['invalidations'].append(command)

    scratch = work/'scratch'; scratch.mkdir()
    argv = ['--image',str(args.image),'--device',str(env.path),
            '--expect-model','Cruzer Blade','--mgfsck',str(args.mgfsck)]
    result = {'euid':os.geteuid(), 'stdin_tty':sys.stdin.isatty(),
              'stdout_tty':sys.stdout.isatty(), 'counts':counts}
    try:
        with patch.dict(os.environ, {'TMPDIR':str(scratch)}), \
             patch.object(flash, 'inspect_device', side_effect=inspect), \
             patch.object(flash, 'geometry', return_value=(size,512,512,0)), \
             patch.object(flash.os, 'open', side_effect=opened), \
             patch.object(flash.os, 'fstat', side_effect=fd_stat), \
             patch.object(flash.os, 'pread', side_effect=read), \
             patch.object(flash.os, 'pwrite', side_effect=write), \
             patch.object(flash.fcntl, 'ioctl', side_effect=ioctl):
            result['cli_rc'] = flash.main(argv)
        active['fd'] = None
        result['destination_unchanged'] = fingerprint(env.path,flash.COMPACT_BYTES) == before
        result['scratch_clean'] = not list(scratch.iterdir())
        assert result['scratch_clean']
        assert len(counts['opens']) == 1
        flags = counts['opens'][0]
        assert flags & os.O_EXCL and flags & os.O_NOFOLLOW
        assert not flags & (os.O_TRUNC|os.O_CREAT)
        if result['cli_rc'] == 0:
            with env.path.open('rb') as disk, args.image.open('rb') as source:
                observed = flash.load_gpt(lambda o,n:flash.read_exact(disk.fileno(),o,n),size)
                original = flash.load_gpt(lambda o,n:flash.read_exact(source.fileno(),o,n),
                                          flash.COMPACT_BYTES,compact=True)
            assert observed.entries == original.entries
            assert observed.primary[56:72] == original.primary[56:72]
            assert flash.u64(observed.primary,32) == 30629375
            assert counts['written'] == 135318016 and counts['read'] == 135334400
            assert counts['invalidations'] == [0x1261]
            result['gpt_and_contents_verified'] = True
        else:
            assert counts['pwrite_calls'] == 0 and counts['written'] == 0
            assert result['destination_unchanged']
        (work/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    finally:
        env.path.unlink()


def run_case(work, output, source, image, checker, name, stdin_mode, reply,
             expected, controlling=True, redirected=False):
    folder = work/name; folder.mkdir()
    master, slave = pty.openpty()
    token = f'ERASE {folder}/linux/dev/usb serial-123'

    def session():
        os.setsid()
        if controlling: fcntl.ioctl(slave,termios.TIOCSCTTY,0)

    command = [sys.executable,str(Path(__file__).resolve()),'--child',name,
               '--work',str(folder),'--source',str(source),'--image',str(image),
               '--mgfsck',str(checker)]
    process = None
    transcript = bytearray()
    prompt_seen = False
    try:
        process = subprocess.Popen(command, preexec_fn=session, pass_fds=(slave,),
            stdin=slave if stdin_mode=='tty' else
                  subprocess.PIPE if stdin_mode=='pipe' else subprocess.DEVNULL,
            stdout=subprocess.PIPE if redirected else slave,
            stderr=subprocess.PIPE if redirected else slave)
        os.close(slave); slave = None
        if stdin_mode == 'pipe':
            # Even an exact token on stdin cannot approve a write.
            process.stdin.write((token+'\n').encode()); process.stdin.close()
            process.stdin = None
        deadline = time.monotonic()+30
        while True:
            assert len(transcript) < 65536, 'unbounded child output'
            if time.monotonic() > deadline: raise TimeoutError(f'{name}: terminal child stalled')
            if select.select([master],[],[],.05)[0]:
                try: data = os.read(master,4096)
                except OSError as error:
                    if error.errno != errno.EIO: raise
                    data = b''
                if data: transcript.extend(data)
                elif process.poll() is not None: break
            if not prompt_seen and b'to overwrite this USB: ' in transcript:
                prompt_seen = True
                if reply == 'accept': os.write(master,(token+'\n').encode())
                elif reply == 'wrong': os.write(master,b'yes\n')
                elif reply == 'eof': os.write(master,b'\x04')
                elif reply == 'interrupt': process.send_signal(signal.SIGINT)
                else: raise AssertionError(f'{name}: unexpected confirmation prompt')
            if process.poll() is not None: break
        stdout, stderr = process.communicate(timeout=5)
        transcript.extend(stdout or b''); transcript.extend(stderr or b'')
        text = transcript.decode(errors='replace')
        if output: (output/f'{name}.log').write_text(text)
        assert process.returncode == 0, text
        result = json.loads((folder/'result.json').read_text())
        if output: (output/f'{name}.json').write_text(json.dumps(result,indent=2)+'\n')
        assert result['cli_rc'] == expected, text
        assert result['stdin_tty'] == (stdin_mode=='tty')
        assert result['stdout_tty'] != redirected
        assert prompt_seen == (controlling and name != 'before-fix')
        assert ('VERIFIED:' in text) == (expected==0)
        if name == 'before-fix': assert 'File or stream is not seekable' in text
        if name == 'identity-change': assert 'identity change after confirmation' in text
        return {'case':name,'stdin':stdin_mode,'controlling_tty':controlling,
                'cli_rc':result['cli_rc'],'writes':result['counts']['written'],
                'unchanged':result['destination_unchanged']}
    finally:
        if process is not None:
            if process.poll() is None:
                process.kill()
            process.wait()
        if slave is not None: os.close(slave)
        os.close(master)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mkmgfs',type=Path)
    parser.add_argument('--mgfsck',type=Path,required=True)
    parser.add_argument('--image',type=Path)
    parser.add_argument('--source',type=Path,default=ROOT/'tools/flash_usb.py')
    parser.add_argument('--legacy-source',type=Path)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--child',help=argparse.SUPPRESS)
    parser.add_argument('--work',type=Path,help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.child:
        child(args); return
    if not args.image and not args.mkmgfs: parser.error('provide --image or --mkmgfs')
    if args.output: args.output.mkdir(parents=True,exist_ok=False)
    scratch = Path(os.environ.get('TMPDIR',ROOT/'build/flash-usb-tests'))
    scratch.mkdir(parents=True,exist_ok=True)
    assert shutil.disk_usage(scratch).free >= fixtures.flash.COMPACT_BYTES*3
    summaries = []
    with tempfile.TemporaryDirectory(prefix='tty-',dir=scratch) as temporary:
        work = Path(temporary).resolve()
        image = args.image.resolve() if args.image else fixtures.fixture_image(work,args.mkmgfs.resolve())
        def run(name, stdin, reply, expected, **kw):
            summaries.append(run_case(work,args.output,args.source.resolve(),image,
                                      args.mgfsck.resolve(),name,stdin,reply,expected,**kw))
        if args.legacy_source:
            summaries.append(run_case(work,args.output,args.legacy_source.resolve(),image,
                args.mgfsck.resolve(),'before-fix','tty',None,1))
        run('terminal-accept','tty','accept',0)
        run('terminal-decline','tty','wrong',1)
        run('terminal-eof','tty','eof',1)
        run('terminal-interrupt','tty','interrupt',1)
        run('pipe-accept','pipe','accept',0)
        run('pipe-cannot-approve','pipe','wrong',1)
        run('null-stdin-accept','null','accept',0)
        run('redirected-output','null','accept',0,redirected=True)
        run('no-ctty-pipe','pipe',None,1,controlling=False)
        run('no-ctty-tty-stdin','tty',None,1,controlling=False)
        run('identity-change','tty','accept',1)
    assert not work.exists(), 'owned test workspace was not removed'
    if args.output:
        (args.output/'results.json').write_text(json.dumps(summaries,indent=2)+'\n')
    print('PASS: real PTY CLI confirmation with terminal/pipe/null stdin, decline/EOF/SIGINT/no controlling terminal, pre-write rollback and GPT/MGFS readback')


if __name__ == '__main__': main()
