#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boot the diagnostic slab core and independently check native guest output."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import secrets
import sys
sys.dont_write_bytecode = True
import guest_mgfs_test as guest

PASS = re.compile(
    r'^SLAB_GUEST_PASS classes=(\d+) refill=(\d+) retire=(\d+) ordinary=(\d+) '
    r'leaves=(\d+) online=(\d+) cpumask=([0-9a-fA-F]+) cross=(\d+) '
    r'classifier-payload=(\d+) heap-used-delta=(\d+) '
    r'mapped-heap-delta=(\d+)$', re.M)
SMP = re.compile(
    r'^\[SMP\] (?:final )?discovered=(\d+) online=(\d+)$', re.M)


def check_boot_log(text, cpus):
    if 'SLAB_GUEST_FAIL' in text:
        raise ValueError('guest boot recorded a slab failure')
    matches = PASS.findall(text)
    if len(matches) != 1:
        raise ValueError('guest boot log lacks one complete slab witness')
    smp_records = SMP.findall(text)
    if not smp_records:
        raise ValueError('guest boot log lacks its final SMP online record')
    discovered, smp_online = map(int, smp_records[-1])
    if discovered < cpus or smp_online < cpus:
        raise ValueError('guest boot log reports fewer online CPUs than requested')
    (classes, refill, retired, ordinary, leaves, online, cpu_mask, cross,
     classifier_payload, heap_delta, mapped_heap_delta) = matches[0]
    classes, refill, retired, ordinary, leaves, online, cross, payload, delta = \
        map(int, (classes, refill, retired, ordinary, leaves, online, cross,
                 classifier_payload, heap_delta))
    mapped_heap_delta = int(mapped_heap_delta)
    mask = int(cpu_mask, 16)
    if (classes != 20 or refill != 1 or retired == 0 or ordinary != 1 or
            leaves < 2 or online < cpus or payload == 0 or delta < payload):
        raise ValueError('guest slab witness is incomplete or inconsistent')
    if mapped_heap_delta % 4096:
        raise ValueError('guest mapped heap delta is not page-aligned')
    if cpus == 2 and ((mask & 3) != 3 or cross == 0):
        raise ValueError('two-vCPU witness lacks cross-CPU slab activity')
    return {'classes': classes, 'refill': refill, 'retired_slabs': retired,
            'ordinary_heap_reuse': bool(ordinary), 'classifier_leaves': leaves,
            'online_cpus': online, 'smp_discovered_cpus': discovered,
            'smp_online_cpus': smp_online, 'cpu_mask': mask,
            'cross_cpu_frees': cross,
            'classifier_payload_bytes': payload,
            'heap_used_delta_bytes': delta,
            'mapped_heap_delta_bytes': mapped_heap_delta,
            'boot_log_sha256': hashlib.sha256(text.encode('ascii')).hexdigest()}


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
    args = parser.parse_args()
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
        with disk.open('rb') as source:
            source.seek(guest.ROOT_OFFSET)
            stopped_image = source.read(guest.PART_BYTES)
        stopped = folder / 'slab-stopped-root.img'
        stopped.write_bytes(stopped_image)
        guest.check_image(args.build_dir, stopped,
                          folder / 'slab-stopped-checker.log')

        _, boot_record = guest.find(stopped_image, '/sys/logs/boot.log')
        boot_log = guest.payload(stopped_image, boot_record).decode('ascii')
        witness = check_boot_log(boot_log, args.cpus)
        _, native_record = guest.find(stopped_image, '/temp/slab-native.log')
        native_output = guest.payload(stopped_image, native_record)
        if native_output != b'SLAB_NATIVE_PROCESS_COMPLETED\n':
            raise ValueError('native guest process output is missing or incorrect')
        _, exit_record = guest.find(stopped_image, '/temp/slab.exit')
        if guest.payload(stopped_image, exit_record) != b'child-exit-0-slab\n':
            raise ValueError('native guest completion command did not run')

        # Negative control: the same evidence parser must reject a stopped
        # guest log with its slab witness removed.
        try:
            check_boot_log(PASS.sub('', boot_log), args.cpus)
        except ValueError:
            negative_control = 'missing slab witness rejected'
        else:
            raise ValueError('slab evidence negative control was accepted')

        report.update({
            'status': 'PASS', 'run': run,
            'kernel_sha256': guest.digest(args.build_dir / 'Mangrove/pith.elf'),
            'native_process_output': native_output.decode('ascii').strip(),
            'negative_control': negative_control,
            'smp': {'discovered_cpus': witness['smp_discovered_cpus'],
                    'online_cpus': witness['smp_online_cpus']},
            'witness': witness,
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
