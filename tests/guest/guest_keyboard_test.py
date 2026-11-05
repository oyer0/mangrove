#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute Shoot commands using isolated PS/2-only and USB-only keyboards."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/shared'))
import artifact_lifecycle as artifacts
import guest_mgfs_test as mgfs
from ovmf_firmware import default_path
from qmp_client import QMP
import qemu_accel


def run(workspace, args, mode):
    work = workspace.path / mode
    work.mkdir()
    disk, variables = work / 'disk.img', work / 'vars.fd'
    shutil.copyfile(args.image, disk)
    with disk.open('rb') as source:
        if hashlib.file_digest(source, 'sha256').hexdigest() != args.image_sha256:
            raise RuntimeError('copied image does not match qualification input')
    shutil.copyfile(default_path('vars'), variables)
    machine = 'q35,i8042=off' if mode == 'usb-only' else 'q35'
    command = [args.qemu, '-machine', machine, '-accel', args.accel,
               '-cpu', 'host' if args.accel == 'kvm' else 'max',
               '-smp', '2', '-m', '512M', '-display', 'none', '-vga', 'none',
               '-device', 'VGA,xres=1024,yres=768', '-no-reboot',
               '-serial', f'file:{work}/serial.log', '-qmp', 'stdio',
               '-drive', f'if=pflash,format=raw,readonly=on,file={default_path("code")}',
               '-drive', f'if=pflash,format=raw,file={variables}',
               '-drive', f'if=none,id=root,format=raw,file={disk}',
               '-device', 'qemu-xhci,id=xhci,p3=0',
               '-device', 'usb-storage,drive=root,bus=xhci.0,port=2,bootindex=1']
    if mode != 'ps2-only':
        command += ['-device', 'usb-kbd,bus=xhci.0,port=1']
    qtest_temp = None
    if args.inject_aux and mode != 'usb-only':
        qtest_temp = tempfile.TemporaryDirectory(prefix='input-qtest-',dir=ROOT/'build')
        qtest_path = Path(qtest_temp.name)/'io.sock'
        command += ['-qtest',f'unix:{qtest_path},server=on,wait=off']
    (work / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    log = (work / 'qemu.log').open('wb')
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=log, bufsize=0)
    qmp = None
    try:
        qmp = QMP(process, timeout=15)
        tree = qmp.call('human-monitor-command', {'command-line':'info qtree'})
        usb = qmp.call('human-monitor-command', {'command-line':'info usb'})
        (work / 'devices.txt').write_text(tree + '\n' + usb)
        assert ('dev: i8042' in tree) == (mode != 'usb-only')
        assert ('dev: usb-kbd' in tree) == (mode != 'ps2-only')

        def screen(stem):
            path = work / f'{stem}.ppm'
            qmp.call('screendump', {'filename':str(path)})
            text = mgfs.screen_text(path)
            (work / f'{stem}.txt').write_text(text + '\n')
            return text

        def wait_prompt(stem, timeout):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                text = screen(stem)
                # A trace overlay can cover cells to the right/below the
                # prompt. Command-output verification remains offline/exact.
                if any(re.fullmatch(r'developer ~:[ ?]*', line)
                       for line in text.splitlines()):
                    return text
                if process.poll() is not None:
                    raise RuntimeError('guest exited before Shoot prompt')
                time.sleep(.2)
            raise TimeoutError(f'{mode}: no Shoot prompt after {stem}')

        def type_line(line):
            keys = {' ':'spc', '/':'slash', '-':'minus', '.':'dot', '>':'shift-dot'}
            for char in line:
                qmp.call('human-monitor-command',
                         {'command-line':'sendkey ' + keys.get(char, char) + ' 12'})
                time.sleep(.025)
            qmp.call('human-monitor-command', {'command-line':'sendkey ret 12'})
            time.sleep(.3)

        def inject_aux():
            # Test-only emulated hardware access. Pause CPUs for the controller
            # transaction; never race the guest's port 60 reader. D3 produces
            # a real AUX-tagged OBF/IRQ12, not a keyboard callback invocation.
            qmp.call('stop')
            with socket.socket(socket.AF_UNIX) as channel:
                channel.settimeout(5); channel.connect(str(qtest_path))
                with channel.makefile('rwb',buffering=0) as stream:
                    def io(command):
                        stream.write(command.encode()+b'\n')
                        reply=stream.readline().decode().strip().split()
                        if not reply or reply[0]!='OK': raise RuntimeError(reply)
                        return int(reply[1],0) if len(reply)>1 else None
                    assert not io('inb 0x64') & 3, 'controller busy before injection'
                    io('outb 0x64 0x20'); config=io('inb 0x60')
                    io('outb 0x64 0x60'); io(f'outb 0x60 {config|2}')
                    io('outb 0x64 0xd3'); io('outb 0x60 0x08')
                    pending=io('inb 0x64')
                    assert pending & 0x21 == 0x21, pending
                    qmp.call('cont')
                    deadline=time.monotonic()+3
                    status=pending
                    while status & 1 and time.monotonic()<deadline:
                        status=io('inb 0x64')
                        if status & 1: time.sleep(.01)
                    result={'config_before':config,'config_injected':config|2,
                            'pending_status':pending,'final_status':status,
                            'aux_obf_drained':not bool(status&1)}
                    (work/'aux-injection.json').write_text(json.dumps(result,indent=2)+'\n')
                    screen('aux-injection')
                    assert result['aux_obf_drained'], 'AUX output remains occupied after IRQ12'

        wait_prompt('boot', 75)
        type_line('say keyboardproof > /temp/keyboard-proof')
        wait_prompt('proof', 15)
        type_line('help > /temp/keyboard-help')
        wait_prompt('help-file', 15)
        type_line('list / > /temp/keyboard-list')
        wait_prompt('list-file', 15)
        type_line('help')
        wait_prompt('help', 15)
        type_line('list')
        wait_prompt('list', 15)
        if qtest_temp: inject_aux()
        typing = []
        for index in range(args.rounds):
            payload = f'gkkkhuabcdefghijklmnopqrstuvwxyz0123456789{index}'
            type_line(f'say {payload} > /temp/keyboard-typing-{index}')
            wait_prompt('typing', 15)
            typing.append(payload.encode()+b'\n')
        final_text=screen('sustained')
        footer_live=False
        if 'INPUT ms=' in final_text:
            pattern=r'INPUT ms=(\d+) IRQ1=(\d+)/EOI=(\d+) IRQ12=(\d+)/EOI=(\d+)'
            first=re.search(pattern,final_text)
            assert first, 'diagnostic footer is clipped'
            fresh=None
            deadline=time.monotonic()+5
            while time.monotonic()<deadline:
                last=re.search(pattern,screen('idle-footer'))
                if last and int(last[1])>int(first[1]):
                    if fresh is None:
                        fresh=last  # discard a possibly stale pre-idle frame
                    elif int(last[1])>int(fresh[1]):
                        assert last.groups()[1:]==fresh.groups()[1:], 'unexpected input during idle check'
                        assert last[2]==last[3] and last[4]==last[5], 'incomplete input EOI'
                        footer_live=True;break
                time.sleep(.05)
            assert footer_live, 'footer did not update without keyboard input'
        qmp.call('quit'); process.wait(timeout=15)
        assert process.returncode == 0
    finally:
        if qmp: qmp.close()
        artifacts.terminate_and_reap(process)
        log.close()
        if qtest_temp: qtest_temp.cleanup()

    # Offline only after QEMU is reaped. Verify actual command output, not
    # echoed input or the mere presence of an interactive prompt.
    with disk.open('rb') as stream:
        stream.seek(mgfs.ROOT_OFFSET)
        root = stream.read(64 * 1024 * 1024)
    outputs = {}
    for name in ('proof', 'help', 'list'):
        _, record = mgfs.find(root, '/temp/keyboard-' + name)
        outputs[name] = mgfs.payload(root, record)
        (work / f'command-{name}.txt').write_bytes(outputs[name])
    assert outputs['proof'] == b'keyboardproof\n'
    assert outputs['help'].startswith(b'Shoot built-in commands:\n')
    assert b'jump' in outputs['help'] and b'reload' in outputs['help']
    assert b'bin' in outputs['list'] and b'home' in outputs['list']
    for index, expected in enumerate(typing):
        _, record = mgfs.find(root, f'/temp/keyboard-typing-{index}')
        observed = mgfs.payload(root, record)
        assert observed == expected, f'{mode}: sustained typing mismatch at round {index}'
    (work/'sustained-output.txt').write_bytes(b''.join(typing))
    # These media are owned copies; retain bounded screenshots and text only.
    disk.unlink(); variables.unlink()
    return {'mode':mode, 'status':'PASS', 'commands':['say', 'help', 'list'],
            'proof_bytes':len(outputs['proof']), 'help_bytes':len(outputs['help']),
            'list_bytes':len(outputs['list']), 'typing_rounds':len(typing),
            'typing_output_bytes':sum(map(len,typing)), 'key_interval_ms':25,
            'key_hold_ms':12, 'aux_injected':bool(qtest_temp),
            'idle_footer_live':footer_live}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--accel', choices=('kvm','tcg'), default=qemu_accel.default_accel())
    parser.add_argument('--mode', choices=('ps2-only','usb-only','both','all'), default='all')
    parser.add_argument('--rounds', type=int, default=20)
    parser.add_argument('--inject-aux', action='store_true',
                        help='inject one AUX output byte through emulated i8042 hardware')
    args = parser.parse_args()
    if not 1 <= args.rounds <= 100: parser.error('--rounds must be in 1..100')
    error = qemu_accel.accel_error(args.accel, qemu=args.qemu)
    if error: parser.error(error)
    if not args.image.is_file(): parser.error('image must be a regular image file')
    with args.image.open('rb') as source:
        args.image_sha256 = hashlib.file_digest(source, 'sha256').hexdigest()
    workspace = artifacts.ArtifactWorkspace(output=args.output, name='keyboard')
    report = {'status':'FAIL', 'accel':args.accel, 'runs':[],
              'image':str(args.image), 'image_sha256':args.image_sha256}
    try:
        workspace.require_free_space(args.image.stat().st_size + 64*1024*1024)
        modes = ('ps2-only','usb-only','both') if args.mode == 'all' else (args.mode,)
        for mode in modes:
            report['runs'].append(run(workspace, args, mode))
        report['status'] = 'PASS'
    except BaseException as error:
        report['reason'] = str(error)
        raise
    finally:
        workspace.finish(report, verified=report['status']=='PASS')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
