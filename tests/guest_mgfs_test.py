#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Opt-in real guest MGFS qualification. Never opens a developer disk image."""
import argparse
from collections import Counter
import hashlib
import json
import os
import resource
from pathlib import Path
import secrets
import shutil
import select
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tools'))
import image_payloads as manifest
import update_mgfs as image_tool
from populate_mgfs import directory_entry, checksum

SIZE = 8193
DISK_BYTES = 135283200
ROOT_OFFSET = 133120 * 512
PART_BYTES = 67108864


def digest(path):
    value=hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda:source.read(1024*1024),b''): value.update(block)
    return value.hexdigest()


def command(args, log):
    with log.open('ab') as output:
        subprocess.run([str(a) for a in args], cwd=ROOT, stdout=output,
                       stderr=subprocess.STDOUT, check=True, timeout=300,
                       env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1'})


def check_image(build, image, log):
    command([build/'mgfsck',image],log)
    text=log.read_text()
    if 'Errors: 0\n' not in text or 'Warnings: 0\n' not in text:
        raise ValueError('independent checker did not confirm clean media')


def layout(image):
    return {name: image_tool.u64(image, at) for name, at in (
        ('allocation_bitmap_start', 104), ('allocation_bitmap_blocks', 112),
        ('record_bitmap_start', 120), ('record_bitmap_blocks', 128),
        ('record_table_start', 136), ('record_table_blocks', 144),
        ('record_count', 152), ('data_start', 160), ('data_blocks', 168))}


def record(image, identifier):
    view = layout(image)
    at = image_tool.record_offset(image, view['record_table_start'], identifier,
                                  view['record_count'])
    value = image[at:at + 192]
    if image_tool.u64(value, 16) != identifier:
        raise ValueError('missing Record ' + str(identifier))
    return value


def payload(image, item):
    # Runtime MGFS may store short ordinary files inline in the Record.
    size = image_tool.u64(item, 32)
    if image_tool.u64(item, 0) == 1 and image_tool.u64(item, 8) & 1:
        if size > 56: raise ValueError('invalid inline file length')
        return bytes(item[128:128 + size])
    return image_tool.record_payload(image, item)


def find(image, path):
    identifier = image_tool.u64(image, 88)
    for part in path.strip('/').split('/'):
        matches = [item[2] for item in image_tool.directory_entries(
            payload(image, record(image, identifier)))
            if item[3] & 1 and item[4] == part.encode()]
        if len(matches) != 1:
            raise ValueError('namespace mismatch: ' + path)
        identifier = matches[0]
    return identifier, record(image, identifier)


def install(image, parent, name, payload):
    """Offline test payload installation via existing host image-builder primitives."""
    view = layout(image)
    slot = image_tool.allocate_record_slots(image, view, 1)[0]
    identifier = image_tool.u64(image, 96)
    struct.pack_into('<Q', image, 96, identifier + 1)
    changed = image_tool._create_payload_record(image, view, identifier, slot,
                                                1, payload, 1, set())
    old = image_tool.record_payload(image, record(image, parent))
    parent_changed = image_tool.replace_directory_payload(
        image, view, parent, old + directory_entry(identifier, name), set())
    for block in {changed, parent_changed}:
        image_tool.refresh_table_checksum(image, view, block)
    image_tool.refresh_record_bitmap_checksum(image, view)
    image_tool.refresh_metadata_checksums(image, view)
    header = bytearray(image[:4096]); checksum(header, 192, 200)
    image[:4096] = header


def create_disk(folder, build, run):
    log = folder / 'image.log'
    root = folder / 'initial-root.img'
    command([build / 'mkmgfs', '--blocks', '16384', '--uuid',
             '00000000-0000-0000-0000-000000000001', '--format-time-ns', '0', root], log)
    paths = []
    for _, key, _ in manifest.PAYLOAD_MANIFEST:
        directory = {'pith': 'Mangrove', 'sprout_core': 'sprout', 'sprout': 'sproutcmd'}.get(key, key)
        filename = 'pith.elf' if key == 'pith' else 'sprout.elf' if key == 'sprout_core' else key + '.elf'
        paths.append(build / directory / filename)
    command([sys.executable, ROOT / 'tools/populate_mgfs.py', root, *paths,
             *[ROOT / path for _, _, path in manifest.DATA_MANIFEST],
             '--autologin=developer'], log)
    image = bytearray(root.read_bytes())
    install(image, 2, 'fstest', (build / 'fstest/fstest.elf').read_bytes())
    for phase in ('create', 'verify', 'negative'):
        script = (f'/bin/fstest {"verify" if phase == "negative" else phase} {run} > /temp/{phase}.log\n'
                  f'/bin/say child-exit-0-{phase}-{run} > /temp/{phase}.exit\n'
                  '/bin/shutdown\n').encode()
        install(image, 6, phase + '.shoot', script)
    root.write_bytes(image)
    check_image(build,root,log)
    esp = folder / 'esp.img'
    with esp.open('wb') as output: output.truncate(PART_BYTES)
    command(['/usr/sbin/mkfs.fat', '-F32', '--invariant', esp], log)
    command(['mmd', '-i', esp, '::/EFI', '::/EFI/BOOT'], log)
    command(['mcopy', '-i', esp, build / 'EFI/BOOT/BOOTX64.EFI', '::/EFI/BOOT/'], log)
    disk = folder / 'disk.img'
    with disk.open('wb') as output: output.truncate(DISK_BYTES)
    command(['/usr/sbin/sgdisk', '--new=1:2048:133119', '--typecode=1:EF00',
             '--change-name=1:MANGROVE_ESP', '--new=2:133120:264191',
             '--typecode=2:8300', '--change-name=2:MANGROVE_ROOT', disk], log)
    with disk.open('r+b') as output:
        output.seek(2048 * 512); output.write(esp.read_bytes())
        output.seek(ROOT_OFFSET); output.write(image)
    return disk


class QMP:
    def __init__(self, process):
        self.process = process
        self.events = []
        self.receive()
        self.call('qmp_capabilities')

    def receive(self):
        line = bytearray()
        deadline = time.monotonic() + 10
        while not line.endswith(b'\n'):
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.process.stdout], [], [], remaining)[0]:
                raise TimeoutError('QMP response timeout')
            byte = self.process.stdout.read(1)
            if not byte: raise EOFError('QMP closed')
            line.extend(byte)
            if len(line) > 65536: raise ValueError('QMP response bound')
        return json.loads(line)

    def call(self, name, arguments=None):
        request = {'execute': name}
        if arguments is not None: request['arguments'] = arguments
        self.process.stdin.write(json.dumps(request).encode() + b'\n')
        self.process.stdin.flush()
        while True:
            reply = self.receive()
            if 'event' in reply:
                if len(self.events)>=1024: raise ValueError('QMP event bound')
                self.events.append(reply)
            if 'error' in reply: raise RuntimeError(reply)
            if 'return' in reply: return reply['return']

    def close(self):
        self.process.stdin.close(); self.process.stdout.close()


