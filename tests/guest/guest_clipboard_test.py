#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One disposable native clipboard and two-editor workflow, using existing QMP/media helpers."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time
import zlib
import struct

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tests/shared'))
import artifact_lifecycle as artifacts
import qemu_accel
import guest_mgfs_test as guest
import guest_secondary_test as secondary

SOURCE = 'hello clipboard 🌱\nsecond line\n'.encode()


def png(ppm, destination):
    header, dimensions, maximum, pixels = ppm.read_bytes().split(b'\n', 3)
    width, height = map(int, dimensions.split())
    if header != b'P6' or maximum != b'255' or len(pixels) != width*height*3:
        raise ValueError('invalid framebuffer snapshot')
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind+data))
    rows = b''.join(b'\0'+pixels[row*width*3:(row+1)*width*3] for row in range(height))
    destination.write_bytes(b'\x89PNG\r\n\x1a\n' +
        chunk(b'IHDR', struct.pack('>IIBBBBB',width,height,8,2,0,0,0)) +
        chunk(b'IDAT',zlib.compress(rows)) + chunk(b'IEND',b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--large', action='store_true', help='exercise 8 MiB Reed documents and cross-editor clipboard edits')
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--accel', choices=('kvm','tcg'), default=qemu_accel.default_accel())
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--ovmf-code', type=Path, default=Path('/usr/share/OVMF/OVMF_CODE_4M.fd'))
    parser.add_argument('--ovmf-vars', type=Path, default=Path('/usr/share/OVMF/OVMF_VARS_4M.fd'))
    artifacts.add_retention_argument(parser)
    args = parser.parse_args()
    source = SOURCE
    if args.large:
        # Valid UTF-8, moderate logical lines, exact non-round content size.
        source = SOURCE + (b'large document contents 0123456789\n' * 270000)[:8*1024*1024]

    error = qemu_accel.accel_error(args.accel, qemu=args.qemu)
    if error: parser.error(error)
    build = args.build_dir.resolve()
    if any(',' in str(p) or '\n' in str(p) for p in (build,args.output,args.ovmf_code,args.ovmf_vars)):
        parser.error('QEMU paths cannot contain comma or newline')
    workspace = artifacts.ArtifactWorkspace(args.output, name='guest-clipboard', retain=args.retain_artifacts)
    work = workspace.path
    report = {'status':'INCOMPLETE', 'accel':args.accel,'cpus':2,
              'kernel_sha256':guest.digest(build/'Mangrove/pith.elf'),
              'payload_sha256':{name:guest.digest(build/name/(name+'.elf'))
                  for name in ('clipd','cliptest','reed','shoot')},
              'actions':[], 'observations':[], 'large_documents':args.large}
    process = qmp = None
    try:
        disk = guest.create_disk(work,build,'0123456789abcdef',power_history_smoke=True)
        workspace.set_failure_media(disk)
        image = bytearray((work/'initial-root.img').read_bytes())
        guest.install(image,2,'cliptest',(build/'cliptest/cliptest.elf').read_bytes())
        secondary.install(image,6,'clip-source.txt',source,owner=1000,permissions=3)
        secondary.install(image,6,'clip-target.txt',b'TARGET\n',owner=1000,permissions=3)
        (work/'initial-root.img').write_bytes(image)
        guest.check_image(build,work/'initial-root.img',work/'prepared-check.log')
        with disk.open('r+b') as stream: stream.seek(guest.ROOT_OFFSET);stream.write(image)
        variables = work/'vars.fd'
        shutil.copyfile(args.ovmf_vars,variables)
        guest.sync_image_for_boot(disk);guest.sync_image_for_boot(variables)
        invocation = [args.qemu,'-machine','q35','-accel',args.accel,
            '-cpu','host' if args.accel=='kvm' else 'max','-smp','2','-m','512M',
            '-rtc','base=utc,clock=vm','-display','none','-qmp','stdio',
            '-serial',f'file:{work}/serial.log','-debugcon',f'file:{work}/debugcon.log',
            '-drive',f'if=pflash,format=raw,readonly=on,file={args.ovmf_code}',
            '-drive',f'if=pflash,format=raw,file={variables}',
            '-drive',f'id=usb,file={disk},format=raw,if=none',
            '-device','qemu-xhci,id=xhci','-device','usb-kbd,bus=xhci.0,port=1',
            '-device','usb-storage,bus=xhci.0,port=2,drive=usb,bootindex=1',
            '-no-reboot','-no-shutdown']
        (work/'command.json').write_text(json.dumps(invocation,indent=2))
        with (work/'qemu.log').open('wb') as log:
            process = subprocess.Popen(invocation,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,bufsize=0)
        qmp = guest.QMP(process)
        overall_deadline = time.monotonic()+(600 if args.large else 300)
        def screen():
            if time.monotonic() >= overall_deadline: raise TimeoutError("workflow deadline")
            failure = guest.kernel_failure(work)
            if failure: raise RuntimeError(failure)
            path = work/'poll.ppm'
            qmp.call('screendump',{'filename':str(path)})
            return guest.screen_text(path)
        def wait(predicate, timeout=90 if args.large else 45):
            deadline=min(time.monotonic()+timeout,overall_deadline)
            while time.monotonic()<deadline:
                text=screen();(work/'poll.txt').write_text(text)
                if predicate(text):return text
                time.sleep(.05)
            raise TimeoutError('framebuffer condition not observed')
        prompt='developer ~:'
        def at_prompt(text): return text.rstrip().splitlines()[-1:]==[prompt]
        def key(code):
            result=qmp.call('human-monitor-command',{'command-line':f'sendkey {code} 30'})
            if result:raise RuntimeError(result)
            report['actions'].append(code)
            # Let this emulated USB press include a release before the next key.
            time.sleep(.05)
        def capture(name, predicate):
            text=wait(predicate)
            (work/(name+'.txt')).write_text(text)
            png(work/'poll.ppm',work/(name+'.png'))
            report['observations'].append(name)
        def command(text, complete=True):
            for index,char in enumerate(text):
                expected=(prompt+' '+text[:index]).rstrip()
                wait(lambda t:t.rstrip().splitlines()[-1:]==[expected])
                key({'/':'slash',' ':'spc','.':'dot','-':'minus','>':'shift-dot'}.get(char,char))
            key('ret')
            if complete:wait(at_prompt,180)
        def editor_command(text):
            key('slash')
            for char in text:key(char)
            key('ret')
        wait(at_prompt,90)
        command('/bin/cliptest suite > /temp/cliptest.log')
        capture('suite-returned',at_prompt)
        command('reed /temp/clip-source.txt',False)
        if args.large:
            wait(lambda t:'NORMAL' in t and '/temp/clip-source.txt' in t);key('g')
        capture('source-open',lambda t:'hello clipboard' in t and 'NORMAL' in t)
        key('alt-s');capture('source-selection',lambda t:'SELECT-CHAR' in t)
        key('c');capture('source-copy',lambda t:'Copied to clipboard.' in t)
        key('alt-s');key('x');capture('source-cut',lambda t:'Cut to clipboard.' in t and 'hello clipboard' not in t)
        key('u')
        if args.large:
            wait(lambda t:'NORMAL' in t and 'Opened.' not in t);key('g')
        capture('source-undo',lambda t:'hello clipboard' in t and 'NORMAL' in t)
        editor_command('q');wait(at_prompt)
        command('reed /temp/clip-target.txt',False)
        capture('target-open',lambda t:'TARGET' in t and 'NORMAL' in t)
        key('alt-s');key('v')
        if args.large:
            wait(lambda t:'Pasted from clipboard.' in t);key('g')
        capture('target-paste',lambda t:'hello clipboard' in t and 'Pasted from clipboard.' in t)
        key('u');capture('target-undo',lambda t:'TARGET' in t and 'hello clipboard' not in t)
        key('shift-u')
        if args.large:
            wait(lambda t:'NORMAL' in t and 'TARGET' not in t);key('g')
        capture('target-redo',lambda t:'hello clipboard' in t and 'TARGET' not in t)
        editor_command('s');capture('target-saved',lambda t:'Saved' in t)
        editor_command('q');wait(at_prompt)
        command('reed /temp/clip-target.txt',False)
        if args.large:
            wait(lambda t:'NORMAL' in t and '/temp/clip-target.txt' in t);key('g')
        capture('target-reopened',lambda t:'hello clipboard' in t and 'NORMAL' in t)
        editor_command('q');wait(at_prompt)
        command('say clipboard-restored')
        capture('shoot-restored',lambda t:at_prompt(t) and 'clipboard-restored' in t)
        command('/bin/shutdown',False)
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            if qmp.call('query-status').get('status')=='shutdown':break
            time.sleep(.05)
        else:raise TimeoutError('orderly shutdown not observed')
        qmp.call('quit');process.wait(timeout=10)
        if process.returncode:raise RuntimeError('QEMU failed')
        with disk.open('rb') as stream:
            stream.seek(guest.ROOT_OFFSET);image=stream.read(guest.PART_BYTES)
        stopped=work/'stopped-root.img';stopped.write_bytes(image)
        guest.check_image(build,stopped,work/'stopped-check.log')
        result=guest.payload(image,guest.find(image,'/temp/cliptest.log')[1]).decode()
        (work/'cliptest.log').write_text(result)
        if 'cliptest: PASS publisher-exit, 8-MiB' not in result or 'FAIL' in result:
            raise ValueError('native clipboard suite failed: '+result)
        for name in ('source','target'):
            actual=guest.payload(image,guest.find(image,f'/temp/clip-{name}.txt')[1])
            # Select All copies editable bytes; the target retains its own
            # final-newline setting. The source may deliberately lack one.
            expected = source if name == 'source' or source.endswith(b'\n') else source + b'\n'
            if actual!=expected:raise ValueError(f'{name}: saved bytes differ from its final-newline contract')
        report.update(status='PASS',native_suite=result,source_bytes=len(source),
            saved_bytes=len(source) + (0 if source.endswith(b'\n') else 1),
            source_sha256=hashlib.sha256(source).hexdigest(),
            saved_sha256=hashlib.sha256(source if source.endswith(b'\n') else source+b'\n').hexdigest(),
            smp=guest.inspect_smp_boot(disk,2),independent_mgfs='PASS')
    except Exception as error:
        report['status']='FAIL';report['error']=str(error)
    finally:
        if process is not None:artifacts.terminate_and_reap(process)
        report=workspace.finish(report,verified=report['status']=='PASS')
        print(json.dumps(report,indent=2),flush=True)
    return 0 if report['status']=='PASS' else 1

if __name__=='__main__':sys.exit(main())
