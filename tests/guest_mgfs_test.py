#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Opt-in real guest MGFS qualification. Never opens a developer disk image."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import os
import re
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


def create_disk(folder, build, run, relative_launch=False):
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
    if relative_launch:
        install(image,6,'fstest-relative',(build / 'fstest/fstest.elf').read_bytes())
    for phase in ('create', 'verify', 'negative', 'stdio', 'time'):
        prefix = "cd /temp\n" if relative_launch else ""
        executable = "./fstest-relative" if relative_launch else "/bin/fstest"
        if phase == 'time':
            launch_command = f'{executable} time'
        elif phase == 'stdio':
            launch_command = f'{executable} stdio {run}'
        else:
            launch_command = f'{executable} {"verify" if phase == "negative" else phase} {run}'
        script = (prefix + f'{launch_command} > /temp/{phase}.log\n'
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
    def __init__(self, process, reader=None, writer=None):
        self.process = process
        self.reader = reader if reader is not None else process.stdout
        self.writer = writer if writer is not None else process.stdin
        self.events = []
        self.receive()
        self.call('qmp_capabilities')

    def receive(self):
        line = bytearray()
        deadline = time.monotonic() + 10
        while not line.endswith(b'\n'):
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.reader], [], [], remaining)[0]:
                raise TimeoutError('QMP response timeout')
            byte = self.reader.read(1)
            if not byte: raise EOFError('QMP closed')
            line.extend(byte)
            if len(line) > 65536: raise ValueError('QMP response bound')
        return json.loads(line)

    def call(self, name, arguments=None):
        request = {'execute': name}
        if arguments is not None: request['arguments'] = arguments
        self.writer.write(json.dumps(request).encode() + b'\n')
        self.writer.flush()
        while True:
            reply = self.receive()
            if 'event' in reply:
                if len(self.events)>=1024: raise ValueError('QMP event bound')
                self.events.append(reply)
            if 'error' in reply: raise RuntimeError(reply)
            if 'return' in reply: return reply['return']

    def drain_events(self):
        # Peer has stopped. Pipes kept open by the host do not produce EOF;
        # drain only available bytes, with the same strict finite bound.
        tail = bytearray()
        while select.select([self.reader], [], [], 0)[0]:
            chunk = os.read(self.reader.fileno(), 65537 - len(tail))
            if not chunk: break
            tail.extend(chunk)
            if len(tail) > 65536: raise ValueError('QMP final output bound')
        for line in tail.splitlines():
            item = json.loads(line)
            if 'event' in item:
                if len(self.events) >= 1024: raise ValueError('QMP event bound')
                self.events.append(item)

    def close(self):
        self.writer.close(); self.reader.close()

    def wait_deleted(self, device):
        deadline = time.monotonic() + 10
        while not any(e.get('event') == 'DEVICE_DELETED' and
                      e.get('data', {}).get('device') == device for e in self.events):
            if time.monotonic() >= deadline: raise TimeoutError('USB deletion event missing')
            event = self.receive()
            if 'event' not in event: raise ValueError('unexpected QMP response during deletion')
            if len(self.events) >= 1024: raise ValueError('QMP event bound')
            self.events.append(event)


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