def screen_text(path):
    """Exact ASCII glyph recognition for the existing 8x16 console, not OCR."""
    header, dimensions, maximum, pixels = path.read_bytes().split(b'\n', 3)
    if header != b'P6' or maximum != b'255': raise ValueError('unsupported screendump')
    width, height = map(int, dimensions.split())
    if width * height > 4096 * 2160 or len(pixels) != width * height * 3:
        raise ValueError('screendump bounds')
    glyphs = {}
    for line in (ROOT / 'kernel/assets/font/unscii-16.hex').read_text().splitlines():
        if ':' not in line: continue
        code, bits = line.split(':')
        if 32 <= int(code, 16) < 127 and len(bits) == 32:
            glyphs[bytes.fromhex(bits)] = chr(int(code, 16))
    rows = []
    for y in range(10, height - 16, 18):
        row = ''
        for x in range(10, width - 8, 8):
            colors = [pixels[((y + dy) * width + x + dx) * 3:((y + dy) * width + x + dx) * 3 + 3]
                      for dy in range(16) for dx in range(8)]
            background = Counter(colors).most_common(1)[0][0]
            bits = bytes(sum((color != background) << (7 - dx)
                             for dx, color in enumerate(colors[dy * 8:dy * 8 + 8])) for dy in range(16))
            row += glyphs.get(bits, '?')
        rows.append(row.rstrip())
    return '\n'.join(rows)


