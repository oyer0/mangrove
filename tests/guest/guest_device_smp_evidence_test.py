#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Strict host-side validator and negative controls for device SMP records."""
import re


def _record(text, mode):
    lines = text.splitlines()
    if len(lines) != 1 or not lines[0].startswith('DEVSMPS1 '):
        raise ValueError('missing or extra device SMP transcript lines')
    fields = {}
    for item in lines[0].split()[1:]:
        if '=' not in item:
            raise ValueError('malformed device SMP field')
        key, value = item.split('=', 1)
        if not key or key in fields or not value:
            raise ValueError('duplicate or empty device SMP field')
        fields[key] = value
    if fields.get('mode') != mode:
        raise ValueError('device SMP mode differs from requested workload')
    return fields


def _number(fields, key):
    value = fields.get(key)
    if value is None or not re.fullmatch(r'-?(?:0|[1-9][0-9]*)', value):
        raise ValueError('missing or malformed device SMP number: ' + key)
    return int(value)


def validate(text, mode, cpus):
    if mode not in ('storage', 'network') or cpus not in (2, 4):
        raise ValueError('unsupported device SMP validation configuration')
    fields = _record(text, mode)
    expected_mask = (1 << cpus) - 1
    try:
        online_mask = int(fields['online_mask'], 16)
        busy_mask = int(fields['busy_mask'], 16)
    except (KeyError, ValueError) as error:
        raise ValueError('missing or malformed device SMP CPU mask') from error
    if (_number(fields, 'pass') != 1 or _number(fields, 'cpus') != cpus or
            online_mask != expected_mask or busy_mask != expected_mask):
        raise ValueError('device SMP pass or CPU participation evidence failed')
    if (_number(fields, 'spin_workers') != cpus + 1 or
            _number(fields, 'spin_started') != cpus + 1 or
            _number(fields, 'spin_units') <= 0 or
            _number(fields, 'multicore_samples') <= 0 or
            _number(fields, 'overlap_samples') <= 0):
        raise ValueError('concurrent useful-work overlap was not established')
    busy_delta = fields.get('busy_delta', '').split(',')
    if len(busy_delta) != cpus or any(
            not re.fullmatch(r'[1-9][0-9]*', value) for value in busy_delta):
        raise ValueError('per-CPU useful execution evidence is incomplete')
    start_ms = _number(fields, 'start_ms')
    end_ms = _number(fields, 'end_ms')
    if start_ms < 0 or end_ms <= start_ms or _number(fields, 'elapsed_ms') <= 0:
        raise ValueError('device workload interval is incomplete')

    evidence = {
        'mode': mode,
        'cpus': cpus,
        'online_cpu_mask': online_mask,
        'cpu_busy_tick_deltas': [int(value) for value in busy_delta],
        'spin_workers': cpus + 1,
        'useful_work_units': _number(fields, 'spin_units'),
        'multicore_process_samples': _number(fields, 'multicore_samples'),
        'device_overlap_samples': _number(fields, 'overlap_samples'),
        'workload_interval_ms': [start_ms, end_ms],
    }
    if mode == 'storage':
        if (_number(fields, 'io_workers') != cpus or
                _number(fields, 'io_started') != cpus or
                _number(fields, 'io_completed') != cpus or
                _number(fields, 'io_failures') != 0 or
                _number(fields, 'max_active') < 2 or
                _number(fields, 'exact_guest_readback') != 1):
            raise ValueError('concurrent storage completion/readback evidence failed')
        evidence.update(io_workers=cpus,
                        maximum_overlapping_native_io_workers=
                            _number(fields, 'max_active'),
                        exact_guest_readback=True)
    else:
        if (_number(fields, 'timeout_status') != -18 or
                _number(fields, 'status') != 0 or
                _number(fields, 'close_status') != 0 or
                _number(fields, 'replies') != 16 or
                _number(fields, 'reply_bytes') <= 0 or
                fields.get('source') != '10.0.2.2'):
            raise ValueError('network timeout/reply/completion evidence failed')
        rx_before = _number(fields, 'rx_before')
        rx_after = _number(fields, 'rx_after')
        tx_before = _number(fields, 'tx_before')
        tx_after = _number(fields, 'tx_after')
        if (rx_after - rx_before < 16 or tx_after - tx_before < 16):
            raise ValueError('e1000 receive/transmit counters did not advance')
        evidence.update(gateway_replies=16, reply_bytes=_number(fields, 'reply_bytes'),
                        rx_packets=[rx_before, rx_after],
                        tx_packets=[tx_before, tx_after],
                        timed_out_probe_recovered=True)
    return evidence


def negative_controls():
    storage = (
        'DEVSMPS1 mode=storage pass=1 cpus=2 online_mask=3 busy_mask=3 '
        'busy_delta=2,3 spin_workers=3 spin_started=3 spin_units=16384 '
        'multicore_samples=4 overlap_samples=3 start_ms=10 end_ms=20 '
        'elapsed_ms=10 io_workers=2 io_started=2 io_completed=2 '
        'io_failures=0 max_active=2 exact_guest_readback=1')
    network = (
        'DEVSMPS1 mode=network pass=1 cpus=2 online_mask=3 busy_mask=3 '
        'busy_delta=2,3 spin_workers=3 spin_started=3 spin_units=16384 '
        'multicore_samples=4 overlap_samples=3 start_ms=10 end_ms=20 '
        'elapsed_ms=10 timeout_status=-18 status=0 close_status=0 '
        'replies=16 reply_bytes=40 source=10.0.2.2 rx_before=0 rx_after=16 '
        'tx_before=0 tx_after=17')
    validate(storage, 'storage', 2)
    validate(network, 'network', 2)
    cases = [
        (storage.replace('online_mask=3', 'online_mask=1'), 'storage', 2),
        (storage.replace('busy_mask=3', 'busy_mask=1'), 'storage', 2),
        (storage.replace('overlap_samples=3', 'overlap_samples=0'), 'storage', 2),
        (storage.replace('io_completed=2', 'io_completed=1'), 'storage', 2),
        (storage.replace('max_active=2', 'max_active=1'), 'storage', 2),
        (storage.replace('exact_guest_readback=1', 'exact_guest_readback=0'),
         'storage', 2),
        (network.replace('timeout_status=-18', 'timeout_status=0'), 'network', 2),
        (network.replace('replies=16', 'replies=15'), 'network', 2),
        (network.replace('rx_after=16', 'rx_after=15'), 'network', 2),
        (network.replace('overlap_samples=3', 'overlap_samples=0'),
         'network', 2),
    ]
    for transcript, mode, cpus in cases:
        try:
            validate(transcript, mode, cpus)
        except ValueError:
            continue
        raise ValueError('device SMP validator accepted a negative control')
    return {'valid_storage_and_network_records': True,
            'rejected_negative_controls': len(cases)}


if __name__ == '__main__':
    print(negative_controls())
