#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded graphical Reed navigation capture; never opens developer media."""
import argparse
import bisect
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tests/guest'), str(ROOT / 'tests/shared')]
import artifact_lifecycle as artifacts
import guest_mgfs_test as guest
import guest_secondary_test as secondary
import qemu_accel

NAV = {35: ('h', 0x0b), 36: ('j', 0x0d), 37: ('k', 0x0e), 38: ('l', 0x0f)}
KINDS = {1: 'hid', 2: 'repeat', 3: 'queue', 4: 'read', 5: 'cancel',
         6: 'present', 7: 'batch', 8: 'flush_begin', 9: 'flush_end',
         10: 'usb_irq', 11: 'usb_work'}
HEADER = struct.Struct('<8Q')
EVENT = struct.Struct('<QQIIII')
LINUX_EVENT = struct.Struct('@llHHi')


def decode_trace(data):
    magic, count, dropped, physical, period, mask, capacity, size = HEADER.unpack_from(data)
    if (magic != 0x3145434152544E49 or size != EVENT.size or
            capacity != 16384 or count > capacity or not period or not physical):
        raise ValueError('invalid or uninitialized guest trace header')
    if len(data) < HEADER.size + count * size:
        raise ValueError('truncated guest trace')
    events = []
    for index in range(count):
        counter, uptime, kind, key, first, second = EVENT.unpack_from(data, HEADER.size + index * size)
        if kind not in KINDS:
            raise ValueError('unknown guest trace event')
        events.append(dict(counter=counter, uptime_ms=uptime, kind=KINDS[kind],
                           key=key, first=first, second=second))
    return dict(dropped=dropped, physical=physical, period_fs=period,
                mask=mask, events=events)


def counter_time(counter, samples, period_fs):
    """Interpolate bracketed HPET reads; return time AND measurement uncertainty."""
    ordered = sorted(samples, key=lambda s: s['counter'])
    if not ordered:
        raise ValueError('no guest/host clock calibration')
    index = bisect.bisect_right([s['counter'] for s in ordered], counter)
    if 0 < index < len(ordered):
        left, right = ordered[index - 1], ordered[index]
        span = right['counter'] - left['counter']
        fraction = (counter - left['counter']) / span
        center = lambda s: (s['before_ns'] + s['after_ns']) / 2
        value = center(left) + fraction * (center(right) - center(left))
        error = max(s['after_ns'] - s['before_ns'] for s in (left, right)) / 2
        # A stopped/slowed VM clock cannot be compared as a host wall clock.
        expected = span * period_fs / 1e6
        error += abs((center(right) - center(left)) - expected)
        return value, error
    nearest = ordered[0 if index == 0 else -1]
    return ((nearest['before_ns'] + nearest['after_ns']) / 2 +
            (counter - nearest['counter']) * period_fs / 1e6,
            (nearest['after_ns'] - nearest['before_ns']) / 2)


