#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Source-matched native storage or e1000 receive workload under SMP KVM."""
import argparse
import hashlib
import json
from pathlib import Path
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
from guest_device_smp_evidence_test import validate, negative_controls

FILE_BYTES = 128 * 1024


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def source_hash(path):
    return sha256((ROOT / path).read_bytes())


def expected_storage_payload(worker, run):
    return bytes((offset * 37 + (offset >> 8) * 11 + worker * 53 +
                  ord(run[offset & 15])) & 255
                 for offset in range(FILE_BYTES))


def stopped_root(disk, folder, build):
    with disk.open('rb') as source:
        source.seek(guest.ROOT_OFFSET)
        data = source.read(guest.PART_BYTES)
    root = folder / 'stopped-root.img'
    root.write_bytes(data)
    guest.check_image(build, root, folder / 'stopped-checker.log')
    return data


def run(args, folder):
    run_id = secrets.token_hex(8)
    folder.mkdir()
    disk = guest.create_disk(
        folder, args.build_dir, run_id,
        device_smp_mode=args.mode, device_smp_cpus=args.cpus)
    (folder / 'initial-disk.sha256').write_text(guest.digest(disk) + '\n')
    runtime = guest.boot(folder, disk, 'device-smp', args)
    if runtime.get('child_status') != 0:
        raise ValueError('native device SMP child did not exit successfully')

    root = stopped_root(disk, folder, args.build_dir)
    boot_record = guest.find(root, '/sys/logs/boot.log')[1]
    boot_log = guest.payload(root, boot_record).decode('ascii', errors='strict')
    boot_smp = guest.parse_smp_boot_log(boot_log, args.cpus)
    if boot_smp['online_cpus'] != args.cpus:
        raise ValueError('SMP boot record online CPU count differs from request')

    pith = guest.payload(root, guest.find(root, '/boot/pith.elf')[1])
    fstest = guest.payload(root, guest.find(root, '/bin/fstest')[1])
    built_pith = (args.build_dir / 'Mangrove/pith.elf').read_bytes()
    built_fstest = (args.build_dir / 'fstest/fstest.elf').read_bytes()
    if pith != built_pith or fstest != built_fstest:
        raise ValueError('embedded Pith or fstest differs from isolated build')

    transcript_path = f'/temp/device-smp-{args.mode}.log'
    transcript = guest.payload(root, guest.find(root, transcript_path)[1])
    text = transcript.decode('ascii', errors='strict')
    (folder / 'device-smp-guest.log').write_text(text)
    evidence = validate(text, args.mode, args.cpus)
    completion = guest.payload(root, guest.find(root, '/temp/device-smp.exit')[1])
    if completion != f'child-exit-0-device-smp-{run_id}\n'.encode():
        raise ValueError('native workload did not return to Shoot successfully')
    evidence['shoot_child_completion'] = True

    if args.mode == 'storage':
        files = []
        for worker in range(args.cpus):
            path = f'/home/developer/smp-storage-{run_id}-{worker}.bin'
            contents = guest.payload(root, guest.find(root, path)[1])
            expected = expected_storage_payload(worker, run_id)
            if contents != expected:
                raise ValueError(f'independent stopped-image data mismatch: {path}')
            files.append({'path': path, 'bytes': len(contents),
                          'sha256': sha256(contents),
                          'exact_content_verified': True})
        evidence['independent_stopped_image_files'] = files
    else:
        interfaces = guest.payload(
            root, guest.find(root, '/temp/device-smp-interfaces.log')[1]
        ).decode('ascii', errors='strict')
        (folder / 'device-smp-interfaces.log').write_text(interfaces)
        fields = dict(item.split('=', 1) for item in text.strip().split()[1:])
        receive_text = (
            f"NET1 source={fields['source']} reply_bytes={fields['reply_bytes']} "
            f"echoes={fields['replies']} timeout_recovered="
            f"{int(fields['timeout_status'] == '-18')} "
            f"rx_before={fields['rx_before']} rx_after={fields['rx_after']} "
            f"tx_before={fields['tx_before']} tx_after={fields['tx_after']}\n")
        evidence['independent_interface_counters'] = \
            guest.validate_network_receive(receive_text, interfaces)

    return {
        'status': 'PASS', 'run': run_id, 'mode': args.mode,
        'cpus_requested': args.cpus, 'accelerator': args.accel,
        'source_revision': subprocess.run(
            ['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True,
            capture_output=True, check=True).stdout.strip(),
        'source_hashes': {
            path: source_hash(path) for path in (
                'kernel/src/block.c', 'kernel/src/irq.c',
                'kernel/src/usb/xhci.c', 'kernel/src/usb/xhci_storage.c',
                'kernel/src/net/e1000.c', 'userspace/fstest/device_smp.c',
                'tests/guest/guest_device_smp_test.py',
                'tests/guest/guest_device_smp_evidence_test.py')
        },
        'embedded_identity': {
            'pith_sha256': sha256(pith),
            'fstest_sha256': sha256(fstest),
            'embedded_pith_matches_isolated_build': True,
            'embedded_fstest_matches_isolated_build': True,
        },
        'smp_boot': {**boot_smp, 'online_indices': list(range(args.cpus))},
        'witness': evidence,
        'guest_runtime': {
            key: runtime[key] for key in (
                'seconds', 'launch_echoed_characters', 'child_status',
                'host_image_sync', 'power_transition_event')
        },
        'independent_mgfs_check': 'PASS on stopped guest image',
        'stopped_disk_sha256': guest.digest(disk),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--mode', choices=('storage', 'network'), required=True)
    parser.add_argument('--cpus', type=int, choices=(2, 4), required=True)
    parser.add_argument('--output', type=Path,
                        help='new retained evidence directory')
    artifacts.add_retention_argument(parser)
    parser.add_argument('--accel', choices=('tcg', 'kvm'),
                        default=guest.default_accel())
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--ovmf-code', type=Path, default=default_path('code'))
    parser.add_argument('--ovmf-vars', type=Path, default=default_path('vars'))
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--negative-control', action='store_true',
                        help='run device SMP evidence-validator controls only')
    args = parser.parse_args()
    error = guest.accel_error(args.accel, args.qemu)
    if error:
        parser.error(error)
    args.build_dir = args.build_dir.resolve()
    if (args.output is not None and args.output.exists()) or not 30 <= args.timeout <= 300:
        parser.error('output must be new and timeout must be 30..300 seconds')
    if args.negative_control:
        print(json.dumps({'status': 'PASS', 'mode': 'validator-controls',
                          **negative_controls()}, indent=2))
        return
    workspace = artifacts.ArtifactWorkspace(
        args.output, name='guest-device-smp', retain=args.retain_artifacts)
    args.no_nic = args.mode == 'storage'
    try:
        report = run(args, workspace.path / f'{args.mode}-{args.cpus}vcpu')
        report['status'] = 'PASS'
    except Exception as error:
        report = {'status': 'FAIL', 'mode': args.mode, 'cpus': args.cpus,
                  'error': str(error)}
        raise
    finally:
        report = workspace.finish(report, verified=report.get('status') == 'PASS')
        print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
