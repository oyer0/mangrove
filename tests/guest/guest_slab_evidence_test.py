#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check positive and negative controls for native slab SMP evidence."""
import sys
sys.dont_write_bytecode = True
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))

from guest_slab_test import check_concurrency_records


def record(cpu_cache, worker_mask='3', allocator_mask='3', allocations=192,
           arrived=4, first_allocations=4, worker_failed=0,
           terminated=1, rendezvous_timeout=0, passed=1):
    return (
        f'SLAB_GUEST_CONCURRENCY pass={passed} cpu_cache={cpu_cache} '
        f'queued=4 created=4 worker_mask={worker_mask} '
        f'allocator_mask={allocator_mask} allocations={allocations} '
        f'arrived={arrived} first_allocations={first_allocations} '
        f'cross=12 worker_failed={worker_failed} terminated={terminated} '
        f'rendezvous_timeout={rendezvous_timeout}')


def must_reject(label, text, cpus=2):
    try:
        check_concurrency_records(text, cpus)
    except ValueError:
        return
    raise AssertionError(f'accepted invalid concurrency evidence: {label}')


def main():
    good = '\n'.join((record(0), record(1)))
    result = check_concurrency_records(good, 2)
    assert [phase['cpu_cache'] for phase in result] == [False, True]
    assert all(phase['successful_allocations'] == 192 for phase in result)
    assert all(phase['worker_cpu_mask'] == 3 and
               phase['allocator_cpu_mask'] == 3 for phase in result)

    must_reject('allocation mask misses CPU 1', '\n'.join((
        record(0, allocator_mask='1'), record(1))))
    must_reject('worker mask misses CPU 1', '\n'.join((
        record(0, worker_mask='1'), record(1))))
    must_reject('allocation count incomplete', '\n'.join((
        record(0, allocations=191), record(1))))
    must_reject('workers did not rendezvous', '\n'.join((
        record(0, arrived=3), record(1))))
    must_reject('first allocations did not rendezvous', '\n'.join((
        record(0, first_allocations=3), record(1))))
    must_reject('barrier timeout', '\n'.join((
        record(0, rendezvous_timeout=1), record(1))))
    must_reject('worker failure', '\n'.join((
        record(0, worker_failed=1), record(1))))
    must_reject('phase order', '\n'.join((record(1), record(0))))

    one_cpu = '\n'.join((record(0, worker_mask='1', allocator_mask='1'),
                          record(1, worker_mask='1', allocator_mask='1')))
    assert len(check_concurrency_records(one_cpu, 1)) == 2
    must_reject('missing CPU 1 in two-CPU guest', one_cpu, cpus=2)
    print('Native slab SMP evidence controls: PASS')


if __name__ == '__main__':
    main()