def confirmation_echo(text, answer):
    lines = text.rstrip().splitlines()
    return answer in ('y', 'n') and bool(lines) and lines[-1] == '[y/N] ' + answer


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
    secondary = getattr(args, 'secondary_disk', None)
    hotplug_run = getattr(args, 'hotplug_run', None)
    if secondary:
        if hotplug_run:
            invocation += ['-blockdev', json.dumps({'driver':'file', 'node-name':'secondary-file',
                                                   'filename':str(secondary)}),
                           '-blockdev', json.dumps({'driver':'raw', 'node-name':'secondary',
                                                   'file':'secondary-file'})]
        else:
            invocation += ['-drive', f'id=secondary,file={secondary},format=raw,if=none' +
                       (',readonly=on' if getattr(args, 'secondary_readonly', False) else ''),
                          ]
        invocation += ['-device', 'usb-storage,id=secondary-storage,bus=xhci.0,port=3,drive=secondary']
        replacement = getattr(args, 'hotplug_replacement', None)
        if replacement:
            invocation += ['-blockdev', json.dumps({'driver':'file','node-name':'secondary-wrong-file','filename':str(replacement)}),
                           '-blockdev', json.dumps({'driver':'raw','node-name':'secondary-wrong','file':'secondary-wrong-file'})]
        (part / 'command.json').write_text(json.dumps(invocation, indent=2))
    answers = list(getattr(args, 'confirmations', []))
    answered_screen = None
    pending_answer = None
    pending_prompt = None
    confirmed = 0
    started = time.monotonic()
    qmp = None
    launched = False
    launch_line = None
    launch_sent = 0
    negative_exit = False
    hotplug_actions = []
    expected_failure = phase == 'negative' or getattr(args, 'expected_child_failure', False)
    pipe_qmp = getattr(args, 'qmp_pipe', False)
    reader = writer = None
    if pipe_qmp:
        prefix = part / 'qmp'
        for suffix in ('.in', '.out'): os.mkfifo(str(prefix) + suffix, 0o600)
        writer = os.fdopen(os.open(str(prefix)+'.in', os.O_RDWR | os.O_NONBLOCK), 'wb', buffering=0)
        reader = os.fdopen(os.open(str(prefix)+'.out', os.O_RDWR | os.O_NONBLOCK), 'rb', buffering=0)
        invocation[invocation.index('-qmp')+1] = 'pipe:' + str(prefix)
    control = getattr(args, 'lifetime_control', None)
    if control is not None: control.configure(invocation, part)
    (part / 'command.json').write_text(json.dumps(invocation, indent=2))
    with (part / 'qemu.log').open('wb') as log, (part / 'diagnostics.log').open('wb') as diagnostic:
        process = subprocess.Popen(invocation, stdin=subprocess.DEVNULL if pipe_qmp else subprocess.PIPE,
                                   stdout=diagnostic if pipe_qmp else subprocess.PIPE, stderr=log, bufsize=0)
        try:
            poll_interval=getattr(args,'guest_poll_interval',.5)
            if not .05 <= poll_interval <= 1:raise ValueError('guest poll interval bound')
            while process.poll() is None:
                if time.monotonic() - started > args.timeout: raise TimeoutError('guest phase timeout')
                for diagnostic in ('qemu.log','serial.log','diagnostics.log'):
                    path=part/diagnostic
                    if path.exists() and path.stat().st_size>8*1024*1024:
                        raise ValueError('guest diagnostic size bound')
                if qmp is None:
                    qmp = QMP(process, reader, writer)
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
                if launched and control is not None: control.observe(qmp, text, part, started)
                if launched and hotplug_run:
                    stage = len(hotplug_actions)
                    marker = f'HOTPLUG1 {"ready" if stage == 0 else "removed"} run={hotplug_run}'
                    if stage < 2 and marker in text:
                        (part / f'hotplug-{stage}.txt').write_text(text)
                        # Frontend deletion resets its QEMU accounting. Capture
                        # the completed initial workload before removing it.
                        before_delete = qmp.call('query-blockstats') if stage == 0 else None
                        if stage == 0:
                            qmp.call('device_del', {'id':'secondary-storage'})
                            qmp.wait_deleted('secondary-storage')
                        else:
                            qmp.call('device_add', {'driver':'usb-storage','id':'secondary-storage-new',
                                'bus':'xhci.0','port':'3','drive':'secondary-wrong' if getattr(args,'hotplug_replacement',None) else 'secondary'})
                        hotplug_actions.append({'stage':stage, 'marker':marker,
                                                'seconds':time.monotonic()-started})
                        if before_delete is not None:
                            hotplug_actions[-1]['qemu_blockstats_before_delete'] = before_delete
                        qmp.call('human-monitor-command', {'command-line':'sendkey ret 30'})
                if launched and 'exited with status 43.' in text:
                    if not expected_failure: raise RuntimeError('native child failed with status 43')
                    negative_exit = True
                    (part / 'blockstats.json').write_text(json.dumps(qmp.call('query-blockstats'),indent=2))
                    qmp.call('quit')
                    process.wait(timeout=10)
                    break
                if launched and any(message in text for message in ('Could not run ', 'Could not wait for ', 'Could not close ')):
                    raise RuntimeError('native launch, exit or wait failure; see framebuffer evidence')
                if pending_answer is not None:
                    # QMP acknowledgement proves injection, not guest delivery.
                    # Require native console echo before submitting the line.
                    if confirmation_echo(text, pending_answer):
                        qmp.call('human-monitor-command', {'command-line': 'sendkey ret 30'})
                        answered_screen = pending_prompt
                        pending_answer = None
                        confirmed += 1
                elif launched and text.rstrip().endswith('[y/N]') and text != answered_screen:
                    if not answers: raise RuntimeError('unexpected trusted authorization prompt')
                    answer = answers.pop(0)
                    if answer not in ('y', 'n'): raise ValueError('invalid authorization decision')
                    qmp.call('human-monitor-command', {'command-line': f'sendkey {answer} 30'})
                    pending_answer = answer
                    pending_prompt = text
                prompt = getattr(args, 'login_prompt', 'developer ~:')
                if not launched and launch_line is None and prompt in text:
                    launch_line = f'/bin/shoot /temp/{phase}.shoot'
                elif not launched and launch_line is not None:
                    lines = text.rstrip().splitlines()
                    expected = (prompt + ' ' + launch_line[:launch_sent]).rstrip()
                    # Never advance on QMP acknowledgement or elapsed time.
                    # Prefix echo gates each key; the complete exact native
                    # command line must be visible before Enter is injected.
                    if lines and lines[-1] == expected:
                        if launch_sent < len(launch_line):
                            character = launch_line[launch_sent]
                            key = {'/':'slash','.':'dot',' ':'spc','-':'minus'}.get(character,character)
                            qmp.call('human-monitor-command', {'command-line':f'sendkey {key} 30'})
                            launch_sent += 1
                        else:
                            qmp.call('human-monitor-command', {'command-line':'sendkey ret 30'})
                            launched = True
                time.sleep(poll_interval) # condition polling never substitutes for a witness
            if process.returncode != 0: raise RuntimeError(f'QEMU exit {process.returncode}')
            if not launched: raise RuntimeError('no native launch evidence')
            if expected_failure and not negative_exit: raise RuntimeError('no expected child failure evidence')
        finally:
            if hotplug_run:
                (part / 'hotplug-actions.json').write_text(json.dumps(hotplug_actions,indent=2))
            if process.poll() is None:
                if qmp:
                    try: qmp.call('quit')
                    except (OSError, ValueError, EOFError): pass
                try: process.wait(timeout=10)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
            if qmp:
                # The process is stopped: collect bounded final events, without
                # waiting for a live QMP peer or trusting exit alone as success.
                qmp.drain_events()
                (part / 'events.json').write_text(json.dumps(qmp.events,indent=2))
                qmp.close()
            elif pipe_qmp:
                reader.close(); writer.close()
    if control is not None: control.finish(expected_failure, negative_exit)
    if not expected_failure and not any(e.get('event') == 'SHUTDOWN' and
            e.get('data',{}).get('guest') is True for e in qmp.events):
        raise RuntimeError('no guest-initiated shutdown evidence')
    if answers: raise RuntimeError('missing trusted authorization prompts')
    if pending_answer is not None: raise RuntimeError('unconfirmed authorization input')
    if hotplug_run and len(hotplug_actions) != 2: raise RuntimeError('incomplete hotplug handshakes')
    stats=(part/'blockstats.json')
    if not stats.exists(): raise RuntimeError('no stopped-guest block accounting')
    return {'seconds':time.monotonic()-started,'launch_echoed_characters':launch_sent,'child_status':43 if expected_failure else 0,
            'hotplug_actions':hotplug_actions,
            'authorization_decisions':confirmed,
            'qemu_blockstats':json.loads(stats.read_text())}


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


