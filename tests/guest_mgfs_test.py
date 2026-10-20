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
sys.path.insert(0, str(ROOT / 'tests'))
import libm_accuracy_test as math_oracle
import libm_complex_accuracy_test as complex_oracle

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


def install_qemu_network_config(image):
    """Use a deterministic address on QEMU user networking for RX tests."""
    view = layout(image)
    image_tool.ensure_network_config(image, view)
    root_id = image_tool.u64(image, 88)
    conf_id = image_tool.directory_find(
        payload(image, record(image, root_id)), 'conf')
    network_id = image_tool.directory_find(
        payload(image, record(image, conf_id)), 'network')
    config_id = image_tool.directory_find(
        payload(image, record(image, network_id)), 'config')
    if not conf_id or not network_id or not config_id:
        raise ValueError('could not locate /conf/network/config')
    configuration = (b'mode=manual\n'
                     b'address=10.0.2.15/24\n'
                     b'gateway=10.0.2.2\n'
                     b'dns=10.0.2.3\n')
    table_index = image_tool.replace_file_payload(
        image, view, config_id, configuration, set())
    image_tool.refresh_table_checksum(image, view, table_index)
    image_tool.refresh_metadata_checksums(image, view)


def create_disk(folder, build, run, relative_launch=False,
                network_receive_smoke=False):
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
    if network_receive_smoke:
        install_qemu_network_config(image)
    install(image, 2, 'fstest', (build / 'fstest/fstest.elf').read_bytes())
    if relative_launch:
        install(image,6,'fstest-relative',(build / 'fstest/fstest.elf').read_bytes())
    phases = ('create', 'verify', 'negative', 'stdio', 'time', 'math')
    if network_receive_smoke:
        phases += ('network',)
    for phase in phases:
        prefix = "cd /temp\n" if relative_launch else ""
        executable = "./fstest-relative" if relative_launch else "/bin/fstest"
        if phase == 'time':
            launch_command = f'{executable} time'
        elif phase == 'network':
            launch_command = f'{executable} network'
        elif phase == 'math':
            launch_command = f'{executable} math'
        elif phase == 'stdio':
            launch_command = f'{executable} stdio {run}'
        else:
            launch_command = f'{executable} {"verify" if phase == "negative" else phase} {run}'
        network_commands = ''
        if network_receive_smoke and phase == 'network':
            network_commands = '/bin/netinfo interfaces > /temp/interfaces.log\n'
        network_wait = ('/bin/wait 10\n'
                        if network_receive_smoke and phase == 'network' else '')
        script = (prefix + network_wait +
                  f'{launch_command} > /temp/{phase}.log\n' +
                  network_commands +
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


def qemu_cpu_count(args):
    count = getattr(args, 'cpus', 1)
    if count not in (1, 2):
        raise ValueError('QEMU vCPU count must be 1 or 2')
    return count


def qemu_network_arguments(args):
    # Keep the production kernel image unchanged while allowing a controlled
    # guest run without QEMU's virtual NIC. This isolates non-network SMP
    # workloads from device-specific receive failures.
    if getattr(args, 'no_nic', False):
        return []
    return ['-netdev', 'user,id=net0', '-device', 'e1000,netdev=net0']


def parse_smp_boot_log(text, requested_cpus):
    matches = re.findall(
        r'\[SMP\] final discovered=(\d+) online=(\d+)', text)
    if not matches:
        # A genuine single-CPU boot takes smp_start()'s early return and emits
        # this record instead of the multi-CPU final summary. Do not accept it
        # for a two-CPU request: that would hide a failed AP startup.
        single_cpu = re.findall(
            r'\[SMP\] discovered=(\d+) online=(\d+)', text)
        if requested_cpus != 1 or not single_cpu:
            raise ValueError('kernel boot log has no final SMP online record')
        discovered, online = map(int, single_cpu[-1])
        if discovered != 1 or online != 1:
            raise ValueError('single-CPU boot record does not prove one online CPU')
    else:
        discovered, online = map(int, matches[-1])
    if discovered < requested_cpus or online < requested_cpus:
        raise ValueError(
            f'guest online CPUs {online}/{discovered}, requested {requested_cpus}')
    return {'discovered_cpus': discovered, 'online_cpus': online}


def inspect_smp_boot(disk, requested_cpus):
    with disk.open('rb') as source:
        source.seek(ROOT_OFFSET)
        image = source.read(PART_BYTES)
    _, boot_log = find(image, '/sys/logs/boot.log')
    return parse_smp_boot_log(payload(image, boot_log).decode('ascii'),
                              requested_cpus)


def boot(folder, disk, phase, args):
    part = folder / phase
    part.mkdir()
    shutil.copyfile(args.ovmf_vars, part / 'vars.fd')
    invocation = [args.qemu, '-machine', 'q35', '-accel', args.accel,
                  '-cpu', 'max' if args.accel == 'tcg' else 'host',
                  '-smp', str(qemu_cpu_count(args)), '-m', '512M',
                  '-rtc', 'base=utc,clock=vm', '-display', 'none', '-serial', f'file:{part}/serial.log',
                  '-qmp', 'stdio',
                  '-drive', f'if=pflash,format=raw,readonly=on,file={args.ovmf_code}',
                  '-drive', f'if=pflash,format=raw,file={part}/vars.fd',
                  '-drive', f'id=usb,file={disk},format=raw,if=none',
                  '-device', 'qemu-xhci,id=xhci',
                  '-device', 'usb-storage,id=boot-storage,bus=xhci.0,port=2,drive=usb,bootindex=1',
                  '-device', 'usb-kbd,id=keyboard,bus=xhci.0,port=1', '-no-reboot', '-no-shutdown']
    invocation += qemu_network_arguments(args)
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
    child_status = 0
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
                failed_child = re.search(r'exited with status (\d+)\.', text)
                if launched and failed_child:
                    child_status = int(failed_child.group(1))
                    if not expected_failure and phase != 'network':
                        raise RuntimeError(
                            'native launch, exit or wait failure; '
                            f'child status {child_status}')
                    if expected_failure and child_status != 43:
                        raise RuntimeError(
                            f'negative control exited with status {child_status}, expected 43')
                    negative_exit = expected_failure
                    (part / 'blockstats.json').write_text(json.dumps(
                        qmp.call('query-blockstats'), indent=2))
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
    if not expected_failure and child_status == 0 and not any(e.get('event') == 'SHUTDOWN' and
            e.get('data',{}).get('guest') is True for e in qmp.events):
        raise RuntimeError('no guest-initiated shutdown evidence')
    if answers: raise RuntimeError('missing trusted authorization prompts')
    if pending_answer is not None: raise RuntimeError('unconfirmed authorization input')
    if hotplug_run and len(hotplug_actions) != 2: raise RuntimeError('incomplete hotplug handshakes')
    stats=(part/'blockstats.json')
    if not stats.exists(): raise RuntimeError('no stopped-guest block accounting')
    return {'seconds':time.monotonic()-started,'launch_echoed_characters':launch_sent,'child_status':43 if expected_failure else child_status,
            'hotplug_actions':hotplug_actions,
            'authorization_decisions':confirmed,
            'qemu_blockstats':json.loads(stats.read_text())}


def expected_payload(run):
    return bytes((i * 37 + (i >> 8) * 11 + ord(run[i % 16])) & 255 for i in range(SIZE))


def expected_operations(phase, run, identifier):
    # Independent contract, including exact operation names and return values.
    rows=[('deviced-running',1),('path-at-byte-limit',0),
          ('path-at-limit-root',2),('path-over-byte-limit',-4),
          ('malformed-utf8-path',-4)]
    if phase == 'create':
        rows += [('directory-absent',-2),('directory-create-trailing',0),
                 ('directory-trailing-stat',0),('directory-trailing-type',2),
                 ('directory-remove-trailing',0),('directory-removed',-2),
                 ('file-create-trailing',-5),('file-create-trailing-stat',-2),
                 ('absent',-2),('create',0)]
    rows += [('file-trailing-stat',-5),('file-trailing-open',-5),
             ('file-trailing-remove',-5)]
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
            'operations': len(expected_operations(phase, run, identifier)),
            'disk_sha256': digest(disk)}


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
                            virtual_clock_bound, network_receive_smoke=False):
    network_start_lines = re.findall(r'(?m)^NET1 begin$', transcript)
    if ((network_receive_smoke and len(network_start_lines) != 1) or
            (not network_receive_smoke and network_start_lines)):
        raise ValueError('network phase marker does not match selected test mode')
    if network_start_lines:
        transcript = transcript.replace('NET1 begin\n', '', 1)
    network_lines = re.findall(r'(?m)^NET1 .*$', transcript)
    if ((network_receive_smoke and len(network_lines) != 1) or
            (not network_receive_smoke and network_lines)):
        raise ValueError('network evidence does not match selected test mode')
    if network_lines:
        transcript = transcript.replace(network_lines[0] + '\n', '', 1)
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
                 virtual_clock_bound, network_receive_smoke=False):
    with disk.open('rb') as source:
        source.seek(ROOT_OFFSET); image = source.read(PART_BYTES)
    root = folder / 'time-root.img'; root.write_bytes(image)
    check_image(build, root, folder / 'time-checker.log')
    _, transcript_record = find(image, '/temp/time.log')
    transcript = payload(image, transcript_record).decode('ascii')
    (folder / 'time-guest.log').write_text(transcript)
    evidence = validate_time_transcript(transcript, run, host_start, host_end,
                                        virtual_clock_bound,
                                        network_receive_smoke)
    _, completion = find(image, '/temp/time.exit')
    if payload(image, completion) != f'child-exit-0-time-{run}\n'.encode():
        raise ValueError('native time child completion evidence is missing')
    evidence['disk_sha256'] = digest(disk)
    evidence['guest_minus_host_end_seconds'] = evidence['unix_seconds'] - host_end
    evidence['virtual_clock_bound_seconds'] = virtual_clock_bound
    evidence['child_completion'] = True
    if network_receive_smoke:
        _, interfaces_record = find(image, '/temp/network.log')
        interfaces_text = payload(image, interfaces_record).decode(
            'ascii', errors='strict')
        evidence['network_receive'] = validate_network_receive(
            transcript, interfaces_text)
    return evidence


