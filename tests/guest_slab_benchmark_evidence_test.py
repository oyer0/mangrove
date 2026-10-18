#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Negative controls for native slab-benchmark evidence validation."""
import sys
sys.dont_write_bytecode = True
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from guest_slab_test import (check_child_completion, check_native_benchmark,
                             check_source_identity)


def make_log(cpus=2):
    lines = [
        'SLAB_NATIVE_BEGIN version=1 timing=lfence-rdtsc units=tsc_ticks '
        'requests=16,72,192,384 population=32 transient=16 samples=7 '
        'batch=16 warmups=2 parallel_pairs=512 online=2 tsc_pair_min=28 '
        'tick_resolution_ms=1 '
        'preemption_filter=same_cpu_and_unchanged_local_scheduler_tick',
        'SLAB_NATIVE_LIFECYCLE first_refill_cpu_same=1 '
        'first_refill_start_cpu=0 first_refill_end_cpu=0 '
        'first_refill_current_cpu=0 first_refill_start_tsc=100 '
        'first_refill_end_tsc=1000 first_refill_tsc_ticks=900 '
        'first_refill_scheduler_ticks=2 retirement_cpu_same=1 '
        'retirement_start_cpu=0 retirement_end_cpu=0 '
        'retirement_current_cpu=0 retirement_start_tsc=1000 '
        'retirement_end_tsc=1200 retirement_tsc_ticks=200 '
        'retirement_scheduler_ticks=0 heap_used_before=1000 '
        'heap_used_with_slab=6000 heap_used_after_retire=5000 '
        'classifier_retained_before=0 classifier_retained_after=5000',
    ]
    for allocator in ('heap', 'slab'):
        for size in (16, 72, 192, 384):
            for phase in ('alloc', 'free'):
                for sample in range(7):
                    locks = 16 if allocator == 'slab' else 0
                    words = (16 if phase == 'alloc' else 0) \
                        if allocator == 'slab' else 0
                    lines.append(
                        f'SLAB_NATIVE_SAMPLE allocator={allocator} size={size} '
                        f'phase={phase} trial={sample} sample={sample} '
                        f'count=16 tsc_ticks_total=5000 cpu=0 '
                        f'scheduler_tick_delta=0 lock_acquisitions={locks} '
                        f'bitmap_words={words} ownership=pass payload=pass')
    for allocator in ('heap', 'slab'):
        for workload in ('same72', 'mixed'):
            mask = 3 if cpus > 1 else 1
            peak = 2 if cpus > 1 else 1
            cross = 4 if cpus > 1 else 0
            lines.append(
                f'SLAB_NATIVE_PARALLEL allocator={allocator} workload={workload} '
                f'run=0 pairs=512 workers=4 elapsed_ms=50 cpu_mask={mask:x} '
                f'peak_active_cpus={peak} cross_cpu_frees={cross} '
                'checksum=1,2,3,4')
    for request, class_size in ((16, 16), (72, 80), (192, 192),
                                (384, 384)):
        lines.append(
            f'SLAB_NATIVE_CLASS request={request} class_size={class_size} '
            'lock_acquisitions=100 allocations=100 frees=100 '
            'bitmap_words=100 backing_allocations=4 backing_releases=4 '
            'peak_live_slots=32 live_slots=0 partial_slabs=0 full_slabs=0')
    lines.append('SLAB_NATIVE_MEMORY heap_used_bytes=100000 '
                 'classifier_payload_bytes=12288 '
                 'classifier_peak_payload_bytes=12300 static_root_bytes=4096 '
                 'middle_payload_bytes=4112 leaf_payload_bytes=8176')
    lines.append('SLAB_NATIVE_END samples=112 parallel_runs=4 status=PASS '
                 'ownership=pass payload=pass')
    return '\n'.join(lines) + '\n'


def must_reject(label, thunk):
    try:
        thunk()
    except (ValueError, KeyError, IndexError):
        print(f'PASS rejected {label}')
    else:
        raise AssertionError(f'evidence validator accepted {label}')