def boot(folder, disk, phase, args):
    part = folder / phase
    part.mkdir()
    shutil.copyfile(args.ovmf_vars, part / 'vars.fd')
    invocation = [args.qemu, '-machine', 'q35', '-accel', args.accel,
                  '-cpu', 'max' if args.accel == 'tcg' else 'host', '-smp', '1', '-m', '512M',
                  '-rtc', 'base=utc,clock=vm', '-display', 'none', '-serial', f'file:{part}/serial.log',
                  '-qmp', 'stdio',
                  '-drive', f'if=pflash,format=raw,readonly=on,file={args.ovmf_code}',
                  '-drive', f'if=pflash,format=raw,file={part}/vars.fd',
                  '-drive', f'id=usb,file={disk},format=raw,if=none',
                  '-netdev', 'user,id=net0', '-device', 'e1000,netdev=net0',
                  '-device', 'qemu-xhci,id=xhci',
                  '-device', 'usb-storage,id=boot-storage,bus=xhci.0,port=2,drive=usb,bootindex=1',
                  '-device', 'usb-kbd,id=keyboard,bus=xhci.0,port=1', '-no-reboot', '-no-shutdown']
    (part / 'command.json').write_text(json.dumps(invocation, indent=2))
    started = time.monotonic()
    qmp = None
    launched = False
    negative_exit = False
    with (part / 'qemu.log').open('wb') as log:
        process = subprocess.Popen(invocation, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, bufsize=0)
        try:
            while process.poll() is None:
                if time.monotonic() - started > args.timeout: raise TimeoutError('guest phase timeout')
                for diagnostic in ('qemu.log','serial.log'):
                    path=part/diagnostic
                    if path.exists() and path.stat().st_size>8*1024*1024:
                        raise ValueError('guest diagnostic size bound')
                if qmp is None:
                    qmp = QMP(process)
                if qmp.call('query-status')['status'] == 'shutdown':
                    (part / 'blockstats.json').write_text(json.dumps(qmp.call('query-blockstats'),indent=2))
                    qmp.call('quit'); process.wait(timeout=10)
                    break
                try:
                    qmp.call('screendump', {'filename': str(part / 'screen.ppm')})
                except (EOFError, BrokenPipeError):
                    process.wait(timeout=10)
                    break
                text = screen_text(part / 'screen.ppm')
                (part / 'screen.txt').write_text(text)
                if '[FAIL] Required kernel bring-up phase failed' in text:
                    raise RuntimeError('guest kernel bring-up failed; see framebuffer evidence')
                if launched and 'exited with status 43.' in text:
                    if phase != 'negative': raise RuntimeError('native child failed with status 43')
                    negative_exit = True
                    (part / 'blockstats.json').write_text(json.dumps(qmp.call('query-blockstats'),indent=2))
                    qmp.call('quit')
                    process.wait(timeout=10)
                    break
                if launched and any(message in text for message in ('Could not run ', 'Could not wait for ', 'Could not close ')):
                    raise RuntimeError('native launch, exit or wait failure; see framebuffer evidence')
                if not launched and 'developer ~:' in text:
                    line = f'/bin/shoot /temp/{phase}.shoot'
                    for character in line:
                        key = {'/': 'slash', '.': 'dot', ' ': 'spc'}.get(character, character)
                        qmp.call('human-monitor-command', {'command-line': f'sendkey {key} 30'})
                        time.sleep(.04) # key release before the next physical key event
                    qmp.call('human-monitor-command', {'command-line': 'sendkey ret 30'})
                    launched = True
                time.sleep(.5) # bounded readiness polling, never substitutes for a witness
            if process.returncode != 0: raise RuntimeError(f'QEMU exit {process.returncode}')
            if not launched: raise RuntimeError('no native launch evidence')
            if phase == 'negative' and not negative_exit: raise RuntimeError('no expected child failure evidence')
        finally:
            if process.poll() is None:
                if qmp:
                    try: qmp.call('quit')
                    except (OSError, ValueError, EOFError): pass
                try: process.wait(timeout=10)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
            if qmp:
                # The process is stopped: collect bounded final events, without
                # waiting for a live QMP peer or trusting exit alone as success.
                tail = process.stdout.read(65537)
                if len(tail) > 65536: raise ValueError('QMP final output bound')
                for line in tail.splitlines():
                    item=json.loads(line)
                    if 'event' in item: qmp.events.append(item)
                (part / 'events.json').write_text(json.dumps(qmp.events,indent=2))
                qmp.close()
    if phase != 'negative' and not any(e.get('event') == 'SHUTDOWN' and
            e.get('data',{}).get('guest') is True for e in qmp.events):
        raise RuntimeError('no guest-initiated shutdown evidence')
    stats=(part/'blockstats.json')
    if not stats.exists(): raise RuntimeError('no stopped-guest block accounting')
    return {'seconds':time.monotonic()-started,'qemu_blockstats':json.loads(stats.read_text())}