def inspect_network(folder, disk, build, run, child_status):
    with disk.open('rb') as source:
        source.seek(ROOT_OFFSET); image = source.read(PART_BYTES)
    root = folder / 'network-root.img'; root.write_bytes(image)
    check_image(build, root, folder / 'network-checker.log')
    try:
        _, transcript_record = find(image, '/temp/network.log')
        transcript = payload(image, transcript_record).decode('ascii', errors='strict')
        (folder / 'network-guest.log').write_text(transcript)
    except ValueError:
        if child_status == 0:
            raise
        transcript = ''
    evidence = {'child_status': child_status, 'transcript': transcript,
                'checker': 'clean', 'disk_sha256': digest(disk)}
    if child_status != 0:
        try:
            _, interfaces_record = find(image, '/temp/interfaces.log')
            interfaces = payload(image, interfaces_record).decode('ascii', errors='strict')
            evidence['interfaces_transcript'] = interfaces
        except ValueError:
            evidence['interfaces_transcript'] = None
        return evidence
    _, interfaces_record = find(image, '/temp/interfaces.log')
    interfaces = payload(image, interfaces_record).decode('ascii', errors='strict')
    _, completion = find(image, '/temp/network.exit')
    if payload(image, completion) != f'child-exit-0-network-{run}\n'.encode():
        raise ValueError('network child completion evidence is missing')
    evidence['network_receive'] = validate_network_transcript(
        transcript, interfaces)
    evidence['interfaces_transcript'] = interfaces
    evidence['child_completion'] = True
    return evidence