def inspect_stdio(folder, disk, build, run):
    with disk.open('rb') as source:
        source.seek(ROOT_OFFSET); image = source.read(PART_BYTES)
    root = folder / 'stdio-root.img'; root.write_bytes(image)
    check_image(build, root, folder / 'stdio-checker.log')
    binary_path = f'/temp/stdio-{run}.bin'
    text_path = f'/temp/stdio-{run}.txt'
    binary_id, binary_record = find(image, binary_path)
    text_id, text_record = find(image, text_path)
    binary = payload(image, binary_record)
    expected = bytes((i * 73 + (i >> 4) * 19 + 0x5b) & 255 for i in range(4097))
    expected_text = f'MTS-{run}|-17|3.25\n'.encode()
    if binary != expected or text_record[0] != 1 or payload(image, text_record) != expected_text:
        raise ValueError('independent stdio file bytes or metadata mismatch')
    _, transcript = find(image, '/temp/stdio.log')
    text = payload(image, transcript).decode('ascii')
    (folder / 'stdio-guest.log').write_text(text)
    expected_rows = [
        ('binary-open', 1), ('binary-write', 4097), ('binary-flush', 0),
        ('binary-seek', 0), ('binary-read', 4097), ('binary-content', 1),
        ('binary-eof', 1), ('rewind', 0), ('binary-close', 0),
        ('text-exclusive-create', 1), ('text-write', len(expected_text)),
        ('text-line-read', len(expected_text)), ('text-content', 1),
        ('formatted-scan', 3), ('scan-values', 1), ('fgetpos', 0),
        ('fsetpos', 0), ('text-close', 0), ('text-reopen', 1),
        ('text-reopened-read', len(expected_text)), ('text-reopened-content', 1),
        ('text-reclose', 0), ('exclusive-collision', 1), ('temporary-create', 1),
        ('temporary-close', 0), ('rename-existing', 17), ('remove', 0),
        ('binary-stat', 0), ('binary-length', 4097), ('text-stat', 0),
        ('text-length', len(expected_text))]
    wanted = f'GUESTIO1 phase=stdio run={run} bytes=4097\n'
    for index, (name, value) in enumerate(expected_rows, 1):
        wanted += f'OP {index} {name} expected={value} observed={value}\n'
    wanted += f'GUESTIO1 complete phase=stdio run={run} ops={len(expected_rows)} bytes=4097\nEXIT-FLUSH'
    if text != wanted: raise ValueError('incorrect/incomplete native stdio transcript or exit flush')
    _, completion = find(image, '/temp/stdio.exit')
    if payload(image, completion) != f'child-exit-0-stdio-{run}\n'.encode():
        raise ValueError('stdio process completion evidence is missing')
    return {'binary_identifier': binary_id, 'text_identifier': text_id,
            'binary_sha256': hashlib.sha256(binary).hexdigest(),
            'text_sha256': hashlib.sha256(expected_text).hexdigest(),
            'operations': len(expected_rows), 'exit_flush': True,
            'disk_sha256': digest(disk)}