def expected_payload(run):
    return bytes((i * 37 + (i >> 8) * 11 + ord(run[i % 16])) & 255 for i in range(SIZE))


def expected_operations(phase, run, identifier):
    # Independent contract, including exact operation names and return values.
    rows=[('deviced-running',1)]
    if phase == 'create': rows += [('absent',-2),('create',0)]
    rows += [('open',1)]
    if phase == 'create': rows += [('write',SIZE)]
    rows += [('seek-start',0),('read',SIZE),('bytes',SIZE),('eof',0)]
    data=expected_payload(run)
    for offset in (511,512,4095,4096,8191,8192):
        rows += [('seek-boundary',0),('read-boundary',1),('byte-boundary',data[offset])]
    rows += [('close',0),('reopen',1),('reread',SIZE),('reopened-bytes',SIZE),
             ('reclose',0),('stat',0),('type',1),('length',SIZE),('identity',1),
             ('directory-open',1),('entry-type',1),('entry-identity',identifier),
             ('directory-eof',-11),('namespace',1),('directory-close',0)]
    return rows


def validate_transcript(text, phase, run, identifier):
    rows=expected_operations(phase,run,identifier)
    wanted=f'GUESTFS1 phase={phase} run={run} bytes={SIZE}\n'
    for i,(name,value) in enumerate(rows,1):
        wanted += f'OP {i} {name} expected={value} observed={value}\n'
    wanted += f'GUESTFS1 complete phase={phase} run={run} ops={len(rows)} identifier={identifier}\n'
    if text != wanted: raise ValueError('missing, incorrect or incomplete operation transcript')


def negative_control(folder, disk, args, run):
    # Deliberately corrupt content only, on stopped disposable media. Metadata
    # remains checker-clean. The real verify process must fail, never repair it.
    with disk.open('r+b') as media:
        media.seek(ROOT_OFFSET); image=media.read(PART_BYTES)
        _, item=find(image,f'/temp/mgfs-{run}.bin')
        extents,_=image_tool.record_extents(image,item)
        logical,physical,count,flags=extents[0]
        if logical != 0 or count < 1 or flags != 1: raise ValueError('negative control mapping')
        media.seek(ROOT_OFFSET+physical*4096)
        media.write(bytes([expected_payload(run)[0]^1]))
    timing=boot(folder,disk,'negative',args)
    with disk.open('rb') as source:
        source.seek(ROOT_OFFSET); image=source.read(PART_BYTES)
    root=folder/'negative-root.img'; root.write_bytes(image)
    check_image(args.build_dir,root,folder/'negative-checker.log')
    _, item=find(image,f'/temp/mgfs-{run}.bin')
    wrong=bytearray(expected_payload(run)); wrong[0]^=1
    if payload(image,item) != wrong: raise ValueError('negative verifier modified content')
    _, transcript=find(image,'/temp/negative.log')
    text=payload(image,transcript).decode('ascii')
    (folder/'negative-guest.log').write_text(text)
    if ('FAIL bytes offset=0 ' not in text or 'GUESTFS1 complete' in text):
        raise ValueError('negative control did not detect wrong content')
    try: find(image,'/temp/negative.exit')
    except ValueError: pass
    else: raise ValueError('failed native child falsely reported successful completion')
    return {'status':'EXPECTED_FAILURE','child_status':43,'first_difference':0,**timing}


