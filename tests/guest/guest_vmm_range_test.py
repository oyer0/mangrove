#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify leased kernel-range teardown and SMP TLB acknowledgements."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import secrets
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tests'))
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1] / "shared"))
import artifact_lifecycle as artifacts
import guest_mgfs_test as guest

RANGE = re.compile(
    r'^VMM_RANGE_GUEST pass=(\d+) online=(\d+) ack=([0-9a-fA-F]+) '
    r'other_cr3=([0-9a-fA-F]+) release_blocked=(\d+) '
    r'frame_reused=(\d+) slab=(\d+)$', re.M)
SMP = re.compile(r'^\[SMP\] (?:final )?discovered=(\d+) online=(\d+)$', re.M)


def validate_boot_log(text, cpus, hybrid_routing):
    records = RANGE.findall(text)
    if len(records) != 1:
        raise ValueError('kernel-range shootdown witness is missing or duplicated')
    passed, online, ack, other, blocked, reused, slab = records[0]
    passed, online, slab = int(passed), int(online), int(slab)
    ack, other = int(ack, 16), int(other, 16)
    blocked, reused = int(blocked), int(reused)
    smp = SMP.findall(text)
    if not smp or int(smp[-1][1]) < cpus or online != cpus or passed != 1:
        raise ValueError('kernel-range guest did not report successful requested CPU participation')
    expected = (1 << cpus) - 1
    if ack & expected != expected:
        raise ValueError('not all online CPUs acknowledged the invalidation')
    if cpus == 2 and not (other & 2):
        raise ValueError('CPU 1 acknowledged without using a distinct CR3')
    if blocked != 1 or reused != 1:
        raise ValueError('owned frame was not pinned and reused after teardown')
    if slab != int(hybrid_routing):
        raise ValueError('runtime slab initialization does not match allocator configuration')
    return {'online_cpus': online, 'ack_mask': ack,
            'remote_distinct_cr3_mask': other,
            'mapped_release_rejected': bool(blocked),
            'frame_reused_after_acknowledged_unmap': bool(reused),
            'hybrid_slab_initialized': bool(slab),
            'smp_discovered_cpus': int(smp[-1][0]),
            'smp_online_cpus': int(smp[-1][1])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path,
                        help='new output directory; retained for review')
    artifacts.add_retention_argument(parser)
    parser.add_argument('--cpus', type=int, choices=(1, 2), required=True)
    parser.add_argument('--accel', choices=('tcg', 'kvm'), default=guest.default_accel())
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--ovmf-code', type=Path,
                        default=Path('/usr/share/OVMF/OVMF_CODE_4M.fd'))
    parser.add_argument('--ovmf-vars', type=Path,
                        default=Path('/usr/share/OVMF/OVMF_VARS_4M.fd'))
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--hybrid-routing', type=int, choices=(0, 1), required=True)
    args = parser.parse_args()
    error = guest.accel_error(args.accel, args.qemu)
    if error:
        parser.error(error)
    args.build_dir = args.build_dir.resolve()
    args.login_prompt = 'developer ~:'
    args.secondary_disk = None
    args.hotplug_replacement = None
    if not 30 <= args.timeout <= 300:
        parser.error('--timeout must be between 30 and 300 seconds')
    workspace = artifacts.ArtifactWorkspace(
        args.output, name='guest-vmm-range', retain=args.retain_artifacts)
    args.output = workspace.path
    report = {
        'status': 'FAIL', 'cpus': args.cpus, 'accel': args.accel,
        'allocator_configuration': ('hybrid' if args.hybrid_routing else 'indexed-only'),
    }
    try:
        run = secrets.token_hex(8)
        folder = args.output / run
        folder.mkdir()
        disk = guest.create_disk(folder, args.build_dir, run)
        workspace.set_failure_media(disk)
        with disk.open('rb') as source:
            source.seek(guest.ROOT_OFFSET)
            image = bytearray(source.read(guest.PART_BYTES))
        script = (
            '/bin/say VMM_RANGE_NATIVE_COMPLETED > /temp/vmm-native.log\n'
            '/bin/say child-exit-0-vmm > /temp/vmm.exit\n'
            '/bin/shutdown\n').encode('ascii')
        guest.install(image, 6, 'vmm.shoot', script)
        prepared = folder / 'prepared-root.img'
        prepared.write_bytes(image)
        with disk.open('r+b') as media:
            media.seek(guest.ROOT_OFFSET)
            media.write(image)
        guest.check_image(args.build_dir, prepared,
                          folder / 'prepared-checker.log')

        runtime = guest.boot(folder, disk, 'vmm', args)
        with disk.open('rb') as source:
            source.seek(guest.ROOT_OFFSET)
            stopped_image = source.read(guest.PART_BYTES)
        stopped = folder / 'stopped-root.img'
        stopped.write_bytes(stopped_image)
        guest.check_image(args.build_dir, stopped,
                          folder / 'stopped-checker.log')
        _, boot_record = guest.find(stopped_image, '/sys/logs/boot.log')
        boot_log = guest.payload(stopped_image, boot_record).decode('ascii')
        (folder / 'boot.log').write_text(boot_log)
        witness = validate_boot_log(boot_log, args.cpus,
                                    bool(args.hybrid_routing))
        _, native_record = guest.find(stopped_image, '/temp/vmm-native.log')
        native = guest.payload(stopped_image, native_record)
        _, exit_record = guest.find(stopped_image, '/temp/vmm.exit')
        child_exit = guest.payload(stopped_image, exit_record)
        if native != b'VMM_RANGE_NATIVE_COMPLETED\n' or \
                child_exit != b'child-exit-0-vmm\n':
            raise ValueError('native child completion evidence is incomplete')
        try:
            validate_boot_log(RANGE.sub('', boot_log), args.cpus,
                              bool(args.hybrid_routing))
        except ValueError:
            negative_control = 'missing shootdown witness rejected'
        else:
            raise ValueError('shootdown evidence negative control was accepted')
        changed = ('Makefile', 'kernel/include/vmm.h', 'kernel/src/cpu.c',
                   'kernel/src/init.c', 'kernel/src/smp.c',
                   'kernel/include/pmm.h', 'kernel/src/pmm.c',
                   'kernel/src/vmm.c',
                   'kernel/src/vmm_guest_test.c',
                   'tests/guest/guest_vmm_range_test.py')
        report.update({
            'status': 'PASS', 'run': run, 'witness': witness,
            'native_child': {'output': native.decode().strip(),
                             'exit': child_exit.decode().strip()},
            'negative_control': negative_control,
            'guest_runtime_seconds': runtime['seconds'],
            'independent_checker': 'MGFS clean before and after guest execution',
            'stopped_image_sha256': guest.digest(disk),
            'boot_log_sha256': hashlib.sha256(boot_log.encode()).hexdigest(),
            'source_revision': subprocess.run(
                ['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True,
                capture_output=True, check=True).stdout.strip(),
            'source_hashes': {
                path: hashlib.sha256((ROOT / path).read_bytes()).hexdigest()
                for path in changed},
            'target_compiler': subprocess.run(
                ['gcc', '--version'], text=True, capture_output=True,
                check=True).stdout.splitlines()[0],
        })
    except Exception as error:
        report['error'] = f'{type(error).__name__}: {error}'
    finally:
        report = workspace.finish(report, verified=report['status'] == 'PASS')
    print(json.dumps(report, sort_keys=True))
    return 0 if report['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
