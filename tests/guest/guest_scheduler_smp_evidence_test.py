#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise scheduler SMP evidence acceptance and missing-witness controls."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tests' / 'guest'))
import guest_scheduler_smp_test as evidence


def records(cpus):
    online = (1 << cpus) - 1
    migration_mask = 1 if cpus == 1 else 3
    transitions = 0 if cpus == 1 else 4
    yields = 0 if cpus == 1 else 32
    migration = (
        f'SCHED_MIGRATION pass=1 cpus={cpus} online_mask={online:x} '
        f'thread=42 samples=100 cpu_mask={migration_mask:x} '
        f'transitions={transitions} cooperative_yields={yields} '
        'thread_stable=1 joined=1 failure=0 start_ms=100 end_ms=2400\n')
    deltas = ','.join('10' for _ in range(cpus))
    multi = 0 if cpus == 1 else 5
    preempt = (
        f'SCHED_PREEMPT pass=1 cpus={cpus} online_mask={online:x} '
        f'workers={cpus + 1} started={cpus + 1} spin_barrier=1 '
        f'cooperative_yields=0 multi_samples={multi} '
        f'busy_mask={online:x} busy_delta={deltas} work_units=1000 '
        'start_ms=200 end_ms=1700\n')
    result = (f'SCHED_SMP_RESULT pass=1 cpus={cpus} migration_status=0 '
              'preemption_status=0 concurrent_children=1\n')
    return migration, preempt, result


def rejects(migration, preempt, result, cpus, name):
    try:
        evidence.validate_native_evidence(migration, preempt, result, cpus)
    except ValueError:
        return
    raise SystemExit(f'negative control was accepted: {name}')


for cpu_count in (1, 2, 4):
    migration, preempt, result = records(cpu_count)
    evidence.validate_native_evidence(migration, preempt, result, cpu_count)

migration, preempt, result = records(2)
rejects(migration.replace('cpu_mask=3 transitions=4',
                          'cpu_mask=1 transitions=0'),
        preempt, result, 2, 'same-thread migration missing')
rejects(migration.replace('cooperative_yields=32',
                          'cooperative_yields=0'),
        preempt, result, 2, 'migration handoff opportunity missing')
rejects(migration, preempt.replace('busy_mask=3 busy_delta=10,10',
                                    'busy_mask=1 busy_delta=10,0'),
        result, 2, 'CPU participation missing')
rejects(migration, preempt.replace('workers=3 started=3',
                                   'workers=2 started=3'),
        result, 2, 'oversubscribed worker missing')
rejects(migration, preempt.replace('cooperative_yields=0',
                                   'cooperative_yields=1'),
        result, 2, 'cooperative-only control')
rejects(migration, preempt.replace('multi_samples=5',
                                   'multi_samples=0'),
        result, 2, 'simultaneous CPU witness missing')
rejects(migration, preempt, result.replace('migration_status=0',
                                            'migration_status=1'),
        2, 'migration child failed')
rejects(migration.replace('end_ms=2400', 'end_ms=150'), preempt,
        result, 2, 'nonoverlapping workloads')

print('scheduler SMP evidence: 1/2/4-vCPU positive controls and migration, '
      'CPU participation, preemption, lifecycle, concurrency, and overlap '
      'negative controls passed')
