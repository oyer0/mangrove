#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate scheduler contention/fairness guest evidence and negative controls."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tests' / 'guest'))
import guest_scheduler_smp_test as evidence


def record(cpus):
    mask = (1 << cpus) - 1
    workers = 2 * cpus + 1
    busy = [10 if i < cpus else 0 for i in range(4)]
    contention = 0 if cpus == 1 else 20
    overlap = 0 if cpus == 1 else 7
    return (
        f'SCHED_CONTENTION pass=1 cpus={cpus} online_mask={mask:x} '
        f'workers={workers} started={workers} completed={workers} '
        'normal_seen=1 background_seen=1 wake_seen=1 normal_priority=0 '
        'background_priority=0 wake_priority=0 normal_latency_ms=5100 '
        'background_latency_ms=5700 wake_latency_ms=750 '
        'wake_ready_ms=260 wake_high_ms=760 '
        f'busy_mask={mask:x} busy_delta={busy[0]},{busy[1]},{busy[2]},{busy[3]} '
        f'cpu_mask={mask:x} overlap_samples={overlap} useful_work=100000 '
        f'lock_contended={contention} lock_wait_iterations={contention * 5} '
        'wakeups=1 timer_preemptions=10 context_switches=100 '
        f'high_dispatches={workers + 4} normal_dispatches=2 '
        'elapsed_ms=5800 timeout=0 queue_validation_samples=50 '
        'queue_valid=1 drained=1\n')


def reject(text, cpus, reason):
    try:
        evidence.validate_contention_evidence(text, cpus)
    except ValueError:
        return
    raise SystemExit(f'negative control accepted: {reason}')


for cpus in (1, 2, 4):
    evidence.validate_contention_evidence(record(cpus), cpus)

text = record(4)
for old, new, reason in (
    ('normal_seen=1', 'normal_seen=0', 'NORMAL starvation'),
    ('background_seen=1', 'background_seen=0', 'BACKGROUND starvation'),
    ('wake_seen=1', 'wake_seen=0', 'lost wakeup'),
    ('started=9 completed=9', 'started=8 completed=9', 'missing worker start'),
    ('started=9 completed=9', 'started=9 completed=8', 'incomplete termination'),
    ('workers=9', 'workers=4', 'no oversubscription'),
    ('normal_priority=0', 'normal_priority=1', 'NORMAL aging transition'),
    ('background_priority=0', 'background_priority=2', 'BACKGROUND aging transition'),
    ('wake_priority=0', 'wake_priority=2', 'wakeup boost transition'),
    ('wake_ready_ms=260', 'wake_ready_ms=0', 'timed waiter never became ready'),
    ('wake_high_ms=760', 'wake_high_ms=5500', 'wakeup priority boost missing'),
    ('normal_latency_ms=5100', 'normal_latency_ms=10', 'priority bypass'),
    ('background_latency_ms=5700', 'background_latency_ms=100', 'BACKGROUND starvation'),
    ('busy_mask=f', 'busy_mask=3', 'insufficient CPU participation'),
    ('busy_delta=10,10,10,10', 'busy_delta=10,0,10,10', 'missing busy CPU'),
    ('cpu_mask=f', 'cpu_mask=3', 'missing worker CPU identity'),
    ('overlap_samples=7', 'overlap_samples=0', 'missing overlap'),
    ('lock_contended=20', 'lock_contended=0', 'missing scheduler lock contention'),
    ('wakeups=1', 'wakeups=0', 'missing wakeup accounting'),
    ('timer_preemptions=10', 'timer_preemptions=0', 'missing timer progress'),
    ('timeout=0', 'timeout=1', 'unbounded wait'),
    ('queue_valid=1', 'queue_valid=0', 'queue invariant failure'),
    ('queue_validation_samples=50', 'queue_validation_samples=0',
     'missing in-load queue validation'),
    ('drained=1', 'drained=0', 'premature cleanup'),
):
    reject(text.replace(old, new, 1), 4, reason)

reject(record(2).replace('overlap_samples=7', 'overlap_samples=0'), 2,
       'two-vCPU overlap absent')
reject(record(2).replace('lock_contended=20', 'lock_contended=0'), 2,
       'two-vCPU lock contention absent')
reject(record(1).replace('overlap_samples=0', 'overlap_samples=1'), 1,
       'impossible one-vCPU overlap')
reject(record(1).replace('lock_contended=0', 'lock_contended=1'), 1,
       'impossible one-vCPU scheduler contention')

print('scheduler contention evidence: 1/2/4-vCPU positive controls and 28 '
      'starvation, wakeup, CPU, overlap, contention, lifecycle and invariant '
      'negative controls passed')