def validate_time_transcript(transcript, run, host_start, host_end,
                            virtual_clock_bound):
    match = re.fullmatch(
        r'TIME1 unix=(-?\d+) nanoseconds=(\d+) '
        r'date=(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d \+0000 UTC) '
        r'busy_cpu=(\d+) sleep_cpu=(\d+) sleep_elapsed_ms=(\d+) '
        r'wall_delta_s=(\d+) wall_elapsed_ms=(\d+) '
        r'wall_start_unix=(-?\d+) later_unix=(-?\d+) '
        r'later_date=(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\n'
        r'TIME1 complete\n?', transcript)
    if not match:
        raise ValueError('missing or malformed native time transcript')
    unix_seconds, nanoseconds, date_text = int(match[1]), int(match[2]), match[3]
    busy_cpu, sleep_cpu = int(match[4]), int(match[5])
    sleep_elapsed, wall_delta = int(match[6]), int(match[7])
    wall_elapsed, wall_start_unix = int(match[8]), int(match[9])
    later_unix, later_date = int(match[10]), match[11]
    if (nanoseconds != 0 or unix_seconds < host_start - 5 or
            unix_seconds > host_end + virtual_clock_bound or
            not host_start - 5 <= wall_start_unix <= host_end + virtual_clock_bound or
            not host_start - 5 <= later_unix <= host_end + virtual_clock_bound):
        raise ValueError('guest UTC time is outside the trusted host/QEMU interval')
    expected_date = datetime.fromtimestamp(unix_seconds, timezone.utc).strftime(
        '%Y-%m-%d %H:%M:%S +0000 UTC')
    if date_text != expected_date:
        raise ValueError('native UTC calendar does not match Unix seconds')
    if (busy_cpu <= 0 or sleep_cpu < busy_cpu or sleep_elapsed < 15 or
            wall_elapsed < 1000 or wall_delta < 1 or
            wall_delta > wall_elapsed // 1000 + 1):
        raise ValueError('process CPU, monotonic or wall-clock progression check failed')
    expected_later = datetime.fromtimestamp(later_unix, timezone.utc).strftime(
        '%Y-%m-%d %H:%M:%S')
    if (later_date != expected_later or later_unix - wall_start_unix != wall_delta):
        raise ValueError('later UTC calendar does not match elapsed wall time')
    return {'unix_seconds': unix_seconds, 'nanoseconds': nanoseconds,
            'process_cpu_ticks_busy': busy_cpu,
            'process_cpu_ticks_after_sleep': sleep_cpu,
            'sleep_elapsed_ms': sleep_elapsed, 'wall_delta_seconds': wall_delta,
            'wall_elapsed_ms': wall_elapsed}


def inspect_time(folder, disk, build, run, host_start, host_end,
                 virtual_clock_bound):
    with disk.open('rb') as source:
        source.seek(ROOT_OFFSET); image = source.read(PART_BYTES)
    root = folder / 'time-root.img'; root.write_bytes(image)
    check_image(build, root, folder / 'time-checker.log')
    _, transcript_record = find(image, '/temp/time.log')
    transcript = payload(image, transcript_record).decode('ascii')
    (folder / 'time-guest.log').write_text(transcript)
    evidence = validate_time_transcript(transcript, run, host_start, host_end,
                                        virtual_clock_bound)
    _, completion = find(image, '/temp/time.exit')
    if payload(image, completion) != f'child-exit-0-time-{run}\n'.encode():
        raise ValueError('native time child completion evidence is missing')
    evidence['disk_sha256'] = digest(disk)
    evidence['guest_minus_host_end_seconds'] = evidence['unix_seconds'] - host_end
    evidence['virtual_clock_bound_seconds'] = virtual_clock_bound
    evidence['child_completion'] = True
    return evidence


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
    parser.add_argument('--relative-launch',action='store_true',help='launch an isolated ./fstest-relative from /temp through Shoot and native chdir')
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
    report = {'status':'FAIL','accel':args.accel,'relative_launch':args.relative_launch,'runs':[]}
    try:
        for _ in range(args.repetitions):
            run=args.run_id or secrets.token_hex(8)
            folder=args.output/run; folder.mkdir()
            result={'run':run}; report['runs'].append(result)
            disk=create_disk(folder,args.build_dir,run,args.relative_launch)
            result['initial_sha256']=digest(disk)
            for phase in ('create','verify','stdio','time'):
                result['active_phase']=phase
                host_start = int(time.time()) if phase == 'time' else None
                elapsed=boot(folder,disk,phase,args)
                host_end = int(time.time()) if phase == 'time' else None
                if phase == 'stdio':
                    result[phase] = inspect_stdio(folder,disk,args.build_dir,run)
                elif phase == 'time':
                    result[phase] = inspect_time(folder,disk,args.build_dir,run,
                                                 host_start,host_end,args.timeout)
                else:
                    result[phase] = inspect(folder,disk,phase,args.build_dir,run)
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
