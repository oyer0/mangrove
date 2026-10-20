#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boot the diagnostic slab core and independently check native guest output."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import secrets
import statistics
import subprocess
import sys
sys.dont_write_bytecode = True
import guest_mgfs_test as guest

PASS = re.compile(
    r'^SLAB_GUEST_PASS classes=(\d+) refill=(\d+) retire=(\d+) ordinary=(\d+) '
    r'cpu_cache=(\d+) leaves=(\d+) online=(\d+) cpumask=([0-9a-fA-F]+) cross=(\d+) '
    r'classifier-payload=(\d+) heap-used-delta=(\d+) '
    r'mapped-heap-delta=(\d+)$', re.M)
SMP = re.compile(
    r'^\[SMP\] (?:final )?discovered=(\d+) online=(\d+)$', re.M)
BEGIN = re.compile(r'^SLAB_GUEST_BEGIN online=(\d+) deep_validation=(\d+)$',
                   re.M)
RECLAIM = re.compile(
    r'^SLAB_GUEST_RECLAIM calls=(\d+) records=(\d+) objects=(\d+) '
    r'pages=(\d+) busy=(\d+) overlap=(\d+) cpumask=([0-9a-fA-F]+) '
    r'record_limit=(\d+) object_limit=(\d+)$',
    re.M)
IRQ_RECLAIM = re.compile(
    r'^SLAB_GUEST_IRQ_RECLAIM pass=(\d+) entry_if=(\d+) result_if=(\d+) '
    r'objects=(\d+)$', re.M)
PUBLIC_HYBRID = re.compile(
    r'^SLAB_GUEST_PUBLIC_API pass=(\d+) classes=(\d+) cutoff=(\d+) '
    r'large_heap=(\d+) zero=(\d+) overflow=(\d+)$', re.M)
PUBLIC_SMP = re.compile(
    r'^SLAB_GUEST_PUBLIC_SMP pass=(\d+) allocations=(\d+) frees=(\d+) '
    r'cpumask=([0-9a-fA-F]+) cross=(\d+) overlap=(\d+)$', re.M)


def check_boot_log(text, cpus, deep_validation=0, hybrid_routing=False,
                   hybrid_cutoff=384):
    if 'SLAB_GUEST_FAIL' in text:
        failures = [line for line in text.splitlines()
                    if 'SLAB_GUEST_FAIL' in line]
        raise ValueError('guest boot recorded a slab failure: ' +
                         '; '.join(failures[-3:]))
    matches = PASS.findall(text)
    if len(matches) != 1:
        raise ValueError('guest boot log lacks one complete slab witness')
    begins = BEGIN.findall(text)
    if len(begins) != 1 or int(begins[0][1]) != deep_validation:
        raise ValueError('guest slab validation mode is missing or mismatched')
    smp_records = SMP.findall(text)
    if not smp_records:
        raise ValueError('guest boot log lacks its final SMP online record')
    reclaim_records = RECLAIM.findall(text)
    if len(reclaim_records) != 1:
        raise ValueError('guest boot log lacks bounded reclaim evidence')
    irq_reclaim_records = IRQ_RECLAIM.findall(text)
    if len(irq_reclaim_records) != 1:
        raise ValueError('guest boot log lacks IRQ-disabled reclaim evidence')
    public_records = PUBLIC_HYBRID.findall(text)
    public_smp_records = PUBLIC_SMP.findall(text)
    if hybrid_routing:
        if (len(public_records) != 1 or
                tuple(map(int, public_records[0])) !=
                (1, 20, hybrid_cutoff, 1, 1, 1)):
            raise ValueError('hybrid guest lacks public allocator routing evidence')
        if len(public_smp_records) != 1:
            raise ValueError('hybrid guest lacks concurrent public allocator evidence')
        public_pass, public_allocations, public_frees, public_mask, public_cross, \
            public_overlap = \
            public_smp_records[0]
        public_mask = int(public_mask, 16)
        public_pass, public_allocations, public_frees, public_cross, \
            public_overlap = map(int, (public_pass, public_allocations,
                                       public_frees, public_cross,
                                       public_overlap))
        if (public_pass != 1 or public_allocations != 192 or
                public_frees != 192 or (public_mask & 1) == 0 or
                (cpus == 2 and ((public_mask & 3) != 3 or
                                public_cross == 0 or public_overlap == 0))):
            raise ValueError('concurrent public allocator counts/CPUs invalid')
    elif public_records:
        raise ValueError('indexed-only guest unexpectedly ran hybrid routing test')
    elif public_smp_records:
        raise ValueError('indexed-only guest unexpectedly ran public SMP routing test')
    discovered, smp_online = map(int, smp_records[-1])
    if discovered < cpus or smp_online < cpus:
        raise ValueError('guest boot log reports fewer online CPUs than requested')
    (classes, refill, retired, ordinary, cpu_cache, leaves, online, cpu_mask, cross,
     classifier_payload, heap_delta, mapped_heap_delta) = matches[0]
    classes, refill, retired, ordinary, cpu_cache, leaves, online, cross, payload, delta = \
        map(int, (classes, refill, retired, ordinary, cpu_cache, leaves, online, cross,
                 classifier_payload, heap_delta))
    mapped_heap_delta = int(mapped_heap_delta)
    mask = int(cpu_mask, 16)
    (reclaim_calls, reclaim_records_examined, reclaim_objects,
     reclaim_pages, reclaim_busy, reclaim_overlap, reclaim_cpu_mask,
     reclaim_record_limit, reclaim_object_limit) = \
        reclaim_records[0]
    reclaim_calls, reclaim_records_examined, reclaim_objects, reclaim_pages, \
        reclaim_busy, reclaim_overlap = map(int, (
            reclaim_calls, reclaim_records_examined, reclaim_objects,
            reclaim_pages, reclaim_busy, reclaim_overlap))
    reclaim_cpu_mask = int(reclaim_cpu_mask, 16)
    reclaim_record_limit = int(reclaim_record_limit)
    reclaim_object_limit = int(reclaim_object_limit)
    irq_reclaim_pass, irq_reclaim_entry_if, irq_reclaim_result_if, \
        irq_reclaim_objects = map(int, irq_reclaim_records[0])
    if (classes != 20 or refill != 1 or retired == 0 or ordinary != 1 or
            cpu_cache != 1 or
            leaves < 2 or online < cpus or payload == 0 or delta < payload):
        raise ValueError('guest slab witness is incomplete or inconsistent')
    if mapped_heap_delta % 4096:
        raise ValueError('guest mapped heap delta is not page-aligned')
    if cpus == 2 and ((mask & 3) != 3 or cross == 0):
        raise ValueError('two-vCPU witness lacks cross-CPU slab activity')
    if (not reclaim_calls or not reclaim_records_examined or
            not reclaim_objects):
        raise ValueError('bounded reclaim witness reports no useful progress')
    if (irq_reclaim_pass != 1 or irq_reclaim_entry_if != 1 or
            irq_reclaim_result_if != 0 or not irq_reclaim_objects):
        raise ValueError('IRQ-disabled reclaim did not preserve IF or reclaim objects')
    if cpus == 2 and ((reclaim_cpu_mask & 3) != 3 or
                      not reclaim_overlap):
        raise ValueError('two-vCPU reclaim lacked CPU participation/overlap')
    return {'classes': classes, 'refill': refill, 'retired_slabs': retired,
            'ordinary_heap_reuse': bool(ordinary), 'classifier_leaves': leaves,
            'cpu_cache_test': bool(cpu_cache),
            'online_cpus': online, 'smp_discovered_cpus': discovered,
            'smp_online_cpus': smp_online, 'cpu_mask': mask,
            'cross_cpu_frees': cross,
            'bounded_reclaim': {
                'calls': reclaim_calls,
                'cache_records_examined': reclaim_records_examined,
                'objects_returned': reclaim_objects,
                'backing_pages_released': reclaim_pages,
                'busy_lock_skips': reclaim_busy,
                'overlapping_allocator_operations': reclaim_overlap,
                'cpu_mask': reclaim_cpu_mask,
                'record_budget_reached_calls': reclaim_record_limit,
                'object_budget_reached_calls': reclaim_object_limit,
            },
            'irq_disabled_reclaim': {
                'entry_interrupts_enabled': bool(irq_reclaim_entry_if),
                'interrupts_remained_disabled': not irq_reclaim_result_if,
                'objects_returned': irq_reclaim_objects,
            },
            'hybrid_public_api_routing': bool(hybrid_routing),
            'hybrid_public_smp': ({
                'allocations': public_allocations,
                'frees': public_frees,
                'cpu_mask': public_mask,
                'cross_cpu_frees': public_cross,
                'overlapping_public_allocator_operations': public_overlap,
            } if hybrid_routing else None),
            'classifier_payload_bytes': payload,
            'heap_used_delta_bytes': delta,
            'mapped_heap_delta_bytes': mapped_heap_delta,
            'boot_log_sha256': hashlib.sha256(text.encode('ascii')).hexdigest()}


