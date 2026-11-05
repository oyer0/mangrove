#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify native thread migration and timer preemption on isolated KVM."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import secrets
import subprocess
import sys

from ovmf_firmware import default_path

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tests' / 'shared'))
import artifact_lifecycle as artifacts
sys.path.insert(0, str(ROOT / 'tests'))
import guest_mgfs_test as guest

MIGRATION = re.compile(
    r'^SCHED_MIGRATION pass=(\d+) cpus=(\d+) online_mask=([0-9a-fA-F]+) '
    r'thread=(\d+) samples=(\d+) cpu_mask=([0-9a-fA-F]+) '
    r'transitions=(\d+) cooperative_yields=(\d+) thread_stable=(\d+) '
    r'joined=(\d+) failure=(\d+) start_ms=(\d+) end_ms=(\d+)$', re.M)
PREEMPT = re.compile(
    r'^SCHED_PREEMPT pass=(\d+) cpus=(\d+) online_mask=([0-9a-fA-F]+) '
    r'workers=(\d+) started=(\d+) spin_barrier=(\d+) '
    r'cooperative_yields=(\d+) multi_samples=(\d+) '
    r'busy_mask=([0-9a-fA-F]+) busy_delta=([0-9,]+) '
    r'work_units=(\d+) start_ms=(\d+) end_ms=(\d+)$', re.M)
RESULT = re.compile(
    r'^SCHED_SMP_RESULT pass=(\d+) cpus=(\d+) migration_status=(-?\d+) '
    r'preemption_status=(-?\d+) concurrent_children=(\d+)$', re.M)
SMP = re.compile(r'^\[SMP\] (?:final )?discovered=(\d+) online=(\d+)$', re.M)


def validate_native_evidence(migration_text, preempt_text, result_text, cpus):
    migration_rows = MIGRATION.findall(migration_text)
    preempt_rows = PREEMPT.findall(preempt_text)
    result_rows = RESULT.findall(result_text)
    if len(migration_rows) != 1 or len(preempt_rows) != 1 or \
            len(result_rows) != 1:
        raise ValueError('native scheduler evidence is missing or duplicated')

    (migration_pass, migration_cpus, online_text, thread_text, samples_text,
     cpu_mask_text, transitions_text, yields_text, stable_text, joined_text,
     failure_text, migration_start_text, migration_end_text) = migration_rows[0]
    migration_pass, migration_cpus, thread, samples, transitions, yields, \
        stable, joined, failure, migration_start, migration_end = map(int, (
            migration_pass, migration_cpus, thread_text, samples_text,
            transitions_text, yields_text, stable_text, joined_text,
            failure_text, migration_start_text, migration_end_text))
    online_mask = int(online_text, 16)
    migration_mask = int(cpu_mask_text, 16)
    expected_mask = (1 << cpus) - 1
    if (migration_pass != 1 or migration_cpus != cpus or
            online_mask != expected_mask or not thread or not samples or
            stable != 1 or joined != 1 or failure != 0 or
            (cpus == 1 and yields != 0) or
            (cpus > 1 and yields == 0) or
            migration_end <= migration_start or
            migration_mask & ~expected_mask):
        raise ValueError('same-thread migration witness is incomplete')
    if cpus == 1:
        if migration_mask != 1 or transitions != 0:
            raise ValueError('one-vCPU control unexpectedly reports migration')
    elif (migration_mask.bit_count() < 2 or transitions == 0):
        raise ValueError('same native thread did not migrate between CPUs')

    (preempt_pass, preempt_cpus, preempt_online_text, workers_text,
     started_text, barrier_text, preempt_yields_text, multi_text,
     busy_mask_text, delta_text, units_text, load_start_text,
     load_end_text) = preempt_rows[0]
    preempt_pass, preempt_cpus, workers, started, barrier, preempt_yields, \
        multi_samples, work_units, load_start, load_end = map(int, (
            preempt_pass, preempt_cpus, workers_text, started_text,
            barrier_text, preempt_yields_text, multi_text, units_text,
            load_start_text, load_end_text))
    preempt_online = int(preempt_online_text, 16)
    busy_mask = int(busy_mask_text, 16)
    busy_deltas = [int(value) for value in delta_text.split(',')]
    if (preempt_pass != 1 or preempt_cpus != cpus or
            preempt_online != expected_mask or workers != cpus + 1 or
            started != workers or barrier != 1 or preempt_yields != 0 or
            work_units <= 0 or load_end <= load_start or
            busy_mask != expected_mask or len(busy_deltas) != cpus or
            any(delta <= 0 for delta in busy_deltas)):
        raise ValueError('oversubscribed noncooperative preemption evidence is incomplete')
    if cpus == 1:
        if multi_samples != 0:
            raise ValueError('one-vCPU control reports impossible simultaneous CPUs')
    elif multi_samples <= 0:
        raise ValueError('multicore work lacks a simultaneous process CPU snapshot')
    if max(migration_start, load_start) >= min(migration_end, load_end):
        raise ValueError('migration sampling did not overlap the CPU load interval')

    result_pass, result_cpus, migration_status, preempt_status, concurrent = \
        map(int, result_rows[0])
    if (result_pass != 1 or result_cpus != cpus or migration_status != 0 or
            preempt_status != 0 or concurrent != 1):
        raise ValueError('native child lifecycle or concurrent launch failed')

    return {
        'online_cpu_mask': expected_mask,
        'migration': {
            'thread_id': thread, 'sample_count': samples,
            'cpu_mask': migration_mask, 'cpu_transitions': transitions,
            'cooperative_yields': yields, 'failure_code': failure,
            'sample_interval_ms': [migration_start, migration_end],
            'joined_and_reaped': bool(joined),
        },
        'preemption': {
            'oversubscribed_workers': workers,
            'spin_only_workers_started': started,
            'cooperative_yields': preempt_yields,
            'simultaneous_process_samples': multi_samples,
            'cpu_busy_tick_deltas': busy_deltas,
            'useful_work_units': work_units,
            'load_interval_ms': [load_start, load_end],
        },
        'child_statuses': [migration_status, preempt_status],
    }