def analyze(trace, samples, physical, qemu_log, start_ns, end_ns):
    events = []
    for event in trace['events']:
        host, error = counter_time(event['counter'], samples, trace['period_fs'])
        events.append(dict(event, host_ns=host, uncertainty_ns=error))
    qemu = []
    for stamp, key, down in re.findall(
            r'\d+@([0-9.]+):input_event_key_qcode con -?\d+, key qcode ([hjkl]), down ([01])', qemu_log):
        real = float(stamp) * 1e9
        sample = min(samples, key=lambda s: abs(s['realtime_ns'] - real))
        mono = real + sample['before_ns'] - sample['realtime_ns']
        if start_ns <= mono <= end_ns:
            qemu.append(dict(host_ns=mono, key=key, down=int(down)))
    rows = []
    pressed = {}
    for item in physical:
        identity = (item['device'], item['code'])
        if item['value'] == 1:
            pressed[identity] = item
        if item['value'] != 0 or identity not in pressed:
            continue
        press = pressed.pop(identity)
        key, usb = NAV[item['code']]
        release = item['host_ns']
        next_press = min((e['host_ns'] for e in physical if e['value'] == 1 and
                          e['host_ns'] > release), default=end_ns)
        window = [e for e in events if release <= e['host_ns'] < next_press]
        ups = [e for e in qemu if e['key'] == key and not e['down'] and
               press['host_ns'] <= e['host_ns'] < next_press]
        up = min(ups, key=lambda e: abs(e['host_ns'] - release)) if ups else None
        hid = [e for e in events if e['kind'] == 'hid' and
               not e['first'] & (1 << (usb - 0x0b)) and
               (up['host_ns'] if up else release) - e['uncertainty_ns'] <= e['host_ns'] < next_press]
        hid_release = hid[0] if hid else None
        after_hid = [e for e in window if hid_release and
                     e['host_ns'] > hid_release['host_ns'] + e['uncertainty_ns']]
        presents = [e for e in events if e['kind'] == 'present' and e['host_ns'] < next_press]
        changes = [b for a, b in zip(presents, presents[1:]) if
                   (a['first'], a['second']) != (b['first'], b['second']) and
                   b['host_ns'] >= release]
        rows.append(dict(key=key, device=item['device'], hold_ms=(release - press['host_ns']) / 1e6,
            physical_to_qemu_up_ms=(up['host_ns'] - release) / 1e6 if up else None,
            qemu_to_hid_release_ms=(hid_release['host_ns'] - up['host_ns']) / 1e6 if up and hid_release else None,
            hid_timing_uncertainty_ms=hid_release['uncertainty_ns'] / 1e6 if hid_release else None,
            host_autorepeats=sum(e['value'] == 2 and e['device'] == item['device'] and
                e['code'] == item['code'] and press['host_ns'] <= e['host_ns'] < release for e in physical),
            qemu_downs_during_hold=sum(e['key'] == key and e['down'] and
                press['host_ns'] <= e['host_ns'] < release for e in qemu),
            generated_after_hid_release=sum(e['kind'] == 'repeat' and e['key'] == usb for e in after_hid),
            initial_presses_read_after_hid_release=sum(e['kind'] == 'read' and e['key'] == usb and not e['first'] for e in after_hid),
            repeats_read_after_hid_release=sum(e['kind'] == 'read' and e['key'] == usb and e['first'] for e in after_hid),
            last_cursor_change_after_physical_release_ms=(changes[-1]['host_ns'] - release) / 1e6 if changes else 0,
            visible_cursor_changes_after_physical_release=len(changes)))
    flush_starts, flushes = {}, []
    for event in events:
        if not start_ns <= event['host_ns'] <= end_ns:
            continue
        if event['kind'] == 'flush_begin':
            flush_starts[event['key']] = event
        elif event['kind'] == 'flush_end' and event['key'] in flush_starts:
            first = flush_starts.pop(event['key'])
            flushes.append(dict(cpu=event['key'], pixels_high=event['second'],
                milliseconds=(event['counter'] - first['counter']) * trace['period_fs'] / 1e12))
    return dict(holds=rows, guest_events=len(events), lost_guest_events=trace['dropped'],
        framebuffer_flushes=flushes,
        max_raw_queue_depth=max((e['second'] for e in events if e['kind'] == 'queue' and
                                start_ns <= e['host_ns'] <= end_ns), default=0),
        qemu_navigation_events=qemu,
        limitation='Cursor positions measure guest framebuffer presentation, not compositor/monitor scanout. '
                   'Boundary scrolling may move document contents without moving the screen cursor.')


