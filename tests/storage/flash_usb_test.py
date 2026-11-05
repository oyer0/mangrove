#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real file I/O/GPT/MGFS, injected failures, and synthetic Linux admission.

No loop devices, physical devices, root privileges, or actual sysfs discovery.
"""
import argparse
from contextlib import redirect_stderr, redirect_stdout
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('flash_usb', ROOT/'tools/flash_usb.py')
flash = importlib.util.module_from_spec(spec); sys.modules[spec.name] = flash
spec.loader.exec_module(flash)


def rejected(function, text=None):
    try:
        function()
    except (flash.FlashError, OSError) as error:
        if text: assert text in str(error), str(error)
    else:
        raise AssertionError('invalid operation was accepted')


def fixture_image(work, mkmgfs):
    image, root = work/'compact.img', work/'mgfs.img'
    subprocess.run([str(mkmgfs), '--blocks', '16384', '--uuid',
                    '12345678-1234-1234-1234-123456789abc', '--format-time-ns', '0',
                    str(root)], check=True, capture_output=True, timeout=30)
    entries = bytearray(16384)
    for index, name, first, last, kind in (
            (0, 'MANGROVE_ESP', 2048, 133119, flash.ESP_TYPE),
            (1, 'MANGROVE_ROOT', 133120, 264191, b'R'*16)):
        offset = index*128
        entries[offset:offset+16] = kind
        entries[offset+16:offset+32] = bytes([index+1])*16
        struct.pack_into('<QQQ', entries, offset+32, first, last, index+7)
        encoded = name.encode('utf-16-le'); entries[offset+56:offset+56+len(encoded)] = encoded
    def header(current, other, table):
        value = bytearray(512); value[:8] = b'EFI PART'
        struct.pack_into('<II', value, 8, 0x10000, 92)
        struct.pack_into('<QQQQ', value, 24, current, other, 34, 264191)
        value[56:72] = b'D'*16
        struct.pack_into('<QIII', value, 72, table, 128, 128, zlib.crc32(entries))
        struct.pack_into('<I', value, 16, zlib.crc32(value[:92]))
        return value
    mbr = bytearray(512); mbr[450] = 0xee; mbr[451:454] = b'\xff'*3
    struct.pack_into('<II', mbr, 454, 1, 264224); mbr[510:] = b'\x55\xaa'
    with image.open('w+b') as disk:
        disk.truncate(flash.COMPACT_BYTES)
        disk.write(mbr); disk.write(header(1,264224,2)); disk.write(entries)
        disk.seek(264192*512); disk.write(entries); disk.write(header(264224,1,264192))
        disk.seek(2048*512); disk.write(b'ESP payload with nonzero bytes'*100)
        disk.seek(133120*512)
        with root.open('rb') as source:
            while data := source.read(flash.CHUNK): disk.write(data)
    return image


class FaultTarget(flash.FDTarget):
    def __init__(self, fd, fail_write=0, fail_read=False, corrupt=False, fail_sync=0):
        super().__init__(fd)
        self.calls = self.syncs = 0
        self.fail_write, self.fail_read = fail_write, fail_read
        self.corrupt, self.fail_sync = corrupt, fail_sync
    def write(self, offset, data):
        self.calls += 1
        if self.calls == self.fail_write:
            os.pwrite(self.fd, data[:512], offset)  # uncertain partial write
            raise OSError('injected partial write')
        super().write(offset, data)
    def sync(self, invalidate=False):
        self.syncs += 1
        if self.syncs == self.fail_sync: raise OSError('injected flush error')
        super().sync(invalidate)
    def read(self, offset, size):
        if self.fail_read: raise OSError('injected read error')
        data = super().read(offset, size)
        if self.corrupt and offset == 0: data = bytes([data[0]^1])+data[1:]
        return data


def transfer_tests(work, snapshot, gpt):
    summaries = []
    with snapshot.open('rb') as source:
        for sectors in (264225, 264226, 1048576, 30629376):
            target = work/f'target-{sectors}.img'
            size = sectors*512
            with target.open('w+b') as output:
                output.truncate(size)
                # Stale bytes cover payload, old GPT, and new GPT locations.
                for pos in (0, 2048*512, 133120*512, flash.COMPACT_BYTES-16896,
                            size-16896, size-512):
                    os.pwrite(output.fileno(), b'X'*512, pos)
                marker_at = flash.COMPACT_BYTES+flash.CHUNK
                if marker_at+512 < size-16896:
                    os.pwrite(output.fileno(), b'U'*512, marker_at)
                transport = flash.FDTarget(output.fileno())
                plan = flash.relocation_plan(gpt,size,physical=4096)
                written, read = flash.flash_snapshot(source.fileno(), transport, gpt, plan,
                    lambda: None, lambda: True, progress=lambda _:None)
                observed = flash.load_gpt(transport.read, size)
                assert observed.entries == gpt.entries  # includes attributes and GUIDs
                assert observed.primary[56:72] == gpt.primary[56:72]
                assert struct.unpack_from('<Q', observed.primary, 32)[0] == sectors-1
                assert written < flash.COMPACT_BYTES+65536 and read < flash.COMPACT_BYTES+65536
                if marker_at+512 < size-16896:
                    assert os.pread(output.fileno(),512,marker_at) == b'U'*512
                if sectors > 264258:
                    assert os.pread(output.fileno(),16896,264192*512) == bytes(16896)
                summaries.append({'sectors':sectors,'written':written,'verified':read})
            target.unlink()
        # Both geometry and confirmation failures precede the first write.
        target = work/'failures.img'
        with target.open('w+b') as output:
            output.truncate(flash.COMPACT_BYTES)
            plan = flash.relocation_plan(gpt,flash.COMPACT_BYTES)
            transport = FaultTarget(output.fileno())
            rejected(lambda: flash.flash_snapshot(source.fileno(),transport,gpt,plan,
                lambda:None,lambda:False,progress=lambda _:None), 'declined')
            assert transport.calls == 0
            checks = []
            def change_after_confirm():
                checks.append(1)
                if len(checks)==2: raise flash.FlashError('identity changed')
            rejected(lambda: flash.flash_snapshot(source.fileno(),transport,gpt,plan,
                change_after_confirm,lambda:True,progress=lambda _:None), 'identity changed')
            assert transport.calls == 0
            for kwargs in ({'fail_write':1},{'fail_write':132},{'fail_write':135},{'fail_read':True},
                           {'corrupt':True},{'fail_sync':1},{'fail_sync':2}):
                fault = FaultTarget(output.fileno(),**kwargs)
                rejected(lambda: flash.flash_snapshot(source.fileno(),fault,gpt,plan,
                    lambda:None,lambda:True,progress=lambda _:None))
        for size, logical, physical, alignment in (
                (flash.COMPACT_BYTES-512,512,512,0), (flash.COMPACT_BYTES+1,512,512,0),
                (flash.COMPACT_BYTES,4096,4096,0), (flash.COMPACT_BYTES,512,768,0),
                (flash.COMPACT_BYTES,512,4096,512), ((1<<63),512,512,0)):
            rejected(lambda: flash.relocation_plan(gpt,size,logical,physical,alignment))
        # PMBR saturation is tested without a multi-terabyte file or device.
        big = flash.relocation_plan(gpt,((1<<32)+100)*512)
        assert struct.unpack_from('<I',big.patches[0][1],458)[0] == 0xffffffff
        # Partial syscalls are completed; zero-length and error returns are not success.
        real_write = os.pwrite
        with target.open('r+b') as output:
            with patch.object(flash.os,'pwrite',side_effect=lambda f,d,o:real_write(f,d[:7],o)):
                flash.write_exact(output.fileno(),0,b'partial writes'*16)
            with patch.object(flash.os,'pwrite',return_value=0):
                rejected(lambda:flash.write_exact(output.fileno(),0,b'a'))
            rejected(lambda:flash.read_exact(output.fileno(),flash.COMPACT_BYTES,1))
            with patch.object(flash.fcntl,'ioctl',side_effect=PermissionError('cache invalidation refused')):
                rejected(lambda:flash.FDTarget(output.fileno(),block=True).sync(invalidate=True))
    return summaries


class FakeLinux:
    def __init__(self, folder):
        self.sys, self.proc, self.dev = folder/'sys', folder/'proc', folder/'dev'
        (self.sys/'dev/block').mkdir(parents=True); (self.proc/'self').mkdir(parents=True)
        self.dev.mkdir(); (self.sys/'bus/usb').mkdir(parents=True)
        self.usb = self.sys/'devices/usb/1-1'; self.usb.mkdir(parents=True)
        (self.usb/'subsystem').symlink_to(self.sys/'bus/usb')
        for name,value in (('idVendor','0781'),('idProduct','5567'),('serial','serial-123')):
            (self.usb/name).write_text(value)
        self.disk = self.node(self.usb/'host/block/usb','8:0')
        self.part = self.node(self.disk/'usb1','8:1',partition=True)
        (self.disk/'removable').write_text('1'); (self.disk/'ro').write_text('0')
        (self.disk/'device').mkdir(); (self.disk/'device/model').write_text('Cruzer Blade\n')
        self.host = self.node(self.sys/'devices/nvme/block/host','259:0')
        self.hostpart = self.node(self.host/'host2','259:2',partition=True)
        self.path = self.dev/'usb'; self.path.touch()
        self.numbers={str(self.path):os.makedev(8,0),'/dev/rootfs':os.makedev(259,2)}
        (self.proc/'swaps').write_text('Filename Type Size Used Priority\n')
        self.mount('259:2')
    def node(self,path,number,partition=False):
        path.mkdir(parents=True); (path/'dev').write_text(number)
        (path/'slaves').mkdir(); (path/'holders').mkdir()
        if partition:(path/'partition').write_text('1')
        (self.sys/'dev/block'/number).symlink_to(path)
        return path
    def mount(self,number,source='/dev/rootfs',fstype='ext4'):
        (self.proc/'self/mountinfo').write_text(f'1 0 {number} / / rw - {fstype} {source} rw\n')
    def stat(self,path):
        return SimpleNamespace(st_mode=stat.S_IFBLK|0o600,st_rdev=self.numbers[str(path)])
    def inspect(self,**kw):
        return flash.inspect_device(self.path,sys_root=self.sys,proc_root=self.proc,
                                    stat_path=self.stat,**kw)


def policy_tests(work):
    count=0
    def case(change=None,message=None):
        nonlocal count
        folder=work/f'linux-{count}'; count+=1
        env=FakeLinux(folder)
        if change:change(env)
        if message:rejected(env.inspect,message)
        else:assert env.inspect(expect_model='Cruzer Blade').serial=='serial-123'
        return env
    case()
    case(lambda e:(e.disk/'removable').write_text('0'),'not removable')
    case(lambda e:(e.disk/'ro').write_text('1'),'read-only')
    case(lambda e:(e.usb/'idVendor').unlink(),'USB-device ancestry')
    case(lambda e:(e.disk/'holders/active').touch(),'holders')
    case(lambda e:e.mount('8:1'),'mounted')
    case(lambda e:(e.proc/'swaps').write_text(f'Filename Type Size Used Priority\n{e.path} partition 1 0 0\n'),'swap')
    case(lambda e:e.mount('0:100','overlay','overlay'),'host root disk')
    case(lambda e:(e.proc/'self/mountinfo').write_text('invalid\n'),'malformed mount inventory')
    case(lambda e:e.mount('0:100','/dev/rootfs','btrfs'))
    def dm_root(e):
        dm=e.node(e.sys/'devices/dm','253:0')
        (dm/'slaves/usb1').symlink_to(e.part); e.mount('253:0')
    case(dm_root,'system filesystem')
    def btrfs_member(e):
        member=e.sys/'fs/btrfs/id/devices/1';member.mkdir(parents=True)
        (member/'dev').write_text('8:1')
    case(btrfs_member,'system filesystem')
    env=case(); rejected(lambda:env.inspect(expect_model='Wrong'),'model mismatch')
    partition_path=env.dev/'usb1';partition_path.touch();env.numbers[str(partition_path)]=os.makedev(8,1)
    rejected(lambda:flash.inspect_device(partition_path,sys_root=env.sys,proc_root=env.proc,
                                        stat_path=env.stat),'not a partition')
    original=env.inspect(); (env.usb/'serial').write_text('replaced')
    assert env.inspect()!=original
    # No real tty access. Confirmation must exactly match device and serial.
    class TTY:
        def __init__(self,line):self.line=line
        def __enter__(self):return self
        def __exit__(self,*_):pass
        def isatty(self):return True
        def write(self,_):pass
        def flush(self):pass
        def readline(self):return self.line
    for line,expected in ((f'ERASE {original.path} serial-123\n',True),('FLASH\n',False),('',False)):
        with patch('builtins.open',return_value=TTY(line)):
            assert flash.confirm_device(original)==expected


def cli_tests(work, image, checker):
    """Full CLI path with synthetic Linux identity and a regular-file fd.
    Only device discovery, kernel identity/ioctls and terminal input are mocked.
    The destination open flags, pinned fd, checks, writes and readback are real.
    """
    env=FakeLinux(work/'cli-linux')
    size=30629376*512
    with env.path.open('r+b') as disk:
        disk.truncate(size);disk.write(b'P'*512)
    real_open,real_stat,real_inspect=os.open,os.fstat,flash.inspect_device
    active={'fd':None};flags_seen=[];invalidations=[]
    def opened(path,flags,*args,**kwargs):
        fd=real_open(path,flags,*args,**kwargs)
        if Path(path)==env.path:
            flags_seen.append(flags);active['fd']=fd
        elif fd==active['fd']:
            active['fd']=None
        return fd
    def fd_stat(fd):
        value=real_stat(fd)
        if fd==active['fd']:
            return SimpleNamespace(st_mode=stat.S_IFBLK|0o600,st_rdev=os.makedev(8,0))
        return value
    scratch=work/'cli-success-scratch';scratch.mkdir()
    argv=['--image',str(image),'--device',str(env.path),
          '--expect-model','Cruzer Blade','--mgfsck',str(checker)]
    with patch.dict(os.environ,{'TMPDIR':str(scratch)}), \
         patch.object(flash,'inspect_device',side_effect=lambda *a,**k:real_inspect(
             env.path,expect_model='Cruzer Blade',sys_root=env.sys,proc_root=env.proc,stat_path=env.stat)), \
         patch.object(flash,'geometry',return_value=(size,512,512,0)), \
         patch.object(flash.os,'open',side_effect=opened), \
         patch.object(flash.os,'fstat',side_effect=fd_stat), \
         patch.object(flash.fcntl,'ioctl',side_effect=lambda fd,command:invalidations.append(command)):
        for accept in (False,True):
            active['fd']=None
            stdout=io.StringIO()
            with patch.object(flash,'confirm_device',return_value=accept), \
                 redirect_stdout(stdout),redirect_stderr(io.StringIO()):
                result=flash.main(argv)
            active['fd']=None
            assert result==(0 if accept else 1)
            assert ('VERIFIED:' in stdout.getvalue())==accept
            assert not list(scratch.iterdir())
            if not accept:
                with env.path.open('rb') as disk:assert disk.read(512)==b'P'*512
    assert len(flags_seen)==2 and invalidations==[0x1261]
    for flags in flags_seen:
        assert flags & os.O_EXCL and flags & os.O_NOFOLLOW and not flags & (os.O_TRUNC|os.O_CREAT)
    with env.path.open('rb') as disk:
        observed=flash.load_gpt(lambda o,n:flash.read_exact(disk.fileno(),o,n),size)
        assert observed.sectors==30629376
    env.path.unlink()


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--mkmgfs',type=Path,required=True)
    parser.add_argument('--mgfsck',type=Path,required=True)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    scratch=Path(os.environ.get('TMPDIR',ROOT/'build/flash-usb-tests'))
    scratch.mkdir(parents=True,exist_ok=True)
    if args.output: args.output.mkdir(parents=True,exist_ok=False)
    with tempfile.TemporaryDirectory(prefix='test-',dir=scratch) as temp:
        work=Path(temp);image=fixture_image(work,args.mkmgfs.resolve())
        snapdir=work/'snapshot';snapdir.mkdir()
        snapshot,gpt,digest=flash.snapshot_image(image,snapdir,args.mgfsck.resolve())
        assert digest==hashlib.sha256(image.read_bytes()).hexdigest()
        summaries=transfer_tests(work,snapshot,gpt)
        policy_tests(work)
        # Mutated metadata is rejected even when its CRC has been repaired.
        original=image.read_bytes()
        for pos in (512+16,1024+56,flash.COMPACT_BYTES-512+16):
            bad=bytearray(original);bad[pos]^=1
            rejected(lambda:flash.load_gpt(lambda o,n:bytes(bad[o:o+n]),len(bad),compact=True))
        def changed_table(change):
            bad=bytearray(original)
            array=bytearray(bad[1024:1024+16384]);change(array)
            for offset in (1024,264192*512):bad[offset:offset+16384]=array
            for offset in (512,264224*512):
                struct.pack_into('<I',bad,offset+88,zlib.crc32(array))
                struct.pack_into('<I',bad,offset+16,0)
                struct.pack_into('<I',bad,offset+16,zlib.crc32(bad[offset:offset+92]))
            return bad
        for change in (lambda a:struct.pack_into('<Q',a,128+32,2048),
                       lambda a:a.__setitem__(slice(128+16,128+32),bytes(16)),
                       lambda a:a.__setitem__(slice(128+16,128+32),a[16:32]),
                       lambda a:a.__setitem__(slice(128+56,128+128),bytes(72)),
                       lambda a:a.__setitem__(slice(0,16),b'X'*16)):
            bad=changed_table(change)
            rejected(lambda:flash.load_gpt(lambda o,n:bytes(bad[o:o+n]),len(bad),compact=True))
        rejected(lambda:flash.load_gpt(lambda o,n:original[o:o+n],len(original)+512,compact=True))
        rejected(lambda:flash.load_gpt(lambda o,n:original[o:o+n],len(original)-512,compact=True))
        # MGFS validation happens before the destination is even inspected.
        with image.open('r+b') as damaged:os.pwrite(damaged.fileno(),b'BAD!',133120*512)
        bad_folder=work/'bad-root';bad_folder.mkdir()
        rejected(lambda:flash.snapshot_image(image,bad_folder,args.mgfsck.resolve()),'MGFS root')
        image.write_bytes(original)
        cli_tests(work,image,args.mgfsck.resolve())
        cli_target=work/'ordinary-file';cli_target.write_bytes(b'preserve')
        cli_scratch=work/'cli-scratch';cli_scratch.mkdir()
        with patch.dict(os.environ,{'TMPDIR':str(cli_scratch)}), redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            status=flash.main(['--image',str(image),'--device',str(cli_target),
                               '--mgfsck',str(args.mgfsck.resolve())])
            assert not list(cli_scratch.iterdir())
            def interrupted(path,folder,checker):
                (folder/'partial').write_bytes(b'owned')
                raise KeyboardInterrupt()
            with patch.object(flash,'snapshot_image',side_effect=interrupted):
                assert flash.main(['--image',str(image),'--device',str(cli_target)])==1
            assert not list(cli_scratch.iterdir())
            with patch.object(flash.subprocess,'run',side_effect=subprocess.TimeoutExpired('mgfsck',120)):
                assert flash.main(['--image',str(image),'--device',str(cli_target),
                                   '--mgfsck',str(args.mgfsck.resolve())])==1
            assert not list(cli_scratch.iterdir())
        assert status==1 and cli_target.read_bytes()==b'preserve'
        if args.output:(args.output/'results.json').write_text(json.dumps(summaries,indent=2)+'\n')
    print('PASS: compact flashing/GPT relocation, bounded I/O, stale data, CRCs, root integrity, admission/confirmation, short I/O, write/flush/read failures')


if __name__=='__main__':main()