def source_hash(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, workspace):
    run_id = secrets.token_hex(8)
    folder = workspace.path / run_id
    folder.mkdir()
    disk = guest.create_disk(
        folder, args.build_dir, run_id, scheduler_smp_smoke=True,
        scheduler_smp_cpus=args.cpus)
    workspace.set_failure_media(disk)
    (folder / 'initial-disk.sha256').write_text(guest.digest(disk) + '\n')
    runtime = guest.boot(folder, disk, 'scheduler-smp', args)

    with disk.open('rb') as source:
        source.seek(guest.ROOT_OFFSET)
        image = source.read(guest.PART_BYTES)
    stopped_root = folder / 'stopped-root.img'
    stopped_root.write_bytes(image)
    guest.check_image(args.build_dir, stopped_root,
                      folder / 'stopped-checker.log')
    boot_record = guest.find(image, '/sys/logs/boot.log')[1]
    boot_log = guest.payload(image, boot_record).decode('ascii', errors='strict')
    (folder / 'boot.log').write_text(boot_log)
    smp_records = SMP.findall(boot_log)
    if not smp_records:
        raise ValueError('guest has no final SMP online CPU record')
    discovered, online = map(int, smp_records[-1])
    if online != args.cpus or discovered < args.cpus:
        raise ValueError(f'guest online CPU set is {online}/{discovered}, '
                         f'expected exactly {args.cpus}')

    threadtest_record = guest.find(image, '/bin/threadtest')[1]
    threadtest = guest.payload(image, threadtest_record)
    built_threadtest = (args.build_dir / 'threadtest/threadtest.elf').read_bytes()
    if threadtest != built_threadtest:
        raise ValueError('embedded native workload differs from isolated build')
    pith_record = guest.find(image, '/boot/pith.elf')[1]
    pith = guest.payload(image, pith_record)
    built_pith = (args.build_dir / 'Mangrove/pith.elf').read_bytes()
    if pith != built_pith:
        raise ValueError('embedded Pith differs from isolated build')

    def read_log(path):
        item = guest.find(image, path)[1]
        text = guest.payload(image, item).decode('ascii', errors='strict')
        (folder / (path.rsplit('/', 1)[-1])).write_text(text)
        return text

    native = validate_native_evidence(
        read_log('/temp/scheduler-migration.log'),
        read_log('/temp/scheduler-preempt.log'),
        read_log('/temp/scheduler-smp-result.log'), args.cpus)
    completion = guest.payload(
        image, guest.find(image, '/temp/scheduler-smp.exit')[1])
    if completion != f'child-exit-0-scheduler-smp-{run_id}\n'.encode():
        raise ValueError('native scheduler workload did not return to Shoot')
    if runtime.get('child_status') != 0:
        raise ValueError('Shoot reported nonzero native scheduler workload status')
    native['guest_returned_to_shoot_and_shutdown'] = True
    native['stopped_image_sha256'] = guest.digest(disk)
    runtime_summary = {
        key: runtime[key] for key in (
            'seconds', 'launch_echoed_characters', 'child_status',
            'host_image_sync', 'power_transition_event')
    }
    return {
        'status': 'PASS', 'run': run_id, 'cpus_requested': args.cpus,
        'accelerator': args.accel,
        'smp_boot': {'discovered': discovered, 'online': online,
                     'online_indices': list(range(online))},
        'witness': native, 'guest_runtime': runtime_summary,
        'independent_mgfs_check': 'PASS before and after guest execution',
        'embedded_identity': {
            'threadtest_sha256': source_hash(args.build_dir /
                                             'threadtest/threadtest.elf'),
            'pith_sha256': source_hash(args.build_dir / 'Mangrove/pith.elf'),
            'embedded_threadtest_matches_build': True,
            'embedded_pith_matches_build': True,
        },
        'source_revision': subprocess.run(
            ['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True,
            capture_output=True, check=True).stdout.strip(),
        'source_hashes': {
            path: source_hash(ROOT / path) for path in (
                'kernel/src/scheduler.c', 'kernel/src/smp.c',
                'kernel/src/vmm.c',
                'tests/userspace/thread_lifecycle_guest.c',
                'tests/guest/guest_scheduler_smp_test.py',
            )
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--cpus', type=int, choices=(1, 2, 4), required=True)
    parser.add_argument('--output', type=Path,
                        help='new output directory; retained for review')
    artifacts.add_retention_argument(parser)
    parser.add_argument('--accel', choices=('tcg', 'kvm'),
                        default=guest.default_accel())
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--ovmf-code', type=Path, default=default_path('code'))
    parser.add_argument('--ovmf-vars', type=Path, default=default_path('vars'))
    parser.add_argument('--timeout', type=int, default=180)
    args = parser.parse_args()
    error = guest.accel_error(args.accel, args.qemu)
    if error:
        parser.error(error)
    if not 60 <= args.timeout <= 300:
        parser.error('--timeout must be between 60 and 300 seconds')
    args.build_dir = args.build_dir.resolve()
    args.no_nic = True
    args.login_prompt = 'developer ~:'
    args.secondary_disk = None
    args.hotplug_replacement = None
    args.expected_child_failure = False
    workspace = artifacts.ArtifactWorkspace(
        args.output, name=f'guest-scheduler-smp-{args.cpus}vcpu',
        retain=args.retain_artifacts)
    report = {
        'status': 'FAIL', 'cpus': args.cpus, 'accelerator': args.accel,
        'build_dir': str(args.build_dir),
    }
    source_paths = ('kernel/src/scheduler.c', 'kernel/src/smp.c',
                    'kernel/src/vmm.c',
                    'tests/userspace/thread_lifecycle_guest.c',
                    'tests/guest/guest_scheduler_smp_test.py')
    report['source_revision'] = subprocess.run(
        ['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True,
        capture_output=True, check=True).stdout.strip()
    report['source_hashes'] = {
        path: source_hash(ROOT / path) for path in source_paths}
    for name, path in (
            ('pith', args.build_dir / 'Mangrove/pith.elf'),
            ('threadtest', args.build_dir / 'threadtest/threadtest.elf')):
        if path.is_file():
            report.setdefault('build_hashes', {})[name] = source_hash(path)
    try:
        report.update(run(args, workspace))
    except Exception as error:
        report['error'] = f'{type(error).__name__}: {error}'
    finally:
        report = workspace.finish(report,
                                 verified=report.get('status') == 'PASS')
    print(json.dumps(report, sort_keys=True))
    return 0 if report['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