def _record(line, prefix):
    if not line.startswith(prefix + ' '):
        raise ValueError(f'malformed {prefix} record')
    fields = {}
    for field in line[len(prefix) + 1:].split():
        if '=' not in field:
            raise ValueError(f'malformed field in {prefix} record')
        key, value = field.split('=', 1)
        if key in fields:
            raise ValueError(f'duplicate field {key} in {prefix} record')
        fields[key] = value
    return fields


def check_native_benchmark(text, cpus, free_profile=None):
    lines = text.splitlines()
    begins = [line for line in lines if line.startswith('SLAB_NATIVE_BEGIN ')]
    ends = [line for line in lines if line.startswith('SLAB_NATIVE_END ')]
    lifecycle = [line for line in lines
                 if line.startswith('SLAB_NATIVE_LIFECYCLE ')]
    samples = [line for line in lines
               if line.startswith('SLAB_NATIVE_SAMPLE ')]
    rejected = [line for line in lines
                if line.startswith('SLAB_NATIVE_REJECTED ')]
    parallel = [line for line in lines
                if line.startswith('SLAB_NATIVE_PARALLEL ')]
    parallel_cpu = [line for line in lines
                    if line.startswith('SLAB_NATIVE_PARALLEL_CPU ')]
    parallel_rejected = [line for line in lines
                         if line.startswith('SLAB_NATIVE_PARALLEL_REJECT ')]
    free_profiles = [line for line in lines
                     if line.startswith('SLAB_FREE_RAW ')]
    free_profile_ends = [line for line in lines
                         if line.startswith('SLAB_FREE_PROFILE_END ')]
    parallel_cross = [line for line in lines
                      if line.startswith('SLAB_NATIVE_PARALLEL_CROSS ')]
    tracker_overheads = [line for line in lines
                         if line.startswith('SLAB_NATIVE_TRACKER_OVERHEAD ')]
    classes = [line for line in lines
               if line.startswith('SLAB_NATIVE_CLASS ')]
    memories = [line for line in lines
                if line.startswith('SLAB_NATIVE_MEMORY ')]
    if len(begins) != 1 or len(ends) != 1 or len(lifecycle) != 1:
        raise ValueError('benchmark begin/end/lifecycle witness is incomplete')
    begin = _record(begins[0], 'SLAB_NATIVE_BEGIN')
    expected_begin = {
        'version': '6', 'allocators': 'heap,slab,cpu-slab',
        'timing': 'lfence-rdtsc', 'units': 'tsc_ticks',
        'requests': '16,72,192,384', 'population': '32', 'transient': '16',
        'samples': '31', 'batch': '16', 'warmups': '2',
        'parallel_pairs': '512',
        'free_profile_trials': '3', 'free_profile_ops': '1024',
        'free_profile_samples': '64', 'free_profile_stride': '16',
        'free_profile_stages':
            'end_to_end,classify,lock,token_descriptor,slot_bitmap,'
            'mutate_transition,heap_release',
        'smp_rendezvous': 'bsp-participant-ap-wait',
        'overlap_tracking': 'per-cpu-sequentially-consistent-flags',
    }
    for key, value in expected_begin.items():
        if begin.get(key) != value:
            raise ValueError(f'benchmark configuration mismatch: {key}')
    profile_enabled = int(begin.get('free_profile', '-1'))
    if profile_enabled not in (0, 1) or (free_profile is not None and
                                         profile_enabled != int(free_profile)):
        raise ValueError('free-path instrumentation configuration is invalid')
    expected_log_capacity = 524288 if profile_enabled else 196608
    if int(begin.get('log_capacity', '0')) != expected_log_capacity:
        raise ValueError('benchmark log capacity does not match its output mode')
    if int(begin.get('online', '0')) < cpus or \
            int(begin.get('tsc_pair_min', '0')) <= 0 or \
            begin.get('tick_resolution_ms') != '1':
        raise ValueError('benchmark timing or CPU metadata is invalid')

    life = _record(lifecycle[0], 'SLAB_NATIVE_LIFECYCLE')
    for prefix in ('first_refill', 'retirement'):
        same = int(life.get(prefix + '_cpu_same', '-1'))
        ticks = int(life.get(prefix + '_tsc_ticks', '-1'))
        start_cpu = int(life.get(prefix + '_start_cpu', '-1'))
        end_cpu = int(life.get(prefix + '_end_cpu', '-1'))
        current_cpu = int(life.get(prefix + '_current_cpu', '-1'))
        if (same not in (0, 1) or start_cpu < 0 or end_cpu < 0 or
                current_cpu < 0 or
                (same == 1 and (ticks <= 0 or start_cpu != end_cpu or
                                end_cpu != current_cpu)) or
                (same == 0 and ticks != 0)):
            raise ValueError(f'{prefix} timing validity is inconsistent')
    if (int(life.get('first_refill_scheduler_ticks', '-1')) < 0 or
            int(life.get('retirement_scheduler_ticks', '-1')) < 0 or
            int(life.get('heap_used_with_slab', '0')) <=
                int(life.get('heap_used_before', '0')) or
            int(life.get('heap_used_after_retire', '0')) <
                int(life.get('heap_used_before', '0')) or
            int(life.get('classifier_retained_after', '0')) <
                int(life.get('classifier_retained_before', '0'))):
        raise ValueError('refill/retirement evidence is inconsistent')
    lifecycle_summary = {
        'first_refill': {
            'timed': int(life['first_refill_cpu_same']),
            'tsc_ticks': int(life['first_refill_tsc_ticks']),
            'scheduler_ticks': int(life['first_refill_scheduler_ticks']),
            'heap_used_before': int(life['heap_used_before']),
            'heap_used_with_slab': int(life['heap_used_with_slab']),
            'classifier_retained_before': int(
                life['classifier_retained_before']),
            'classifier_retained_after': int(
                life['classifier_retained_after']),
        },
        'retirement': {
            'timed': int(life['retirement_cpu_same']),
            'tsc_ticks': int(life['retirement_tsc_ticks']),
            'scheduler_ticks': int(life['retirement_scheduler_ticks']),
            'heap_used_after_retire': int(life['heap_used_after_retire']),
        },
        'timing_units': 'serialized TSC ticks (not calibrated to time)',
    }

    stage_sets = {
        'nonfinal': ('end_to_end', 'classify', 'lock',
                     'token_descriptor', 'slot_bitmap', 'mutate_transition'),
        'full_to_partial': ('end_to_end', 'mutate_transition'),
        'final_retire': ('end_to_end', 'mutate_transition', 'heap_release'),
    }
    class_indexes = {16: 0, 72: 4, 192: 11, 384: 19}
    expected_profile_keys = {
        (case, trial, stage, request)
        for case, stages in stage_sets.items()
        for trial in range(3)
        for stage in stages
        for request in (16, 72, 192, 384)
    } if profile_enabled else set()
    observed_profile_keys = set()
    profile_samples = {}
    profile_trial_samples = {}
    profile_lost_samples = {}
    for line in free_profiles:
        row = _record(line, 'SLAB_FREE_RAW')
        key = (row.get('case'), int(row.get('trial', '-1')),
               row.get('stage'), int(row.get('request', '0')))
        if key in observed_profile_keys or key not in expected_profile_keys:
            raise ValueError('duplicate or unexpected free-path profile record')
        observed_profile_keys.add(key)
        request = key[3]
        ticks = [int(value) for value in row.get('ticks', '').split(',')
                 if value]
        if (int(row.get('class', '-1')) != class_indexes[request] or
                not 0 <= int(row.get('cpu', '-1')) < cpus or
                int(row.get('observed', '-1')) != 1024 or
                not 60 <= int(row.get('samples', '-1')) <= 64 or
                not 0 <= int(row.get('lost', '-1')) <= 4 or
                int(row.get('samples', '-1')) + int(row.get('lost', '-1')) != 64 or
                int(row.get('unexpected_lost', '-1')) != 0 or
                len(ticks) != int(row.get('samples', '-1')) or
                any(value <= 0 for value in ticks) or
                int(row.get('allocs', '-1')) != 1024 or
                int(row.get('frees', '-1')) != 1024):
            raise ValueError('free-path profile counts or samples are invalid')
        refills = int(row.get('refills', '-1'))
        retirements = int(row.get('retirements', '-1'))
        if key[0] == 'final_retire':
            if refills != 1024 or retirements != 1024:
                raise ValueError('final-free profile did not retire every slab')
        elif refills or retirements:
            raise ValueError('steady free profile unexpectedly refilled/retired')
        if (int(row.get('classifier_delta', '-1')) < 0 or
                int(row.get('heap_delta', '-1')) != 0):
            raise ValueError('free-path profile memory accounting is invalid')
        profile_samples.setdefault((key[0], key[2], request), []).extend(ticks)
        profile_trial_samples.setdefault((key[0], key[2], request), {})[
            key[1]] = ticks
        profile_lost_samples[(key[0], key[2], request, key[1])] = int(
            row['lost'])
    if (observed_profile_keys != expected_profile_keys or
            len(free_profile_ends) != profile_enabled):
        raise ValueError('free-path profile set is incomplete or truncated')
    if profile_enabled:
        profile_end = _record(free_profile_ends[0], 'SLAB_FREE_PROFILE_END')
        if (profile_end.get('cases') != '3' or
                profile_end.get('requests') != '4' or
                profile_end.get('trials') != '3' or
                profile_end.get('operations_per_case') != '1024' or
                profile_end.get('sampled_attempts_per_trial') != '64' or
                profile_end.get('stride') != '16' or
                profile_end.get('zero_tick_deltas_are_counted') != '1' or
                profile_end.get('status') != 'PASS'):
            raise ValueError('free-path profile completion record is invalid')
    profile_summary = {}
    for key, values in sorted(profile_samples.items()):
        ordered = sorted(values)
        trials = profile_trial_samples[key]
        profile_summary.setdefault(key[0], {}).setdefault(key[1], {})[
            str(key[2])] = {
                'samples': len(values),
                'trials': len(trials),
                'lost_zero_tick_samples': sum(
                    profile_lost_samples[(key[0], key[1], key[2], trial)]
                    for trial in trials),
                'trial_median_ticks': {
                    str(trial): statistics.median(trial_values)
                    for trial, trial_values in sorted(trials.items())},
                'median_ticks': statistics.median(values),
                'p90_ticks': ordered[int((len(ordered) - 1) * 0.90)],
                'min_ticks': ordered[0],
                'max_ticks': ordered[-1],
            }

    expected_keys = {(allocator, size, phase, sample)
                     for allocator in ('heap', 'slab', 'cpu-slab')
                     for size in (16, 72, 192, 384)
                     for phase in ('alloc', 'free')
                     for sample in range(31)}
    observed = set()
    steady_values = {}
    for line in samples:
        row = _record(line, 'SLAB_NATIVE_SAMPLE')
        key = (row.get('allocator'), int(row.get('size', '0')),
               row.get('phase'), int(row.get('sample', '-1')))
        if key in observed or key not in expected_keys:
            raise ValueError('duplicate or unrequested benchmark sample')
        observed.add(key)
        if (int(row.get('count', '0')) != 16 or
                int(row.get('tsc_ticks_total', '0')) <= 0 or
                not 0 <= int(row.get('cpu', '-1')) < cpus or
                row.get('scheduler_tick_delta') != '0' or
                row.get('ownership') != 'pass' or
                row.get('payload') != 'pass'):
            raise ValueError('sample count, timing, or correctness witness is invalid')
        locks = int(row.get('lock_acquisitions', '-1'))
        words = int(row.get('bitmap_words', '-1'))
        if row['allocator'] == 'heap':
            if locks or words:
                raise ValueError('heap sample claims slab-only work')
        elif row['allocator'] == 'slab' and (locks != 16 or words < 0 or \
                (row['phase'] == 'alloc' and words == 0) or \
                (row['phase'] == 'free' and words != 0)):
            raise ValueError('slab operation counters disagree with sample path')
        elif row['allocator'] == 'cpu-slab' and not (0 <= locks <= 16 and
                                                     words >= 0):
            raise ValueError('CPU cache operation counters are invalid')
        steady_values.setdefault(key[:3], []).append(
            int(row['tsc_ticks_total']) / int(row['count']))
    if observed != expected_keys:
        raise ValueError('benchmark sample set is incomplete')

    expected_parallel = {(allocator, workload)
                         for allocator in ('heap', 'slab', 'cpu-slab')
                         for workload in ('same72', 'mixed')}
    if parallel_rejected:
        row = _record(parallel_rejected[-1],
                      'SLAB_NATIVE_PARALLEL_REJECT')
        raise ValueError(
            'native allocator workload was rejected: '
            f'allocator={row.get("allocator", "?")} '
            f'workload={row.get("workload", "?")} '
            f'cpu_mask={row.get("cpu_mask", "?")} '
            f'peak_active_cpus={row.get("peak_active_cpus", "?")} '
            f'cross_cpu_frees={row.get("cross_cpu_frees", "?")}')
    observed_parallel = set()
    observed_cross = set()
    observed_parallel_cpu = set()
    per_cpu_by_run = {}
    for line in parallel:
        row = _record(line, 'SLAB_NATIVE_PARALLEL')
        key = row.get('allocator'), row.get('workload')
        if key in observed_parallel or key not in expected_parallel:
            raise ValueError('duplicate or unexpected parallel workload')
        observed_parallel.add(key)
        mask = int(row.get('cpu_mask', '0'), 16)
        wait_mask = int(row.get('worker_wait_mask', '0'), 16)
        checksums = [int(value) for value in row.get('checksum', '').split(',')]
        coordinator = int(row.get('coordinator_cpu', '-1'))
        remote = int(row.get('remote_cpu', '-1'))
        run = int(row.get('run', '-1'))
        is_smp = cpus >= 2
        expected_allocs = (5 if is_smp else 4) * 512
        expected_frees = expected_allocs + 4
        elapsed_unit = 'tsc_ticks'
        elapsed = int(row.get('elapsed_tsc_ticks', '0'))
        if (int(row.get('pairs', '0')) != 512 or
                int(row.get('workers', '0')) != 4 or
                int(row.get('started', '-1')) != 4 or
                int(row.get('done', '-1')) != 4 or
                int(row.get('failed', '-1')) != 0 or
                int(row.get('allocs', '-1')) != expected_allocs or
                int(row.get('frees', '-1')) != expected_frees or
                int(row.get('coordinator_pairs', '-1')) !=
                    (512 if is_smp else 0) or
                row.get('elapsed_unit') != elapsed_unit or elapsed <= 0 or
                mask == 0 or
                int(row.get('peak_active_cpus', '0')) < 1 or
                int(row.get('peak_active_cpus', '0')) > cpus or
                int(row.get('peak_active_operations', '0')) < 1 or
                int(row.get('cross_cpu_frees', '-1')) < 0 or
                len(checksums) != 5 or
                any(value == 0 for value in checksums[:4]) or
                ((is_smp and checksums[4] == 0) or
                 (not is_smp and checksums[4] != 0))):
            raise ValueError('parallel work or its integrity evidence is invalid')
        if mask & ~((1 << cpus) - 1):
            raise ValueError('parallel work ran on an unrequested CPU')
        if is_smp and (coordinator < 0 or remote < 0 or coordinator == remote or
                       coordinator >= cpus or remote >= cpus or
                       not (mask & (1 << coordinator)) or
                       not (mask & (1 << remote)) or
                       not (wait_mask & (1 << remote)) or
                       int(row['peak_active_cpus']) < 2 or
                       int(row['peak_active_operations']) < 2 or
                       int(row['cross_cpu_frees']) != 4):
            raise ValueError('two-vCPU run lacks concurrent/cross-CPU evidence')
        if not is_smp and (coordinator < 0 or coordinator >= cpus or
                           remote != 0xffffffff or wait_mask):
            raise ValueError('single-vCPU run has invalid rendezvous evidence')
        run_rows = []
        for cpu_line in parallel_cpu:
            cpu_row = _record(cpu_line, 'SLAB_NATIVE_PARALLEL_CPU')
            cpu_key = (cpu_row.get('allocator'), cpu_row.get('workload'),
                       int(cpu_row.get('run', '-1')))
            if cpu_key != (key[0], key[1], run):
                continue
            cpu = int(cpu_row.get('cpu', '-1'))
            identity = cpu_key + (cpu,)
            if identity in observed_parallel_cpu:
                raise ValueError('duplicate per-CPU allocator record')
            observed_parallel_cpu.add(identity)
            allocs = int(cpu_row.get('allocs', '-1'))
            frees = int(cpu_row.get('frees', '-1'))
            if (cpu < 0 or cpu >= cpus or
                    int(cpu_row.get('seen', '-1')) != 1 or
                    allocs < 0 or frees < 0 or allocs + frees == 0):
                raise ValueError('per-CPU allocator activity record is invalid')
            run_rows.append({'cpu': cpu, 'allocations': allocs,
                             'frees': frees})
        expected_cpus = {cpu for cpu in range(cpus) if mask & (1 << cpu)}
        if ({entry['cpu'] for entry in run_rows} != expected_cpus or
                sum(entry['allocations'] for entry in run_rows) !=
                    expected_allocs or
                sum(entry['frees'] for entry in run_rows) != expected_frees):
            raise ValueError('per-CPU operation counts disagree with parallel totals')
        per_cpu_by_run[(key[0], key[1], run)] = sorted(
            run_rows, key=lambda entry: entry['cpu'])
        observed_cross.add(key)
        if is_smp:
            matching = [entry for entry in parallel_cross
                        if _record(entry, 'SLAB_NATIVE_PARALLEL_CROSS').get(
                            'allocator') == key[0] and
                        _record(entry, 'SLAB_NATIVE_PARALLEL_CROSS').get(
                            'workload') == key[1]]
            if len(matching) != 1:
                raise ValueError('cross-CPU free timing record is missing')
            cross = _record(matching[0], 'SLAB_NATIVE_PARALLEL_CROSS')
            if (int(cross.get('run', '-1')) != int(row.get('run', '-2')) or
                    set(map(int, cross.get('owners', '').split(','))) !=
                        {coordinator} or
                    len(cross.get('owners', '').split(',')) != 4 or
                    len(cross.get('free_cpus', '').split(',')) != 4 or
                    len(cross.get('ticks', '').split(',')) != 4 or
                    any(int(value) != remote for value in
                        cross['free_cpus'].split(',')) or
                    any(int(value) <= 0 for value in cross['ticks'].split(','))):
                raise ValueError('cross-CPU free timing does not match the run')
    if observed_parallel != expected_parallel:
        raise ValueError('same-class or mixed-class SMP workload is missing')
    if len(observed_parallel_cpu) != len(parallel_cpu):
        raise ValueError('orphan or malformed per-CPU allocator evidence')
    expected_classes = {16: 16, 72: 80, 192: 192, 384: 384}
    observed_classes = {}
    for line in classes:
        row = _record(line, 'SLAB_NATIVE_CLASS')
        request = int(row.get('request', '0'))
        if request not in expected_classes or request in observed_classes:
            raise ValueError('duplicate or unexpected class-resource record')
        if (int(row.get('class_size', '0')) != expected_classes[request] or
                int(row.get('lock_acquisitions', '0')) <= 0 or
                int(row.get('allocations', '0')) <= 0 or
                int(row.get('frees', '0')) <= 0 or
                int(row.get('bitmap_words', '0')) <= 0 or
                int(row.get('backing_allocations', '0')) <= 0 or
                int(row.get('backing_allocations', '-1')) !=
                    int(row.get('backing_releases', '-2')) or
                int(row.get('peak_live_slots', '0')) <= 0 or
                int(row.get('live_slots', '-1')) != 0 or
                int(row.get('cached_slots', '-1')) != 0 or
                int(row.get('partial_slabs', '-1')) != 0 or
                int(row.get('full_slabs', '-1')) != 0):
            raise ValueError('class resource accounting is inconsistent')
        observed_classes[request] = row
    if set(observed_classes) != set(expected_classes):
        raise ValueError('class resource records are incomplete')
    if len(memories) != 1:
        raise ValueError('native memory-resource record is missing or duplicated')
    memory = _record(memories[0], 'SLAB_NATIVE_MEMORY')
    if (int(memory.get('heap_used_bytes', '0')) <= 0 or
            int(memory.get('cpu_cache_metadata_bytes', '0')) <= 0 or
            int(memory.get('classifier_payload_bytes', '-1')) <= 0 or
            int(memory.get('classifier_peak_payload_bytes', '0')) <
                int(memory.get('classifier_payload_bytes', '0')) or
            int(memory.get('static_root_bytes', '-1')) != 4096 or
            int(memory.get('middle_payload_bytes', '0')) <= 0 or
            int(memory.get('leaf_payload_bytes', '0')) <= 0):
        raise ValueError('native memory-resource accounting is inconsistent')
    end = _record(ends[0], 'SLAB_NATIVE_END')
    if (end.get('samples') != '744' or end.get('parallel_runs') != '6' or
            end.get('status') != 'PASS' or
            end.get('ownership') != 'pass' or end.get('payload') != 'pass'):
        raise ValueError('benchmark end witness is incomplete')
    if len(tracker_overheads) != 1:
        raise ValueError('benchmark overlap-tracker calibration is missing')
    tracker = _record(tracker_overheads[0],
                      'SLAB_NATIVE_TRACKER_OVERHEAD')
    tracker_ticks = [int(value) for value in tracker.get('ticks', '').split(',')
                     if value]
    timer_pair_min = int(begin['tsc_pair_min'])
    if (tracker.get('timing') != 'lfence-rdtsc' or
            tracker.get('units') != 'tsc_ticks' or
            int(tracker.get('iterations', '-1')) != 512 or
            int(tracker.get('trials', '-1')) != 7 or
            not 7 <= int(tracker.get('attempts', '0')) <= 56 or
            int(tracker.get('tsc_pair_min', '-1')) != timer_pair_min or
            len(tracker_ticks) != 7 or
            any(value <= timer_pair_min for value in tracker_ticks)):
        raise ValueError('overlap-tracker calibration is invalid')
    tracker_summary = {
        'iterations_per_trial': 512,
        'trials': len(tracker_ticks),
        'timer_pair_min_ticks': timer_pair_min,
        'total_ticks': tracker_ticks,
        'ticks_per_empty_bracket': [value / 512
                                    for value in tracker_ticks],
        'median_ticks_per_empty_bracket': statistics.median(
            value / 512 for value in tracker_ticks),
    }
    for line in rejected:
        row = _record(line, 'SLAB_NATIVE_REJECTED')
        if (row.get('allocator') not in ('heap', 'slab', 'cpu-slab') or
                int(row.get('size', '0')) not in (16, 72, 192, 384) or
                int(row.get('alloc', '-1')) < 0 or
                int(row.get('free', '-1')) < 0 or
                int(row.get('attempts', '0')) < 7):
            raise ValueError('invalid rejected timing-window record')
    steady_summary = {}
    for (allocator, size, phase), values in sorted(steady_values.items()):
        steady_summary.setdefault(allocator, {}).setdefault(str(size), {})[
            phase] = {
                'samples': len(values),
                'operations_per_sample': 16,
                'median_tsc_ticks_per_operation': statistics.median(values),
                'min_tsc_ticks_per_operation': min(values),
                'max_tsc_ticks_per_operation': max(values),
            }
    parallel_summary = []
    for line in parallel:
        row = _record(line, 'SLAB_NATIVE_PARALLEL')
        parallel_summary.append({
            'allocator': row['allocator'], 'workload': row['workload'],
            'pairs_per_worker': int(row['pairs']),
            'workers': int(row['workers']),
            'elapsed_ms': int(row['elapsed_ms']),
            'elapsed_tsc_ticks': int(row['elapsed_tsc_ticks']),
            'elapsed_unit': row['elapsed_unit'],
            'started': int(row['started']), 'done': int(row['done']),
            'allocator_allocations': int(row['allocs']),
            'allocator_frees': int(row['frees']),
            'cpu_mask': int(row['cpu_mask'], 16),
            'worker_wait_mask': int(row['worker_wait_mask'], 16),
            'peak_active_cpus': int(row['peak_active_cpus']),
            'peak_active_operations': int(row['peak_active_operations']),
            'coordinator_cpu': int(row['coordinator_cpu']),
            'remote_cpu': int(row['remote_cpu']),
            'cross_cpu_frees': int(row['cross_cpu_frees']),
            'failed': int(row['failed']),
            'per_cpu': per_cpu_by_run[(row['allocator'], row['workload'],
                                       int(row['run']))],
            'checksums': [int(value) for value in row['checksum'].split(',')],
        })
    if cpus >= 2 and len(parallel_cross) != 6:
        raise ValueError('cross-CPU timing record set is incomplete')
    return {'steady_samples': len(samples), 'steady': steady_summary,
            'free_profile_enabled': bool(profile_enabled),
            'lifecycle': lifecycle_summary,
            'parallel_runs': len(parallel), 'parallel': parallel_summary,
            'free_path': profile_summary,
            'overlap_tracker_overhead': tracker_summary,
            'classes': {str(key): {
                'class_size': int(value['class_size']),
                'lock_acquisitions': int(value['lock_acquisitions']),
                'allocations': int(value['allocations']),
                'frees': int(value['frees']),
                'bitmap_words': int(value['bitmap_words']),
                'backing_allocations': int(value['backing_allocations']),
                'backing_releases': int(value['backing_releases']),
                'peak_live_slots': int(value['peak_live_slots']),
                'live_slots': int(value['live_slots']),
                'cached_slots': int(value['cached_slots']),
                'cache_hits': int(value['cache_hits']),
                'cache_misses': int(value['cache_misses']),
                'cache_refills': int(value['cache_refills']),
                'cache_refill_conflicts': int(value['cache_refill_conflicts']),
                'cache_refill_objects': int(value['cache_refill_objects']),
                'cache_drain_objects': int(value['cache_drain_objects']),
                'cache_overflows': int(value['cache_overflows']),
                'cross_cpu_frees': int(value['cross_cpu_frees']),
            } for key, value in observed_classes.items()},
            'memory': {key: int(value) for key, value in memory.items()},
            'rejected_window_records': len(rejected),
            'timing_units': 'serialized TSC ticks (not calibrated to time)',
            'benchmark_log_sha256': hashlib.sha256(
                text.encode('ascii')).hexdigest()}


