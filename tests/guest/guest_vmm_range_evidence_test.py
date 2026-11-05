#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise TLB acknowledgement and frame-lifetime evidence controls."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tests' / 'guest'))
import guest_vmm_range_test as evidence


def log(cpus, ack=None, other=None, blocked=1, reused=1, slab=0,
        online=None):
    mask = (1 << cpus) - 1
    if ack is None:
        ack = mask
    if other is None:
        other = 2 if cpus > 1 else 0
    if online is None:
        online = cpus
    return (f'[SMP] final discovered={cpus} online={online}\n'
            f'VMM_RANGE_GUEST pass=1 online={online} ack={ack:x} '
            f'other_cr3={other:x} release_blocked={blocked} '
            f'frame_reused={reused} slab={slab}\n')


def must_reject(text, cpus, hybrid, name):
    try:
        evidence.validate_boot_log(text, cpus, hybrid)
    except ValueError:
        return
    raise SystemExit(f'negative control was accepted: {name}')


for cpus in (1, 2, 4):
    for hybrid in (False, True):
        witness = evidence.validate_boot_log(log(cpus, slab=int(hybrid)),
                                             cpus, hybrid)
        assert witness['online_cpus'] == cpus
        assert witness['ack_mask'] == (1 << cpus) - 1
        assert witness['frame_reused_after_acknowledged_unmap']

must_reject(log(4, ack=0x7), 4, False, 'missing CPU 3 acknowledgement')
must_reject(log(4, ack=0x1f), 4, False, 'ack from unrequested CPU')
must_reject(log(4, other=0), 4, False, 'no distinct-CR3 remote witness')
must_reject(log(4, blocked=0), 4, False, 'premature mapped-frame release')
must_reject(log(4, reused=0), 4, False, 'frame not reused after shootdown')
must_reject(log(4, online=3), 4, False, 'missing online CPU')
must_reject(log(4).replace('VMM_RANGE_GUEST pass=1',
                            'VMM_RANGE_GUEST pass=0'),
             4, False, 'failed VMM witness')
must_reject('[SMP] final discovered=4 online=4\n', 4, False,
            'missing VMM witness')

print('VMM range evidence: 1/2/4-vCPU positive and TLB/lifetime negative '
      'controls passed')