class PhysicalKeys:
    def __init__(self):
        self.devices, self.events, self.denied = {}, [], []
        self.error = None
        self.stopped = threading.Event()
        for path in sorted(Path('/dev/input').glob('event*')):
            fd = None
            try:
                fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
                bits = bytearray(96)
                fcntl.ioctl(fd, 0x80604521, bits)  # EVIOCGBIT(EV_KEY, 96)
                if not all(bits[code // 8] & (1 << (code % 8)) for code in NAV):
                    os.close(fd)
                    continue
                fcntl.ioctl(fd, 0x400445a0, struct.pack('i', 1))  # EVIOCSCLOCKID(CLOCK_MONOTONIC)
                self.devices[fd] = str(path)
            except OSError:
                if fd is not None and fd not in self.devices:
                    os.close(fd)
                self.denied.append(str(path))
        self.worker = threading.Thread(target=self.collect, daemon=True)
        self.worker.start()

    def collect(self):
        try:
            while not self.stopped.wait(.01):
                self.poll()
        except (OSError, ValueError) as error:
            self.error = str(error)

    def poll(self):
        for fd in select.select(list(self.devices), [], [], 0)[0]:
            data = os.read(fd, LINUX_EVENT.size * 128)
            for offset in range(0, len(data), LINUX_EVENT.size):
                sec, usec, kind, code, value = LINUX_EVENT.unpack_from(data, offset)
                if kind == 0 and code == 3:
                    raise ValueError('host input events were dropped; capture is incomplete')
                if kind == 1 and code in NAV:
                    self.events.append(dict(device=self.devices[fd], code=code,
                        value=value, host_ns=sec * 10**9 + usec * 1000))
            if len(self.events) > 8192:
                raise ValueError('physical navigation-event bound exceeded')

    def close(self):
        self.stopped.set()
        self.worker.join(timeout=2)
        for fd in self.devices:
            os.close(fd)


def memory(qmp, folder, address, count, physical=False):
    path = folder / 'memory.bin'
    command = f'{"pmemsave" if physical else "memsave"} {address:#x} {count} "{path}"'
    reply = qmp.call('human-monitor-command', {'command-line': command})
    if reply.strip():
        raise RuntimeError('QEMU memory capture: ' + reply)
    return path.read_bytes()


def screen(qmp, folder, name='screen'):
    path = folder / (name + '.ppm')
    qmp.call('screendump', {'filename': str(path)})
    text = guest.screen_text(path)
    (folder / (name + '.txt')).write_text(text)
    return text


def type_text(qmp, text):
    for character in text:
        tap(qmp, character)


def tap(qmp, character):
    code = {'/': 'slash', '.': 'dot', ' ': 'spc', '-': 'minus',
            '\n': 'ret', '\x1b': 'esc'}.get(character, character)
    for down in (True, False):
        qmp.call('input-send-event', {'events': [{'type': 'key', 'data': {
            'down': down, 'key': {'type': 'qcode', 'data': code}}}]})
        time.sleep(.03)


def replay_plan(path, start):
    report = json.loads(path.read_text())
    physical = json.loads((path.parent / 'physical-events.json').read_text())
    presses, holds = {}, []
    for event in physical:
        identity = (event['device'], event['code'])
        if event['value'] == 1:
            presses[identity] = event['host_ns']
        elif event['value'] == 0 and identity in presses:
            first = presses.pop(identity)
            if event['host_ns'] - first >= 500000000:
                holds.append((first, event['host_ns']))
    if len(holds) < 2:
        raise ValueError('replay requires two captured physical holds')
    first, last = holds[-2][0] - 100000000, holds[-1][1] + 2000000000
    if last - first > 30 * 10**9:
        raise ValueError('captured replay exceeds 30 seconds')
    plan = [dict(e, host_ns=int(e['host_ns'] - first + start)) for e in
            report['measurement']['qemu_navigation_events'] if first <= e['host_ns'] <= last]
    reference = [dict(e, host_ns=int(e['host_ns'] - first + start))
                 for e in physical if first <= e['host_ns'] <= last]
    return plan, reference, start + last - first


def complete_physical_capture(measurement):
    matched = {row['key'] for row in measurement['holds'] if row['hold_ms'] >= 400
               and row['qemu_downs_during_hold'] > 0
               and row['physical_to_qemu_up_ms'] is not None
               and row['qemu_to_hid_release_ms'] is not None}
    return matched == set('hjkl') and not measurement['lost_guest_events']


def run(args):
    error = qemu_accel.accel_error('kvm', qemu=args.qemu)
    if error:
        raise RuntimeError(error)
    if not (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
        raise RuntimeError('graphical desktop unavailable; run make diagnose-reed-input in a desktop terminal')
    if shutil.disk_usage(ROOT).free < 2 * 1024**3:
        raise RuntimeError('physical input diagnostic requires 2 GiB free build storage')
    workspace = artifacts.ArtifactWorkspace(name='reed-physical-input')
    folder, process, devices, qmp, trace = workspace.path, None, None, None, None
    address, size = 0, 0
    report = dict(status='INCOMPLETE', source_revision=subprocess.check_output(
        ['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        tracked_diff=subprocess.check_output(['git', 'diff', 'HEAD', '--'], cwd=ROOT, text=True),
        diagnostic_script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        scope='Physical keyboard navigation diagnostic; no claimed production fix')
    build = folder / 'build'
    try:
        with (folder / 'build.log').open('wb') as log:
            builder = subprocess.Popen([args.make, '-j4', f'BUILD_DIR={build}', 'INPUT_TRACE_DIAGNOSTICS=1',
                'RTC_UTC_TRUST=1', 'binaries', 'fstest', 'mkmgfs', 'mgfsck'], cwd=ROOT,
                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                if builder.wait(timeout=300):
                    raise RuntimeError('diagnostic build failed; see build.log')
            except BaseException:
                # Compiler children share this owned process group. Reap them
                # before the workspace can be removed on timeout/interruption.
                for signum in (signal.SIGTERM, signal.SIGKILL):
                    try:
                        os.killpg(builder.pid, signum)
                    except ProcessLookupError:
                        break
                    try:
                        builder.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        continue
                builder.wait()
                raise
        disk = guest.create_disk(folder, build, '0123456789abcdef')
        image = bytearray((folder / 'initial-root.img').read_bytes())
        secondary.install(image, 6, 'navigation.txt',
            ''.join(f'{line:03d} ' + 'abcdefghijklmnopqrstuvwxyz' * 6 + '\n'
                    for line in range(200)).encode(), owner=1000, permissions=3)
        (folder / 'initial-root.img').write_bytes(image)
        guest.check_image(build, folder / 'initial-root.img', folder / 'initial-check.log')
        with disk.open('r+b') as out:
            out.seek(guest.ROOT_OFFSET); out.write(image)
        platform = list(args.platform)
        if platform and platform[0] == '--':
            platform.pop(0)
        if '-accel' not in platform or platform[platform.index('-accel') + 1] != 'kvm':
            raise ValueError('diagnostic requires make run graphical KVM arguments')
        drives = [platform[i + 1] for i, value in enumerate(platform[:-1]) if value == '-drive']
        if (len(drives) != 3 or sum(v.startswith('id=usb,file=') for v in drives) != 1 or
                sum(v.startswith('if=pflash,format=raw,file=') for v in drives) != 1 or
                sum(v.startswith('if=pflash,format=raw,readonly=on,file=') for v in drives) != 1 or
                any(v in platform for v in ('-blockdev', '-hda', '-hdb', '-hdc', '-hdd', '-sd', '-cdrom')) or
                any('usb-host' in v or 'vfio' in v for v in platform)):
            raise ValueError('diagnostic refuses additional storage or physical-device passthrough')
        for i, argument in enumerate(platform):
            if argument.startswith('id=usb,file='):
                platform[i] = f'id=usb,file={disk},format=raw,if=none'
            elif argument.startswith('if=pflash,format=raw,file='):
                variables = folder / 'vars.fd'
                source = Path(argument.split('file=', 1)[1])
                if not source.is_file():
                    source = Path('/usr/share/OVMF/OVMF_VARS_4M.fd')
                shutil.copyfile(source, variables)
                platform[i] = f'if=pflash,format=raw,file={variables}'
        for path in (disk, folder / 'vars.fd'):
            guest.sync_image_for_boot(path)
        symbols = subprocess.check_output(['nm', '-S', str(build / 'Mangrove/pith.elf')], text=True)
        match = re.search(r'^([0-9a-f]+) ([0-9a-f]+) \w pith_input_trace$', symbols, re.M)
        if not match:
            raise RuntimeError('diagnostic kernel lacks input trace')
        address, size = (int(value, 16) for value in match.groups())
        devices = PhysicalKeys()
        if not devices.devices:
            raise RuntimeError('no readable physical keyboard supports navigation keys; no permission changes attempted')
        report['physical_devices'] = list(devices.devices.values())
        report['unreadable_or_unsupported_input_devices'] = devices.denied
        invocation = [args.qemu, *platform, '-qmp', 'stdio', '-serial', f'file:{folder}/serial.log',
            '-debugcon', f'file:{folder}/debugcon.log', '-msg', 'timestamp=on',
            '-name', 'Mangrove input capture: hold h j k l for one second each; release and pause',
            '-trace', f'events={folder}/trace-events.txt', '-no-reboot', '-no-shutdown']
        (folder / 'trace-events.txt').write_text('input_event_key_qcode\ngd_key_event\nsdl2_process_key\nhid_kbd_queue_full\n')
        (folder / 'command.json').write_text(json.dumps(invocation, indent=2))
        with (folder / 'qemu.log').open('wb') as log:
            process = subprocess.Popen(invocation, cwd=ROOT, stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=log, bufsize=0)
            qmp = guest.QMP(process)
            deadline = time.monotonic() + 90
            while 'developer ~:' not in screen(qmp, folder):
                if time.monotonic() > deadline:
                    raise TimeoutError('guest did not reach Shoot')
                failure = guest.kernel_failure(folder)
                if failure:
                    raise RuntimeError(failure)
                time.sleep(.3)
            type_text(qmp, 'reed /temp/navigation.txt\n')
            while 'NORMAL' not in screen(qmp, folder):
                if 'reed:' in (folder / 'screen.txt').read_text():
                    raise RuntimeError('Reed startup rejected the test file; see screen.txt')
                if time.monotonic() > deadline:
                    raise TimeoutError('Reed did not become interactive')
                time.sleep(.3)
            # Start inside the fixture, clear of first/last line and column boundaries.
            for key, count in (('pgup', 3), ('a', 1), ('l', 45)):
                for _ in range(count):
                    tap(qmp, key)
            deadline = time.monotonic() + 10
            while True:
                text = screen(qmp, folder, 'ready')
                position = re.search(r'NORMAL.*?\b(\d+):(\d+)\b', text)
                probe = decode_trace(memory(qmp, folder, address, size))['events']
                queue = [e for e in probe if e['kind'] in ('queue', 'read', 'cancel')]
                reads = [e['counter'] for e in probe if e['kind'] == 'read']
                presents = [e['counter'] for e in probe if e['kind'] == 'present']
                settled = (queue and queue[-1]['second'] == 0 and
                           reads and presents and presents[-1] >= reads[-1])
                if (re.search(r'\d{3} abcdefgh', text) and position and
                        10 < int(position[1]) < 190 and 20 < int(position[2]) < 140 and settled):
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError('test text/cursor setup not verified; physical capture refused')
                time.sleep(.2)
            header = HEADER.unpack(memory(qmp, folder, address, HEADER.size))
            physical, period = header[3], header[4]
            if not physical or not period:
                raise RuntimeError('guest HPET trace clock is unavailable')
            samples = []
            start = time.monotonic_ns()
            end = start + args.seconds * 10**9
            plan, reference, sent = [], [], 0
            if args.replay:
                plan, reference, end = replay_plan(args.replay, start)
                report['input_origin'] = 'QMP replay of previously captured physical frontend events'
                report['replay_source'] = str(args.replay)
                print('Replaying the final two recorded physical holds; no manual keys needed.', flush=True)
            else:
                report['input_origin'] = 'physical evdev keyboard and graphical QEMU frontend'
                print('READY: in the Mangrove window, hold h, j, k, l for about one second each. '
                      'Release and pause between keys. Capture ends automatically.', flush=True)
            next_sample = 0
            completed_at = None
            released = set()
            while time.monotonic_ns() < end:
                if devices.error:
                    raise RuntimeError(devices.error)
                now = time.monotonic_ns()
                while sent < len(plan) and plan[sent]['host_ns'] <= now:
                    event = plan[sent]
                    qmp.call('input-send-event', {'events': [{'type': 'key', 'data': {
                        'down': bool(event['down']), 'key': {'type': 'qcode', 'data': event['key']}}}]})
                    sent += 1
                if now >= next_sample:
                    before = time.monotonic_ns()
                    real = time.time_ns()
                    counter = struct.unpack('<Q', memory(qmp, folder, physical, 8, physical=True))[0]
                    after = time.monotonic_ns()
                    if samples and counter <= samples[-1]['counter']:
                        raise RuntimeError('HPET physical clock sample did not advance; timing is unqualified')
                    samples.append(dict(counter=counter, before_ns=before,
                                        after_ns=after, realtime_ns=real))
                    next_sample = now + 10**9
                    if not args.replay:
                        observed = analyze(dict(events=[], dropped=0, period_fs=period), samples,
                            [e for e in devices.events if start <= e['host_ns'] <= now],
                            (folder / 'qemu.log').read_text(), start, now)['holds']
                        released = {e['key'] for e in observed if e['hold_ms'] >= 400 and
                                    e['qemu_downs_during_hold'] > 0 and
                                    e['physical_to_qemu_up_ms'] is not None}
                held = {}
                for e in devices.events:
                    if e['value'] in (0, 1):
                        held[(e['device'], e['code'])] = e['value']
                if not args.replay and len(released) == 4 and not any(held.values()):
                    if completed_at is None:
                        completed_at = now
                    if now - completed_at > 3 * 10**9:
                        break
                else:
                    completed_at = None
                if (folder / 'qemu.log').stat().st_size > 8 * 1024**2:
                    raise RuntimeError('QEMU trace bound exceeded')
                if process.poll() is not None:
                    raise RuntimeError('QEMU exited during physical capture')
                time.sleep(.015)
            end = time.monotonic_ns()
            qmp.call('stop')
            trace = decode_trace(memory(qmp, folder, address, size))
            (folder / 'guest-events.json').write_text(json.dumps(trace, indent=2))
            (folder / 'physical-events.json').write_text(json.dumps(devices.events, indent=2))
            (folder / 'clock-calibration.json').write_text(json.dumps(samples, indent=2))
            report['measurement'] = analyze(trace, samples,
                                           reference if args.replay else
                                           [e for e in devices.events if start <= e['host_ns'] <= end],
                                           (folder / 'qemu.log').read_text(), start, end)
            screen(qmp, folder, 'released')
            workspace.set_failure_media(folder / 'ready.ppm', folder / 'released.ppm')
            qmp.call('cont')
            tap(qmp, '\x1b'); type_text(qmp, '/q\n')
            deadline = time.monotonic() + 15
            while 'developer ~:' not in screen(qmp, folder, 'return'):
                if time.monotonic() > deadline:
                    raise TimeoutError('Reed did not return to Shoot')
                time.sleep(.2)
            type_text(qmp, '/bin/shutdown')
            qmp.call('input-send-event', {'events': [{'type': 'key', 'data': {
                'down': True, 'key': {'type': 'qcode', 'data': 'ret'}}}]})
            deadline = time.monotonic() + 20
            while qmp.call('query-status')['status'] != 'shutdown':
                if time.monotonic() > deadline:
                    raise TimeoutError('guest shutdown did not complete')
                time.sleep(.2)
            qmp.call('quit'); process.wait(timeout=10)
        with disk.open('rb') as source:
            source.seek(guest.ROOT_OFFSET)
            (folder / 'stopped-root.img').write_bytes(source.read(guest.PART_BYTES))
        guest.check_image(build, folder / 'stopped-root.img', folder / 'integrity.log')
        report['independent_mgfs'] = 'PASS'
        report['capture'] = ('REPLAY_COMPLETE' if args.replay and sent == len(plan) and not trace['dropped'] else
                            'COMPLETE' if complete_physical_capture(report['measurement']) else 'PARTIAL')
        if report['capture'] in ('COMPLETE', 'REPLAY_COMPLETE'):
            report['status'] = 'CAPTURED'
    except (OSError, EOFError, ValueError, RuntimeError, TimeoutError,
            KeyboardInterrupt, subprocess.SubprocessError) as error:
        report['error'] = str(error)
    finally:
        if process is not None and process.poll() is None and qmp and address and trace is None:
            try:
                qmp.call('stop')
                trace = decode_trace(memory(qmp, folder, address, size))
                (folder / 'guest-events.json').write_text(json.dumps(trace, indent=2))
            except (OSError, EOFError, RuntimeError, ValueError, TimeoutError) as error:
                report['trace_capture_error'] = str(error)
        if process is not None:
            artifacts.terminate_and_reap(process)
        if devices:
            devices.close()
            (folder / 'physical-events.json').write_text(json.dumps(devices.events, indent=2))
        workspace.finish(report, verified=report.get('independent_mgfs') == 'PASS')
    summary = {k: v for k, v in report.items() if k != 'tracked_diff'}
    if 'measurement' in summary:
        summary['measurement'] = {k: v for k, v in report['measurement'].items()
                                  if k not in ('qemu_navigation_events', 'framebuffer_flushes')}
        flushes = report['measurement'].get('framebuffer_flushes', [])
        summary['measurement']['framebuffer_flush_count'] = len(flushes)
        summary['measurement']['max_framebuffer_flush_ms'] = max(
            (e['milliseconds'] for e in flushes), default=0)
    print(json.dumps(summary, indent=2), flush=True)
    return 0 if report.get('capture') in ('COMPLETE', 'REPLAY_COMPLETE') else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--make', default='make')
    parser.add_argument('--seconds', type=int, choices=range(10, 121), default=90)
    parser.add_argument('--replay', type=Path, help='replay the last two physical holds in an earlier report')
    parser.add_argument('platform', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    def interrupted(signum, _frame):
        raise InterruptedError(f'signal {signum}')

    signal.signal(signal.SIGTERM, interrupted)
    try:
        return run(args)
    except (OSError, RuntimeError) as error:
        parser.exit(2, str(error) + '\n')


if __name__ == '__main__':
    sys.exit(main())