def validate_network_receive(ping_text, interfaces_text):
    match = re.search(
        r'(?m)^NET1 source=(\d+\.\d+\.\d+\.\d+) reply_bytes=(\d+) '
        r'echoes=(\d+) '
        r'rx_before=(\d+) rx_after=(\d+) tx_before=(\d+) tx_after=(\d+)$',
        ping_text)
    if not match:
        raise ValueError('native ICMP echo evidence is missing or malformed')
    reply_bytes, echoes, rx_before, rx_after, tx_before, tx_after = map(
        int, match.groups()[1:])
    if (match.group(1) != '10.0.2.2' or reply_bytes == 0 or echoes != 16 or
            rx_after < rx_before or rx_after - rx_before < echoes or
            tx_after < tx_before or tx_after - tx_before < echoes):
        raise ValueError('native ICMP burst or RX/TX counter check failed')
    receive_counts = re.findall(
        r'(?m)^\s*eth\d+\s+ethernet\s+up\s+\S+\s+(\d+)\s+\d+\s*$',
        interfaces_text)
    received = sum(int(count) for count in receive_counts)
    if received < echoes:
        raise ValueError('native network RX counter is below the echo count')
    return {'gateway_echo_replies': echoes, 'kernel_rx_packets': received,
            'native_transcript': match.group(0),
            'interfaces_transcript': interfaces_text}