def main():
    for cpus in (1, 2):
        result = check_native_benchmark(make_log(cpus), cpus)
        assert result['steady_samples'] == 112
        assert result['parallel_runs'] == 4
        assert result['steady']['slab']['72']['alloc']['samples'] == 7
        assert result['memory']['static_root_bytes'] == 4096
        assert result['lifecycle']['first_refill']['tsc_ticks'] == 900
    log = make_log(2)
    must_reject('missing sample', lambda: check_native_benchmark(
        log.replace(next(line for line in log.splitlines()
                         if 'allocator=heap size=16 phase=alloc' in line) + '\n',
                    '', 1), 2))
    sample = next(line for line in log.splitlines()
                  if line.startswith('SLAB_NATIVE_SAMPLE '))
    must_reject('duplicate sample', lambda: check_native_benchmark(
        log.replace(sample + '\n', sample + '\n' + sample + '\n', 1), 2))
    must_reject('incorrect operation count', lambda: check_native_benchmark(
        log.replace('count=16 tsc_ticks_total=5000',
                    'count=15 tsc_ticks_total=5000', 1), 2))
    must_reject('wrong payload', lambda: check_native_benchmark(
        log.replace('payload=pass', 'payload=fail', 1), 2))
    must_reject('overlap failure', lambda: check_native_benchmark(
        log.replace('ownership=pass', 'ownership=fail', 1), 2))
    must_reject('missing CPU participation', lambda: check_native_benchmark(
        log.replace('cpu_mask=3 peak_active_cpus=2',
                    'cpu_mask=1 peak_active_cpus=1', 1), 2))
    must_reject('explicitly rejected native parallel run', lambda:
        check_native_benchmark(
            log + 'SLAB_NATIVE_PARALLEL_REJECT allocator=heap '
            'workload=same72 online=2 cpu_mask=2 peak_active_cpus=1 '
            'cross_cpu_frees=4 failed=0 checksums=1,2,3,4\n', 2))
    must_reject('parallel work outside requested CPUs',
        lambda: check_native_benchmark(
            make_log(1).replace('cpu_mask=1 peak_active_cpus=1',
                                'cpu_mask=2 peak_active_cpus=1', 1), 1))
    must_reject('impossible lock counter', lambda: check_native_benchmark(
        log.replace('lock_acquisitions=16 bitmap_words=16',
                    'lock_acquisitions=15 bitmap_words=16', 1), 2))
    must_reject('CPU outside requested guest', lambda: check_native_benchmark(
        log.replace('cpu=0 scheduler_tick_delta=0',
                    'cpu=2 scheduler_tick_delta=0', 1), 2))
    must_reject('truncated output', lambda: check_native_benchmark(
        log.rsplit('SLAB_NATIVE_END', 1)[0], 2))
    must_reject('missing class resource record', lambda:
        check_native_benchmark(log.replace(next(line for line in log.splitlines()
            if line.startswith('SLAB_NATIVE_CLASS request=16')) + '\n', '', 1), 2))
    must_reject('impossible heap resource accounting', lambda:
        check_native_benchmark(log.replace('live_slots=0 partial_slabs=0',
            'live_slots=1 partial_slabs=0', 1), 2))
    must_reject('missing lifecycle timing', lambda: check_native_benchmark(
        log.replace('first_refill_cpu_same=1',
                    'first_refill_cpu_same=0', 1), 2))
    good_identity = {
        'source_revision': 'a' * 40,
        'source_hashes': {path: 'b' * 64 for path in (
            'Makefile', 'kernel/include/slab.h', 'kernel/src/heap.c',
            'kernel/src/slab.c',
            'kernel/src/slab_guest_test.c',
            'kernel/src/slab_guest_benchmark.c',
            'tests/guest_slab_test.py',
            'tests/guest_slab_benchmark_evidence_test.py')},
        'kernel_sha256': 'c' * 64,
        'target_compiler': 'gcc 15.2',
        'target_configuration': {
            'SLAB_CORE_DIAGNOSTICS': 1,
            'SLAB_CORE_GUEST_TEST': 1,
            'SLAB_CORE_GUEST_BENCHMARK': 1,
            'routing': 'kmalloc/kfree remain indexed-heap-only'},
    }
    check_source_identity(good_identity)
    bad_identity = {**good_identity,
                    'source_hashes': dict(good_identity['source_hashes'])}
    bad_identity['source_hashes'].pop('kernel/src/slab.c')
    must_reject('missing source identity', lambda: check_source_identity(
        bad_identity))
    check_child_completion(b'SLAB_NATIVE_PROCESS_COMPLETED\n',
                           b'child-exit-0-slab\n')
    must_reject('failed child completion', lambda: check_child_completion(
        b'SLAB_NATIVE_PROCESS_COMPLETED\n', b'child-exit-1-slab\n'))
    print('Native slab benchmark evidence controls: PASS')


if __name__ == '__main__':
    main()