def inspect(folder, disk, phase, build, run):
    with disk.open('rb') as source:
        source.seek(ROOT_OFFSET); image = source.read(PART_BYTES)
    root = folder / f'{phase}-root.img'; root.write_bytes(image)
    check_image(build,root,folder/f'{phase}-checker.log')
    identifier, item = find(image, f'/temp/mgfs-{run}.bin')
    actual = payload(image, item)
    if image_tool.u64(item, 0) != 1 or actual != expected_payload(run):
        raise ValueError('independent payload/type mismatch')
    _, transcript = find(image, f'/temp/{phase}.log')
    text = payload(image, transcript).decode('ascii')
    (folder / f'{phase}-guest.log').write_text(text)
    validate_transcript(text, phase, run, identifier)
    _, completion = find(image, f'/temp/{phase}.exit')
    if payload(image, completion) != f'child-exit-0-{phase}-{run}\n'.encode():
        raise ValueError('missing waited-child completion')
    return {'identifier': identifier, 'payload_sha256': hashlib.sha256(actual).hexdigest(),
            'operations': 42 if phase == 'create' else 39, 'disk_sha256': digest(disk)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--accel', choices=('tcg', 'kvm'), default='tcg')
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--ovmf-code', type=Path, default=Path('/usr/share/OVMF/OVMF_CODE_4M.fd'))
    parser.add_argument('--ovmf-vars', type=Path, default=Path('/usr/share/OVMF/OVMF_VARS_4M.fd'))
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--repetitions',type=int,default=1)
    parser.add_argument('--negative-control',action='store_true')
    parser.add_argument('--run-id',help='reproduce one 16-character lowercase hexadecimal identifier')
    args = parser.parse_args()
    args.output = args.output.resolve(); args.build_dir = args.build_dir.resolve()
    # QEMU -drive uses commas as option separators, even within one argv item.
    for path in (args.output,args.ovmf_code,args.ovmf_vars):
        if ',' in str(path) or '\n' in str(path): parser.error('QEMU image paths cannot contain commas or newlines')
    args.output.mkdir(parents=True, exist_ok=False)
    if not 1 <= args.repetitions <= 4 or not 30 <= args.timeout <= 300:
        parser.error('repetitions must be 1..4 and timeout 30..300 seconds')
    if args.run_id and (len(args.run_id)!=16 or any(c not in '0123456789abcdef' for c in args.run_id) or args.repetitions!=1):
        parser.error('--run-id requires 16 lowercase hexadecimal characters and one repetition')
    report = {'status':'FAIL','accel':args.accel,'runs':[]}
    try:
        for _ in range(args.repetitions):
            run=args.run_id or secrets.token_hex(8)
            folder=args.output/run; folder.mkdir()
            result={'run':run}; report['runs'].append(result)
            disk=create_disk(folder,args.build_dir,run)
            result['initial_sha256']=digest(disk)
            for phase in ('create','verify'):
                result['active_phase']=phase
                elapsed=boot(folder,disk,phase,args)
                result[phase]=inspect(folder,disk,phase,args.build_dir,run)
                result[phase].update(elapsed)
            if result['create']['identifier'] != result['verify']['identifier']:
                raise ValueError('identity changed across boot')
            if args.negative_control:
                result['active_phase']='negative'
                result['negative']=negative_control(folder,disk,args,run)
            del result['active_phase']
            result['artifact_bytes']=sum(p.stat().st_size for p in folder.rglob('*') if p.is_file())
        usage=resource.getrusage(resource.RUSAGE_SELF)
        report['runner_cpu_seconds']=usage.ru_utime+usage.ru_stime
        report['runner_peak_rss_bytes']=usage.ru_maxrss*(1 if sys.platform=='darwin' else 1024)
        report['status']='PASS'
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
