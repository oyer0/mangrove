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
        'SLAB_NATIVE_BEGIN version=6 allocators=heap,slab,cpu-slab '
        'timing=lfence-rdtsc units=tsc_ticks '
        'requests=16,72,192,384 population=32 transient=16 samples=31 '
        'batch=16 warmups=2 parallel_pairs=512 online=2 tsc_pair_min=28 '
        'free_profile=1 '
        'tick_resolution_ms=1 free_profile_trials=3 free_profile_ops=1024 '
        'free_profile_samples=64 free_profile_stride=16 '
        'free_profile_stages=end_to_end,classify,lock,token_descriptor,'
        'slot_bitmap,mutate_transition,heap_release '
        'smp_rendezvous=bsp-participant-ap-wait '
        'overlap_tracking=per-cpu-sequentially-consistent-flags '
        'log_capacity=524288',
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
    for allocator in ('heap', 'slab', 'cpu-slab'):
        for size in (16, 72, 192, 384):
            for phase in ('alloc', 'free'):
                for sample in range(31):
                    locks = 16 if allocator == 'slab' else 0
                    words = (16 if phase == 'alloc' else 0) \
                        if allocator == 'slab' else 0
                    lines.append(
                        f'SLAB_NATIVE_SAMPLE allocator={allocator} size={size} '
                        f'phase={phase} trial={sample} sample={sample} '
                        f'count=16 tsc_ticks_total=5000 cpu=0 '
                        f'scheduler_tick_delta=0 lock_acquisitions={locks} '
                        f'bitmap_words={words} ownership=pass payload=pass')
    for allocator in ('heap', 'slab', 'cpu-slab'):
        for workload in ('same72', 'mixed'):
            smp = cpus > 1
            mask = 3 if smp else 1
            peak = 2 if smp else 1
            cross = 4 if smp else 0
            allocations = 2560 if smp else 2048
            frees = allocations + 4
            checksum = '1,2,3,4,5' if smp else '1,2,3,4,0'
            run = len([line for line in lines
                       if line.startswith('SLAB_NATIVE_PARALLEL ')])
            lines.append(
                f'SLAB_NATIVE_PARALLEL allocator={allocator} workload={workload} '
                f'run={run} pairs=512 workers=4 started=4 done=4 '
                f'failed=0 allocs={allocations} frees={frees} '
                f'coordinator_pairs={512 if smp else 0} '
                f'coordinator_cpu=0 remote_cpu={1 if smp else 4294967295} '
                f'elapsed_ms={0 if smp else 50} '
                'elapsed_tsc_ticks=50000 elapsed_unit=tsc_ticks '
                f'cpu_mask={mask:x} worker_wait_mask={2 if smp else 0:x} '
                f'peak_active_cpus={peak} peak_active_operations={peak} '
                f'cross_cpu_frees={cross} checksum={checksum}')
            if smp:
                lines.extend((
                    f'SLAB_NATIVE_PARALLEL_CPU allocator={allocator} '
                    f'workload={workload} run={run} cpu=0 seen=1 '
                    'allocs=512 frees=512',
                    f'SLAB_NATIVE_PARALLEL_CPU allocator={allocator} '
                    f'workload={workload} run={run} cpu=1 seen=1 '
                    'allocs=2048 frees=2052'))
            else:
                lines.append(
                    f'SLAB_NATIVE_PARALLEL_CPU allocator={allocator} '
                    f'workload={workload} run={run} cpu=0 seen=1 '
                    'allocs=2048 frees=2052')
            if smp:
                lines.append(
                    f'SLAB_NATIVE_PARALLEL_CROSS allocator={allocator} '
                    f'workload={workload} run={run} owners=0,0,0,0 '
                    'free_cpus=1,1,1,1 ticks=100,101,102,103')
    stage_groups = {
        'nonfinal': ('end_to_end', 'classify', 'lock',
                     'token_descriptor', 'slot_bitmap', 'mutate_transition'),
        'full_to_partial': ('end_to_end', 'mutate_transition'),
        'final_retire': ('end_to_end', 'mutate_transition', 'heap_release'),
    }
    class_indexes = {16: 0, 72: 4, 192: 11, 384: 19}
    for case, stages in stage_groups.items():
        for trial in range(3):
            for stage in stages:
                for request, class_index in class_indexes.items():
                    refills = 1024 if case == 'final_retire' else 0
                    ticks = ','.join(str(1000 + index) for index in range(64))
                    lines.append(
                        f'SLAB_FREE_RAW case={case} trial={trial} '
                        f'stage={stage} request={request} class={class_index} '
                        f'cpu=0 observed=1024 samples=64 lost=0 '
                        'unexpected_lost=0 '
                        f'allocs=1024 frees=1024 refills={refills} '
                        f'retirements={refills} classifier_delta=0 '
                        f'heap_delta=0 ticks={ticks}')
    lines.append('SLAB_FREE_PROFILE_END cases=3 requests=4 trials=3 '
                 'operations_per_case=1024 sampled_attempts_per_trial=64 '
                 'stride=16 zero_tick_deltas_are_counted=1 '
                 'status=PASS')
    for request, class_size in ((16, 16), (72, 80), (192, 192),
                                (384, 384)):
        lines.append(
            f'SLAB_NATIVE_CLASS request={request} class_size={class_size} '
            'lock_acquisitions=100 allocations=100 frees=100 '
            'bitmap_words=100 backing_allocations=4 backing_releases=4 '
            'peak_live_slots=32 live_slots=0 cached_slots=0 cache_hits=20 '
            'cache_misses=4 cache_refills=4 cache_refill_conflicts=0 '
            'cache_refill_objects=8 cache_drain_objects=8 '
            'cache_overflows=0 cross_cpu_frees=4 '
            'partial_slabs=0 full_slabs=0')
    lines.append(f'SLAB_NATIVE_MEMORY heap_used_bytes=100000 '
                 f'cpu_cache_metadata_bytes={cpus * 2560} '
                 'classifier_payload_bytes=12288 '
                 'classifier_peak_payload_bytes=12300 static_root_bytes=4096 '
                 'middle_payload_bytes=4112 leaf_payload_bytes=8176')
    lines.append('SLAB_NATIVE_TRACKER_OVERHEAD timing=lfence-rdtsc '
                 'units=tsc_ticks iterations=512 trials=7 attempts=7 '
                 'tsc_pair_min=28 '
                 'ticks=10000,10001,10002,10003,10004,10005,10006')
    lines.append('SLAB_NATIVE_END samples=744 parallel_runs=6 status=PASS '
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
        assert result['steady_samples'] == 744
        assert result['parallel_runs'] == 6
        assert result['steady']['slab']['72']['alloc']['samples'] == 31
        assert result['memory']['static_root_bytes'] == 4096
        assert result['lifecycle']['first_refill']['tsc_ticks'] == 900
        assert result['free_path']['nonfinal']['classify']['16']['samples'] == 192
        assert result['overlap_tracker_overhead'][
            'iterations_per_trial'] == 512
        assert result['parallel'][0]['failed'] == 0
        assert sum(cpu['allocations'] for cpu in
                   result['parallel'][0]['per_cpu']) == \
            result['parallel'][0]['allocator_allocations']
        assert len(result['parallel'][0]['per_cpu']) == cpus
    no_profile_lines = [line for line in make_log(2).splitlines()
                        if not line.startswith(('SLAB_FREE_RAW ',
                                                'SLAB_FREE_PROFILE_END '))]
    no_profile_log = '\n'.join(no_profile_lines).replace(
        'free_profile=1', 'free_profile=0', 1) + '\n'
    no_profile_log = no_profile_log.replace(
        'log_capacity=524288', 'log_capacity=196608', 1)
    no_profile = check_native_benchmark(no_profile_log, 2,
                                        free_profile=False)
    assert not no_profile['free_profile_enabled']
    log = make_log(2)
    must_reject('profile-build configuration mismatch', lambda:
        check_native_benchmark(log, 2, free_profile=False))
    must_reject('insufficient profile log capacity', lambda:
        check_native_benchmark(log.replace('log_capacity=524288',
                                           'log_capacity=196608', 1), 2))
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
        log.replace('cpu_mask=3 worker_wait_mask=2 peak_active_cpus=2',
                    'cpu_mask=1 worker_wait_mask=0 peak_active_cpus=1', 1), 2))
    cpu_record = next(line for line in log.splitlines()
                      if line.startswith('SLAB_NATIVE_PARALLEL_CPU '))
    must_reject('missing per-CPU operation counts', lambda:
        check_native_benchmark(log.replace(cpu_record + '\n', '', 1), 2))
    must_reject('incorrect per-CPU count', lambda:
        check_native_benchmark(log.replace('cpu=0 seen=1 allocs=512 frees=512',
                                           'cpu=0 seen=1 allocs=0 frees=0',
                                           1), 2))
    must_reject('reported allocator error', lambda:
        check_native_benchmark(log.replace('failed=0 allocs=2560',
                                           'failed=1 allocs=2560', 1), 2))
    must_reject('explicitly rejected native parallel run', lambda:
        check_native_benchmark(
            log + 'SLAB_NATIVE_PARALLEL_REJECT allocator=heap '
            'workload=same72 online=2 cpu_mask=2 peak_active_cpus=1 '
            'cross_cpu_frees=4 failed=0 checksums=1,2,3,4\n', 2))
    must_reject('parallel work outside requested CPUs',
        lambda: check_native_benchmark(
            make_log(1).replace('cpu_mask=1 worker_wait_mask=0',
                                'cpu_mask=2 worker_wait_mask=0', 1), 1))
    must_reject('missing free-path profile', lambda: check_native_benchmark(
        log.replace(next(line for line in log.splitlines()
                         if line.startswith('SLAB_FREE_RAW ')) + '\n', '', 1), 2))
    must_reject('truncated free-path samples', lambda:
        check_native_benchmark(log.replace(
            'ticks=' + ','.join(str(1000 + index) for index in range(64)),
            'ticks=1000,1001', 1), 2))
    first_profile = next(line for line in log.splitlines()
                         if line.startswith('SLAB_FREE_RAW '))
    full_ticks = 'ticks=' + ','.join(str(1000 + index)
                                     for index in range(64))
    shortened_ticks = 'ticks=' + ','.join(str(1000 + index)
                                          for index in range(63))
    tolerated_quantization = log.replace(
        first_profile,
        first_profile.replace('samples=64 lost=0 unexpected_lost=0',
                              'samples=63 lost=1 unexpected_lost=0')
            .replace(full_ticks, shortened_ticks), 1)
    assert check_native_benchmark(tolerated_quantization, 2)[
        'free_path']['nonfinal']['end_to_end']['16'][
            'lost_zero_tick_samples'] == 1
    must_reject('unaccounted profiler loss', lambda:
        check_native_benchmark(
            tolerated_quantization.replace('unexpected_lost=0',
                                           'unexpected_lost=1', 1), 2))
    must_reject('incorrect retirement accounting', lambda:
        check_native_benchmark(log.replace(
            'refills=1024 retirements=1024',
            'refills=1024 retirements=1023', 1), 2))
    must_reject('missing worker completion', lambda:
        check_native_benchmark(log.replace('started=4 done=4',
                                            'started=3 done=4', 1), 2))
    must_reject('no actual overlapping allocator calls', lambda:
        check_native_benchmark(log.replace('peak_active_operations=2',
                                            'peak_active_operations=1', 1), 2))
    must_reject('wrong cross-free CPU', lambda:
        check_native_benchmark(log.replace('free_cpus=1,1,1,1',
                                            'free_cpus=0,1,1,1', 1), 2))
    must_reject('impossible lock counter', lambda: check_native_benchmark(
        log.replace('lock_acquisitions=16 bitmap_words=16',
                    'lock_acquisitions=15 bitmap_words=16', 1), 2))
    must_reject('CPU outside requested guest', lambda: check_native_benchmark(
        log.replace('cpu=0 scheduler_tick_delta=0',
                    'cpu=2 scheduler_tick_delta=0', 1), 2))
    must_reject('truncated output', lambda: check_native_benchmark(
        log.rsplit('SLAB_NATIVE_END', 1)[0], 2))
    must_reject('missing overlap-tracker calibration', lambda:
        check_native_benchmark(log.replace(next(line for line in log.splitlines()
            if line.startswith('SLAB_NATIVE_TRACKER_OVERHEAD ')) + '\n',
            '', 1), 2))
    must_reject('missing class resource record', lambda:
        check_native_benchmark(log.replace(next(line for line in log.splitlines()
            if line.startswith('SLAB_NATIVE_CLASS request=16')) + '\n', '', 1), 2))
    must_reject('impossible heap resource accounting', lambda:
        check_native_benchmark(log.replace('live_slots=0 cached_slots=0',
            'live_slots=1 cached_slots=0', 1), 2))
    must_reject('missing lifecycle timing', lambda: check_native_benchmark(
        log.replace('first_refill_cpu_same=1',
                    'first_refill_cpu_same=0', 1), 2))
    good_identity = {
        'source_revision': 'a' * 40,
        'source_hashes': {path: 'b' * 64 for path in (
            'Makefile', 'include/kprint.h', 'kernel/include/slab.h',
            'kernel/src/heap.c',
            'kernel/src/slab.c',
            'kernel/src/slab_guest_test.c',
            'kernel/src/slab_guest_benchmark.c',
            'tests/guest/guest_slab_test.py',
            'tests/guest/guest_slab_benchmark_evidence_test.py')},
        'kernel_sha256': 'c' * 64,
        'target_compiler': 'gcc 15.2',
        'target_configuration': {
            'SLAB_CORE_DIAGNOSTICS': 1,
            'SLAB_CORE_GUEST_TEST': 1,
            'SLAB_CORE_GUEST_BENCHMARK': 1,
            'SLAB_CORE_GUEST_FREE_PROFILE': 1,
            'SLAB_DEEP_VALIDATION': 0,
            'KERNEL_BOOT_LOG_CAPACITY': 524288,
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