def check_child_completion(native_output, exit_output):
    if native_output != b'SLAB_NATIVE_PROCESS_COMPLETED\n':
        raise ValueError('native guest process output is missing or incorrect')
    if exit_output != b'child-exit-0-slab\n':
        raise ValueError('native guest completion command did not run')


def check_source_identity(report):
    revision = report.get('source_revision', '')
    if not re.fullmatch(r'[0-9a-f]{40}', revision):
        raise ValueError('benchmark source revision is missing or malformed')
    required = ('Makefile', 'include/kprint.h', 'kernel/include/slab.h',
                'kernel/src/heap.c',
                'kernel/src/slab.c',
                'kernel/src/slab_guest_test.c',
                'kernel/src/slab_guest_benchmark.c',
                'tests/guest_slab_test.py',
                'tests/guest_slab_benchmark_evidence_test.py')
    hashes = report.get('source_hashes', {})
    for path in required:
        if not re.fullmatch(r'[0-9a-f]{64}', hashes.get(path, '')):
            raise ValueError(f'benchmark source hash missing for {path}')
    if not re.fullmatch(r'[0-9a-f]{64}', report.get('kernel_sha256', '')):
        raise ValueError('benchmark target binary hash is missing')
    config = report.get('target_configuration', {})
    profile_enabled = config.get('SLAB_CORE_GUEST_FREE_PROFILE')
    expected_capacity = 524288 if profile_enabled == 1 else 196608
    if (config.get('SLAB_CORE_DIAGNOSTICS') != 1 or
            config.get('SLAB_CORE_GUEST_TEST') != 1 or
            config.get('SLAB_CORE_GUEST_BENCHMARK') != 1 or
            config.get('SLAB_CORE_GUEST_FREE_PROFILE') not in (0, 1) or
            config.get('SLAB_DEEP_VALIDATION') not in (0, 1) or
            config.get('KERNEL_BOOT_LOG_CAPACITY') != expected_capacity or
            config.get('routing') != 'kmalloc/kfree remain indexed-heap-only' or
            not report.get('target_compiler')):
        raise ValueError('benchmark target configuration is incomplete')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cpus', type=int, choices=(1, 2), required=True)
    parser.add_argument('--accel', choices=('tcg', 'kvm'), default='tcg')
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--ovmf-code', type=Path,
                        default=Path('/usr/share/OVMF/OVMF_CODE_4M.fd'))
    parser.add_argument('--ovmf-vars', type=Path,
                        default=Path('/usr/share/OVMF/OVMF_VARS_4M.fd'))
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--benchmark', action='store_true')
    parser.add_argument('--free-profile', action='store_true')
    parser.add_argument('--hybrid-routing', action='store_true',
                        help='require public kmalloc/kfree slab routing evidence')
    parser.add_argument('--hybrid-cutoff', type=int,
                        choices=(256, 288, 320, 352, 384), default=384,
                        help='compiled hybrid request cutoff for routing checks')
    parser.add_argument('--deep-validation', type=int, choices=(0, 1),
                        default=0)
    args = parser.parse_args()
    if args.free_profile and not args.benchmark:
        parser.error('--free-profile requires --benchmark')
    args.build_dir = args.build_dir.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)

    report = {'status': 'FAIL', 'cpus': args.cpus, 'accel': args.accel}
    try:
        run = secrets.token_hex(8)
        folder = args.output / 'mgfs-0'
        folder.mkdir()
        disk = guest.create_disk(folder, args.build_dir, run)

        with disk.open('rb') as source:
            source.seek(guest.ROOT_OFFSET)
            image = bytearray(source.read(guest.PART_BYTES))
        native_script = (
            '/bin/say SLAB_NATIVE_PROCESS_COMPLETED > /temp/slab-native.log\n'
            '/bin/say child-exit-0-slab > /temp/slab.exit\n'
            '/bin/shutdown\n').encode('ascii')
        guest.install(image, 6, 'slab.shoot', native_script)
        prepared = folder / 'slab-prepared-root.img'
        prepared.write_bytes(image)
        with disk.open('r+b') as media:
            media.seek(guest.ROOT_OFFSET)
            media.write(image)
        guest.check_image(args.build_dir, prepared,
                          folder / 'slab-prepared-checker.log')

        runtime = guest.boot(folder, disk, 'slab', args)
        report.update({'run': run, 'guest_runtime': runtime})
        with disk.open('rb') as source:
            source.seek(guest.ROOT_OFFSET)
            stopped_image = source.read(guest.PART_BYTES)
        stopped = folder / 'slab-stopped-root.img'
        stopped.write_bytes(stopped_image)
        guest.check_image(args.build_dir, stopped,
                          folder / 'slab-stopped-checker.log')
        report.update({
            'stopped_image_sha256': guest.digest(disk),
            'independent_checker': 'mgfsck clean before and after guest execution',
        })

        _, boot_record = guest.find(stopped_image, '/sys/logs/boot.log')
        boot_log = guest.payload(stopped_image, boot_record).decode('ascii')
        (folder / 'boot.log').write_text(boot_log)
        smp_records = SMP.findall(boot_log)
        if smp_records:
            discovered, online = map(int, smp_records[-1])
            report['smp'] = {'discovered_cpus': discovered,
                             'online_cpus': online}
        report['boot_log_sha256'] = hashlib.sha256(
            boot_log.encode('ascii')).hexdigest()
        _, native_record = guest.find(stopped_image, '/temp/slab-native.log')
        native_output = guest.payload(stopped_image, native_record)
        _, exit_record = guest.find(stopped_image, '/temp/slab.exit')
        exit_output = guest.payload(stopped_image, exit_record)
        report.update({'native_process_output': native_output.decode(
            'ascii', errors='replace'),
            'native_exit_output': exit_output.decode(
                'ascii', errors='replace')})
        check_child_completion(native_output, exit_output)

        source_metadata = {
            'source_revision': subprocess.run(
                ['git', 'rev-parse', 'HEAD'], cwd=Path(__file__).resolve().parents[1],
                text=True, capture_output=True, check=True).stdout.strip(),
            'source_hashes': {
                relative: hashlib.sha256((Path(__file__).resolve().parents[1] /
                                          relative).read_bytes()).hexdigest()
                for relative in ('Makefile', 'include/kprint.h',
                                 'kernel/include/slab.h',
                                 'kernel/src/heap.c',
                                 'kernel/src/init.c',
                                 'kernel/src/slab.c',
                                 'kernel/src/slab_guest_test.c',
                                 'kernel/src/slab_guest_benchmark.c',
                                 'tests/guest_slab_test.py',
                                 'tests/guest_slab_benchmark_evidence_test.py')},
            'target_compiler': subprocess.run(
                ['gcc', '--version'], text=True, capture_output=True,
                check=True).stdout.splitlines()[0],
            'target_configuration': {
                'SLAB_CORE_DIAGNOSTICS': 1,
                'SLAB_CORE_GUEST_TEST': 1,
                'KERNEL_HYBRID_ALLOCATOR': int(args.hybrid_routing),
                'SLAB_CORE_GUEST_BENCHMARK': int(args.benchmark),
                'SLAB_CORE_GUEST_FREE_PROFILE': int(args.free_profile),
                'SLAB_DEEP_VALIDATION': args.deep_validation,
                'KERNEL_BOOT_LOG_CAPACITY':
                    (524288 if args.free_profile else 196608)
                    if args.benchmark else 65536,
                'kernel_optimization': 'GCC default (-O0; no -O flag in KERNEL_CFLAGS)',
                'routing': ('small kmalloc requests use the slab backend'
                            if args.hybrid_routing else
                            'kmalloc/kfree remain indexed-heap-only')},
        }
        report.update(source_metadata)
        report['kernel_sha256'] = guest.digest(
            args.build_dir / 'Mangrove/pith.elf')
        if args.benchmark:
            check_source_identity({**source_metadata,
                'kernel_sha256': report['kernel_sha256']})

        try:
            witness = check_boot_log(boot_log, args.cpus,
                                     args.deep_validation,
                                    args.hybrid_routing,
                                    args.hybrid_cutoff)
        except ValueError as boot_error:
            if args.benchmark:
                try:
                    check_native_benchmark(boot_log, args.cpus)
                except ValueError as benchmark_error:
                    raise ValueError(f'{boot_error}; benchmark evidence: '
                                     f'{benchmark_error}') from benchmark_error
            raise
        benchmark_witness = None
        if args.benchmark:
            benchmark_witness = check_native_benchmark(
                boot_log, args.cpus, args.free_profile)

        # Negative control: the same evidence parser must reject a stopped
        # guest log with its slab witness removed.
        try:
            check_boot_log(PASS.sub('', boot_log), args.cpus,
                           args.deep_validation, args.hybrid_routing)
        except ValueError:
            negative_control = 'missing slab witness rejected'
        else:
            raise ValueError('slab evidence negative control was accepted')
        try:
            check_boot_log(RECLAIM.sub('', boot_log), args.cpus,
                           args.deep_validation, args.hybrid_routing)
        except ValueError:
            reclaim_negative_control = 'missing bounded reclaim witness rejected'
        else:
            raise ValueError('bounded reclaim evidence negative control was accepted')
        try:
            check_boot_log(IRQ_RECLAIM.sub('', boot_log), args.cpus,
                           args.deep_validation, args.hybrid_routing)
        except ValueError:
            irq_reclaim_negative_control = \
                'missing IRQ-disabled reclaim witness rejected'
        else:
            raise ValueError('IRQ-disabled reclaim evidence negative control was accepted')
        hybrid_routing_negative_control = None
        if args.hybrid_routing:
            try:
                check_boot_log(PUBLIC_HYBRID.sub('', boot_log), args.cpus,
                               args.deep_validation, True)
            except ValueError:
                hybrid_routing_negative_control = \
                    'missing hybrid public-api witness rejected'
            else:
                raise ValueError('hybrid routing evidence negative control was accepted')
            public_smp_negative_control = None
            try:
                check_boot_log(PUBLIC_SMP.sub('', boot_log), args.cpus,
                               args.deep_validation, True)
            except ValueError:
                public_smp_negative_control = \
                    'missing hybrid public-SMP witness rejected'
            else:
                raise ValueError('hybrid public-SMP negative control was accepted')
        else:
            public_smp_negative_control = None

        report.update({
            'status': 'PASS', 'run': run,
            'native_process_output': native_output.decode('ascii').strip(),
            'negative_controls': [negative_control,
                                  reclaim_negative_control,
                                  irq_reclaim_negative_control] +
                ([hybrid_routing_negative_control]
                 if hybrid_routing_negative_control else []) +
                ([public_smp_negative_control]
                 if public_smp_negative_control else []),
            'smp': {'discovered_cpus': witness['smp_discovered_cpus'],
                    'online_cpus': witness['smp_online_cpus']},
            'witness': witness,
            'native_benchmark': benchmark_witness,
            **source_metadata,
            'guest_runtime': runtime,
            'stopped_image_sha256': guest.digest(disk),
            'independent_checker': 'mgfsck clean before and after guest execution',
        })
    except Exception as error:
        report['error'] = f'{type(error).__name__}: {error}'
        raise
    finally:
        (args.output / 'report.json').write_text(
            json.dumps(report, indent=2, sort_keys=True) + '\n')
    print(json.dumps(report, sort_keys=True))


if __name__ == '__main__':
    main()