def validate_network_transcript(transcript, interfaces_text):
    lines = transcript.splitlines()
    if (len(lines) != 3 or lines[0] != 'NET1 begin' or
            lines[2] != 'NET1 complete'):
        raise ValueError('native network phase transcript is incomplete or malformed')
    return validate_network_receive(lines[1], interfaces_text)


def network_receive_validator_negative_controls():
    valid = ('NET1 source=10.0.2.2 reply_bytes=36 echoes=16 '
             'rx_before=0 rx_after=17 tx_before=1 tx_after=17')
    transcript = 'NET1 begin\n' + valid + '\nNET1 complete\n'
    interfaces = ('NAME  TYPE      STATE  IPV4       RX  TX\n'
                  'eth0  ethernet  up     10.0.2.15   17  17\n')
    if validate_network_transcript(transcript, interfaces)['gateway_echo_replies'] != 16:
        raise ValueError('network validator accepted the wrong positive count')

    invalid = (
        ('missing transcript', '', interfaces),
        ('missing start marker', transcript.replace('NET1 begin\n', ''), interfaces),
        ('missing completion marker', transcript.replace('NET1 complete\n', ''), interfaces),
        ('extra transcript line', transcript + 'PASS\n', interfaces),
        ('failure transcript', transcript.replace(valid, 'NET1 failure stage=echo'), interfaces),
        ('wrong gateway', valid.replace('10.0.2.2', '10.0.2.3'), interfaces),
        ('short burst', valid.replace('echoes=16', 'echoes=15'), interfaces),
        ('short RX delta', valid.replace('rx_after=17', 'rx_after=15'), interfaces),
        ('short TX delta', valid.replace('tx_after=17', 'tx_after=15'), interfaces),
        ('missing interface RX', valid, interfaces.replace('17  17', '0  17')),
    )
    for name, transcript, interface_dump in invalid:
        try:
            if name in ('wrong gateway', 'short burst', 'short RX delta',
                        'short TX delta', 'missing interface RX'):
                validate_network_receive(transcript, interface_dump)
            else:
                validate_network_transcript(transcript, interface_dump)
        except ValueError:
            continue
        raise ValueError('network validator accepted negative control: ' + name)
    return [name for name, _, _ in invalid]


