#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native login/logout screen-reset qualification on disposable QEMU media."""
import argparse
import hashlib
import json
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tests' / 'guest'))
import guest_mgfs_test as mgfs
sys.path.insert(0, str(ROOT / 'tests' / 'shared'))
import artifact_lifecycle as artifacts


def screen_has(text, value):
    return value in text


def qmp_key(qmp, key):
    qmp.call('human-monitor-command', {
        'command-line': 'sendkey ' + key + ' 35'})


def type_text(qmp, value):
    key_names = {' ': 'spc', '/': 'slash'}
    for character in value:
        if character == '\n':
            qmp_key(qmp, 'ret')
        else:
            qmp_key(qmp, key_names.get(character, character))


def boot_logout(folder, disk, args):
    part = folder / 'logout'
    part.mkdir()
    variables = part / 'vars.fd'
    shutil.copyfile(args.ovmf_vars, variables)
    synced = {
        'boot_disk': mgfs.sync_image_for_boot(disk),
        'firmware_variables': mgfs.sync_image_for_boot(variables),
    }
    (part / 'image-sync.json').write_text(json.dumps(synced, indent=2))
    invocation = [args.qemu, '-machine', 'q35', '-accel', args.accel,
                  '-cpu', 'max' if args.accel == 'tcg' else 'host',
                  '-smp', str(args.cpus), '-m', '512M',
                  '-rtc', 'base=utc,clock=vm', '-display', 'none',
                  '-serial', f'file:{part}/serial.log',
                  '-debugcon', f'file:{part}/debugcon.log', '-qmp', 'stdio',
                  '-drive', f'if=pflash,format=raw,readonly=on,file={args.ovmf_code}',
                  '-drive', f'if=pflash,format=raw,file={variables}',
                  '-drive', f'id=usb,file={disk},format=raw,if=none',
                  '-device', 'qemu-xhci,id=xhci',
                  '-device', 'usb-storage,id=boot-storage,bus=xhci.0,port=2,drive=usb,bootindex=1',
                  '-device', 'usb-kbd,id=keyboard,bus=xhci.0,port=1',
                  '-netdev', 'user,id=net0',
                  '-device', 'e1000,netdev=net0,mac=52:54:00:18:01:01',
                  '-no-reboot', '-no-shutdown']
    (part / 'command.json').write_text(json.dumps(invocation, indent=2))
    with (part / 'qemu.log').open('wb') as qemu_log:
        process = subprocess.Popen(invocation, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=qemu_log,
                                   bufsize=0)
        qmp = None
        try:
            qmp = mgfs.QMP(process)
            current_ppm = part / 'current.ppm'
            framebuffer_checks = {}

            def save_capture(label, text):
                target = part / f'{label}.ppm'
                shutil.copyfile(current_ppm, target)
                (part / f'{label}.txt').write_text(text)
                framebuffer_checks[label] = {
                    'sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
                    'recognized_rows': [row for row in text.splitlines() if row],
                }

            def capture(label=None):
                if current_ppm.exists():
                    current_ppm.unlink()
                qmp.screendump(current_ppm)
                text = mgfs.screen_text(current_ppm)
                if label:
                    save_capture(label, text)
                return text

            def wait_for(predicate, description, label=None):
                deadline = time.monotonic() + args.timeout
                while time.monotonic() < deadline:
                    failure = mgfs.kernel_failure(part)
                    if failure:
                        raise RuntimeError(failure)
                    if process.poll() is not None:
                        raise RuntimeError(
                            f'QEMU exited while waiting for {description}: '
                            f'{process.returncode}')
                    text = capture()
                    if predicate(text):
                        if label:
                            capture(label)
                        return text
                    time.sleep(0.2)
                capture('timeout')
                raise TimeoutError(f'timed out waiting for {description}')

            def send_line(value, echo=None):
                type_text(qmp, value)
                expected = echo if echo is not None else value
                wait_for(lambda text: expected in text,
                         f'keyboard echo {expected!r}')
                qmp_key(qmp, 'ret')

            def login():
                wait_for(lambda text: screen_has(text, 'Username:'),
                         'username prompt')
                send_line('developer', 'Username: developer')
                wait_for(lambda text: screen_has(text, 'Password:'),
                         'password prompt')
                type_text(qmp, 'mangrove')
                qmp_key(qmp, 'ret')
                return wait_for(lambda text: text.rstrip().endswith(
                    'developer ~:'), 'Shoot prompt')

            def expect_clean_login(label, forbidden=()):
                text = wait_for(lambda value: value.rstrip().endswith(
                    'Username:'), 'cleared login prompt', label)
                rows = [row.strip() for row in text.splitlines() if row.strip()]
                if rows != ['Username:']:
                    raise AssertionError(
                        f'{label}: framebuffer retained prior session rows: {rows!r}')
                for token in forbidden:
                    if token in text:
                        raise AssertionError(
                            f'{label}: prior session text remains: {token}')
                return text

            def run_say(token):
                command = '/bin/say ' + token
                type_text(qmp, command)
                wait_for(lambda text: command in text,
                         'complete command echo')
                qmp_key(qmp, 'ret')
                wait_for(lambda text: token in text and
                         text.rstrip().endswith('developer ~:'),
                         f'command output and next prompt for {token}')

            # Authenticate through the virtual USB HID keyboard.  The first
            # session scrolls old text out of the physical viewport while it
            # remains in the terminal's retained cell ring.
            login()
            run_say('logoutmarker')
            before = capture('session-output-before-scroll')
            if 'logoutmarker' not in before:
                raise AssertionError('session marker was not rendered')
            for index in range(1, 30):
                run_say(f'logoutfill{index:02d}')
                visible = capture()
                if 'logoutmarker' not in visible:
                    save_capture('session-output-after-scroll', visible)
                    break
            else:
                raise AssertionError('guest output did not scroll marker off screen')

            send_line('exit', 'exit')
            expect_clean_login('login-after-exit', ('logoutmarker', 'logoutfill'))

            login()
            send_line('logout', 'logout')
            expect_clean_login('login-after-logout')

            login()
            qmp_key(qmp, 'ctrl-d')
            expect_clean_login('login-after-empty-ctrl-d')

            login()
            send_line('exit 7', 'exit 7')
            expect_clean_login('login-after-nonzero-exit')
            if 'Session ended unexpectedly' in capture():
                raise AssertionError('unexpected-exit text remained on login screen')
            qmp.call('quit')
            process.wait(timeout=10)
            if process.returncode != 0:
                raise RuntimeError(f'QEMU exited with status {process.returncode}')
            return {
                'status': 'PASS', 'vcpus': args.cpus,
                'normal_session_exit': True, 'logout_builtin': True,
                'empty_ctrl_d': True, 'nonzero_shell_exit': True,
                'scroll_before_logout': True,
                'framebuffer_clean_after_each_session': True,
                'framebuffer_checks': framebuffer_checks,
                'qemu_disposable_image': True,
                'qemu_command': invocation,
            }
        finally:
            if process.poll() is None:
                if qmp:
                    try:
                        qmp.call('quit')
                    except (OSError, RuntimeError, EOFError, ValueError):
                        pass
            mgfs.artifacts.terminate_and_reap(process, timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--accel', choices=('tcg', 'kvm'),
                        default=mgfs.default_accel())
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--ovmf-code', type=Path,
                        default=Path('/usr/share/OVMF/OVMF_CODE_4M.fd'))
    parser.add_argument('--ovmf-vars', type=Path,
                        default=Path('/usr/share/OVMF/OVMF_VARS_4M.fd'))
    parser.add_argument('--timeout', type=int, default=240)
    parser.add_argument('--cpus', type=int, choices=(2,), default=2)
    parser.add_argument('--output', type=Path,
                        help='new output directory retained for review')
    artifacts.add_retention_argument(parser)
    args = parser.parse_args()
    error = mgfs.accel_error(args.accel, args.qemu)
    if error:
        parser.error(error)
    args.build_dir = args.build_dir.resolve()
    for path in (args.output, args.ovmf_code, args.ovmf_vars):
        if ',' in str(path) or '\n' in str(path):
            parser.error('QEMU image paths cannot contain commas or newlines')
    if args.output is not None and args.output.exists():
        parser.error('output directory must be new')
    if not 60 <= args.timeout <= 600:
        parser.error('timeout must be between 60 and 600 seconds')
    workspace = artifacts.ArtifactWorkspace(
        args.output, name='guest-logout', retain=args.retain_artifacts)
    args.output = workspace.path
    report = {'status': 'FAIL', 'accel': args.accel, 'cpus': args.cpus}
    try:
        run = secrets.token_hex(8)
        folder = args.output / run
        folder.mkdir()
        disk = mgfs.create_disk(folder, args.build_dir, run,
                                power_history_smoke=True,
                                manual_login=True)
        report['initial_disk_sha256'] = mgfs.digest(disk)
        report.update(boot_logout(folder, disk, args))
        report['smp'] = mgfs.inspect_smp_boot(disk, args.cpus)
        report['run'] = run
        report['artifact_bytes'] = sum(
            item.stat().st_size for item in folder.rglob('*') if item.is_file())
        report['status'] = 'PASS'
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        report = workspace.finish(report, verified=report.get('status') == 'PASS')
        print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