def inspect_math(folder, disk, build, run):
    with disk.open('rb') as source:
        source.seek(ROOT_OFFSET); image = source.read(PART_BYTES)
    root = folder / 'math-root.img'; root.write_bytes(image)
    check_image(build, root, folder / 'math-checker.log')
    _, transcript_record = find(image, '/temp/math.log')
    transcript = payload(image, transcript_record).decode('ascii')
    (folder / 'math-guest.log').write_text(transcript)
    lines = transcript.splitlines()
    expected_prefix = ['MATH1 abi=16 mant=64 max_exp=16384']
    expected_witness = ['NEXT1 f=1 d=1 l=1 flags=48/48/48']
    expected_ilogb = ['ILOGB1 f=1 d=1 l=1 flags=1/1/1']
    expected_suffix = [
        'MATH4 complete records=90 fenv=syscall-context exceptions=verified']
    if (len(lines) != 94 or
            not lines[0].startswith(expected_prefix[0] + ' x87cw=') or
            lines[91:92] != expected_witness or lines[92:93] != expected_ilogb or
            lines[-1:] != expected_suffix):
        raise ValueError('native math transcript is missing, incomplete or malformed')
    try:
        x87_control = int(lines[0].split('x87cw=')[1], 16)
    except (IndexError, ValueError) as error:
        raise ValueError('native x87 control-word evidence is malformed') from error
    if x87_control & 0x0300 != 0x0300:
        raise ValueError(f'native x87 is not using extended precision: 0x{x87_control:04x}')
    if not lines[1].startswith('F sqrt ') or not lines[2].startswith('D sqrt ') or \
            not lines[3].startswith('L sqrt ') or \
            [line.split()[1] for line in lines[4:13]] != \
            ['scalbn', 'fmod', 'remainder', 'remquo', 'scalbn',
             'cbrt', 'hypot', 'cbrt', 'hypot']:
        raise ValueError('native math transcript has unexpected operation ordering')
    expected_operations = [
        ('F', 'exp'), ('D', 'exp'), ('L', 'exp'),
        ('F', 'exp2'), ('D', 'exp2'), ('L', 'exp2'),
        ('F', 'expm1'), ('D', 'expm1'), ('L', 'expm1'),
        ('F', 'log'), ('D', 'log'), ('L', 'log'),
        ('F', 'log2'), ('D', 'log2'), ('L', 'log2'),
        ('F', 'log10'), ('D', 'log10'), ('L', 'log10'),
        ('F', 'log1p'), ('D', 'log1p'), ('L', 'log1p')]
    if [(line.split()[0], line.split()[1]) for line in lines[13:34]] != \
            expected_operations:
        raise ValueError('native transcendental result records are incomplete or reordered')
    expected_hyperbolic = [
        ('F', 'sinh'), ('F', 'asinh'), ('D', 'cosh'), ('D', 'acosh'),
        ('L', 'tanh'), ('L', 'atanh')]
    if [(line.split()[0], line.split()[1]) for line in lines[34:40]] != \
            expected_hyperbolic:
        raise ValueError('native hyperbolic result records are incomplete or reordered')
    expected_inverse = [
        ('F', 'atan'), ('D', 'atan'), ('L', 'atan'),
        ('F', 'atan2'), ('D', 'atan2'), ('L', 'atan2'),
        ('F', 'asin'), ('D', 'asin'), ('L', 'asin'),
        ('F', 'acos'), ('D', 'acos'), ('L', 'acos')]
    if [(line.split()[0], line.split()[1]) for line in lines[40:52]] != \
            expected_inverse:
        raise ValueError('native inverse-trigonometric records are incomplete or reordered')
    expected_trigonometric = [
        ('F', 'sin'), ('D', 'sin'), ('L', 'sin'),
        ('F', 'cos'), ('D', 'cos'), ('L', 'cos'),
        ('F', 'tan'), ('D', 'tan'), ('L', 'tan')]
    if [(line.split()[0], line.split()[1]) for line in lines[52:61]] != \
            expected_trigonometric:
        raise ValueError('native trigonometric records are incomplete or reordered')
    if [(line.split()[0], line.split()[1]) for line in lines[61:64]] != \
            [('F', 'pow'), ('D', 'pow'), ('L', 'pow')]:
        raise ValueError('native power records are incomplete or reordered')
    if [(line.split()[0], line.split()[1]) for line in lines[64:67]] != \
            [('F', 'erf'), ('D', 'erfc'), ('L', 'erf')]:
        raise ValueError('native error-function records are incomplete or reordered')
    if [(line.split()[0], line.split()[1]) for line in lines[67:70]] != \
            [('F', 'fma'), ('D', 'fma'), ('L', 'fma')]:
        raise ValueError('native fused-multiply-add records are incomplete or reordered')
    oracle = math_oracle.Mpfr()
    for line in lines[1:70]:
        fields = line.split()
        error = math_oracle.check_row(oracle, fields)
        if error:
            raise ValueError('native numerical result failed MPFR check: ' + error)
    for line in lines[70:76]:
        error = math_oracle.check_row(oracle, line.split())
        if error:
            raise ValueError('native gamma result failed MPFR check: ' + error)
    expected_gamma = [('F', 'tgamma'), ('D', 'tgamma'), ('L', 'tgamma'),
                      ('F', 'lgamma'), ('D', 'lgamma'), ('L', 'lgamma')]
    if [(line.split()[0], line.split()[1]) for line in lines[70:76]] != expected_gamma:
        raise ValueError('native gamma records are incomplete or reordered')
    expected_complex = [(kind, operation) for operation in
                        ('csqrt', 'cexp', 'cpow', 'catanh', 'cacosh')
                        for kind in ('F', 'D', 'L')]
    if [(line.split()[0], line.split()[1]) for line in lines[76:91]] != expected_complex:
        raise ValueError('native complex records are incomplete or reordered')
    complex_reference = complex_oracle.Oracle()
    for line in lines[76:91]:
        error = complex_oracle.check_guest_record(complex_reference, line.split())
        if error:
            raise ValueError('native complex result failed MPC check: ' + error)
    _, completion = find(image, '/temp/math.exit')
    if payload(image, completion) != f'child-exit-0-math-{run}\n'.encode():
        raise ValueError('native math child completion evidence is missing')
    return {'operations': 90, 'x87_long_double': True,
            'x87_control_word': f'0x{x87_control:04x}',
            'fenv_across_syscall_and_schedule': True,
            'nextafter_zero_underflow_inexact_all_precisions': True,
            'ilogb_invalid_flag_all_precisions': True,
            'single_round_fma': True, 'error_functions': True,
            'gamma_records': 6, 'complex_records': 15,
            'mpfr_checked_records': 75, 'mpc_checked_records': 15,
            'disk_sha256': digest(disk)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--accel', choices=('tcg', 'kvm'), default='tcg')
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--ovmf-code', type=Path, default=Path('/usr/share/OVMF/OVMF_CODE_4M.fd'))
    parser.add_argument('--ovmf-vars', type=Path, default=Path('/usr/share/OVMF/OVMF_VARS_4M.fd'))
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--cpus', type=int, choices=(1, 2), default=1,
                        help='QEMU vCPU count (the two-CPU option exercises AP startup)')
    parser.add_argument('--no-nic', action='store_true',
                        help='omit QEMU NIC for non-network SMP qualification; kernel unchanged')
    parser.add_argument('--network-receive-smoke', action='store_true',
                        help='ping the QEMU gateway and require persisted native RX counters')
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
    if args.network_receive_smoke and args.no_nic:
        parser.error('--network-receive-smoke requires the QEMU NIC')
    if args.run_id and (len(args.run_id)!=16 or any(c not in '0123456789abcdef' for c in args.run_id) or args.repetitions!=1):
        parser.error('--run-id requires 16 lowercase hexadecimal characters and one repetition')
    report = {'status':'FAIL','accel':args.accel,'cpus':args.cpus,
              'no_nic':args.no_nic,'relative_launch':args.relative_launch,
              'runs':[]}
    try:
        if args.network_receive_smoke and args.negative_control:
            report['network_evidence_negative_controls'] = \
                network_receive_validator_negative_controls()
        for _ in range(args.repetitions):
            run=args.run_id or secrets.token_hex(8)
            folder=args.output/run; folder.mkdir()
            result={'run':run}; report['runs'].append(result)
            disk=create_disk(folder,args.build_dir,run,args.relative_launch,
                             args.network_receive_smoke)
            result['initial_sha256']=digest(disk)
            phases = (('create','verify','stdio','math','network')
                      if args.network_receive_smoke else
                      ('create','verify','stdio','time','math'))
            for phase in phases:
                result['active_phase']=phase
                host_start = int(time.time()) if phase == 'time' else None
                elapsed=boot(folder,disk,phase,args)
                host_end = int(time.time()) if phase == 'time' else None
                phase_error = None
                if phase == 'stdio':
                    result[phase] = inspect_stdio(folder,disk,args.build_dir,run)
                elif phase == 'time':
                    result[phase] = inspect_time(folder,disk,args.build_dir,run,
                                                 host_start,host_end,args.timeout,
                                                 False)
                elif phase == 'math':
                    result[phase] = inspect_math(folder,disk,args.build_dir,run)
                elif phase == 'network':
                    result[phase] = inspect_network(folder,disk,args.build_dir,
                                                    run,elapsed['child_status'])
                    if elapsed['child_status'] != 0:
                        phase_error = ('native network child failed with status '
                                       f"{elapsed['child_status']}")
                else:
                    result[phase] = inspect(folder,disk,phase,args.build_dir,run)
                result[phase]['smp'] = inspect_smp_boot(disk, args.cpus)
                result[phase].update(elapsed)
                if phase_error:
                    raise RuntimeError(phase_error)
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
