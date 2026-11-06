#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boot one trace-enabled image twice and verify both MGFS boot records."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "shared"))
sys.path.insert(0, str(ROOT / "tests" / "guest"))
sys.path.insert(0, str(ROOT / "tools"))
import artifact_lifecycle as artifacts
import guest_mgfs_test as mgfs
import physical_qualification
from ovmf_firmware import default_path
from qmp_client import QMP
import qemu_accel


def digest(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def type_line(qmp, line, deadline):
    keys = {" ": "spc", "/": "slash", "-": "minus", ".": "dot",
            ">": "shift-dot"}
    for character in line:
        if time.monotonic() >= deadline:
            raise TimeoutError("Shoot command injection exceeded boot deadline")
        key = keys.get(character, character)
        qmp.call("human-monitor-command",
                 {"command-line": f"sendkey {key} 12"})
        time.sleep(0.025)
    qmp.call("human-monitor-command", {"command-line": "sendkey ret 12"})


def verify_shoot_command(root_image, name, expected):
    _, record = mgfs.find(root_image, f"/temp/{name}")
    observed = mgfs.payload(root_image, record)
    if observed != expected:
        raise RuntimeError(f"Shoot command output mismatch for {name}")


def boot_once(args, workspace, disk, variables, run):
    work = workspace / f"boot-{run}"
    work.mkdir()
    serial = work / "serial.log"
    qemu_log = work / "qemu.log"
    boot_deadline = time.monotonic() + args.timeout
    command = [
        args.qemu, "-machine", "q35", "-accel", args.accel,
        "-cpu", "host" if args.accel == "kvm" else "max",
        "-smp", str(args.cpus), "-m", "512M", "-display", "none", "-vga", "none",
        "-device", "VGA,xres=1024,yres=768", "-serial", f"file:{serial}",
        "-qmp", "stdio",
        "-drive", f"if=pflash,format=raw,readonly=on,file={args.ovmf_code}",
        "-drive", f"if=pflash,format=raw,file={variables}",
        "-drive", f"if=none,id=boot,format=raw,file={disk}",
        "-device", "qemu-xhci,id=xhci,p3=0",
        "-device", "usb-storage,drive=boot,bus=xhci.0,port=2,bootindex=1",
        "-device", "usb-kbd,bus=xhci.0,port=1",
    ]
    (work / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    with qemu_log.open("wb") as stderr:
        process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=stderr,
                                   bufsize=0)
        qmp = None
        try:
            qmp = QMP(process, timeout=min(args.timeout, 10))
            tree = qmp.call("human-monitor-command",
                            {"command-line": "info qtree"})
            require = "dev: qemu-xhci" in tree and "dev: usb-storage" in tree
            if not require:
                raise RuntimeError("QEMU did not create the requested xHCI boot disk")
            # The opt-in trace overlay intentionally remains visible over the
            # Shoot prompt, so prompt OCR is not a sound launch test. Wait a
            # bounded guest-settle interval, then prove userspace input by
            # checking the command's persisted MGFS output after QEMU exits.
            settle_deadline = min(boot_deadline,
                                  time.monotonic() + args.interaction_settle)
            while time.monotonic() < settle_deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"guest exited during boot {run}")
                time.sleep(min(0.2, settle_deadline - time.monotonic()))
            if time.monotonic() >= boot_deadline:
                raise TimeoutError(f"boot {run} exceeded {args.timeout}s")
            command_name = f"diagnostic-boot-{run}"
            type_line(qmp, f"say boot-{run} > /temp/{command_name}",
                      boot_deadline)
            time.sleep(1.0)
            screen_path = work / f"boot-{run}.ppm"
            qmp.call("screendump", {"filename": str(screen_path)})
            qmp.call("quit")
            process.wait(timeout=20)
            if process.returncode != 0:
                raise RuntimeError(f"QEMU boot {run} exited {process.returncode}")
        finally:
            if qmp:
                qmp.close()
            artifacts.terminate_and_reap(process)
    return {"boot": run, "status": "INCOMPLETE", "shoot_command_status":
            "INCOMPLETE", "shoot_command_attempted":
            f"say boot-{run} > /temp/diagnostic-boot-{run}",
            "serial_log": str(serial.relative_to(workspace)),
            "screenshot": str((work / f"boot-{run}.ppm").relative_to(workspace))}


def extract_logs(args, disk, workspace, label):
    extraction = workspace / label
    command = [sys.executable, str(ROOT / "tools/extract_boot_diagnostics.py"),
               "--source", str(disk), "--output", str(extraction),
               "--mgfsck", str(args.build_dir / "mgfsck")]
    result = subprocess.run(command, cwd=ROOT, text=True,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, check=False,
                            timeout=180)
    (workspace / f"{label}.log").write_text(result.stdout)
    if result.returncode != 0:
        raise RuntimeError(f"host extraction failed after {label}")
    evidence = json.loads((extraction / "extraction.json").read_text())
    if evidence["mgfs_check"] != "PASS":
        raise RuntimeError(f"MGFS integrity failed after {label}")
    return extraction, evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path,
                        help="new, retained evidence directory (default: temporary)")
    parser.add_argument("--accel", choices=("kvm", "tcg"),
                        default=qemu_accel.default_accel())
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--ovmf-code", type=Path, default=default_path("code"))
    parser.add_argument("--ovmf-vars", type=Path, default=default_path("vars"))
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--cpus", type=int, choices=(1, 2), default=2,
                        help="guest vCPU count for the two-boot persistence run")
    parser.add_argument("--interaction-settle", type=int, default=10,
                        help="bounded seconds before injecting the Shoot command")
    args = parser.parse_args()
    if args.timeout < 30 or args.timeout > 600:
        parser.error("--timeout must be in 30..600 seconds")
    if args.interaction_settle < 3 or args.interaction_settle > 60:
        parser.error("--interaction-settle must be in 3..60 seconds")
    if args.interaction_settle >= args.timeout:
        parser.error("--interaction-settle must be shorter than --timeout")
    args.image = args.image.resolve(strict=True)
    args.build_dir = args.build_dir.resolve(strict=True)
    args.ovmf_code = args.ovmf_code.resolve(strict=True)
    args.ovmf_vars = args.ovmf_vars.resolve(strict=True)
    error = qemu_accel.accel_error(args.accel, qemu=args.qemu)
    if error:
        parser.error(error)

    build = args.build_dir
    required = [build / "EFI/BOOT/BOOTX64.EFI", build / "Mangrove/pith.elf",
                build / "mgfsck"]
    if any(not path.is_file() for path in required):
        parser.error("build directory must contain the loader, Pith, and mgfsck")
    source_identity = physical_qualification._source_identity(ROOT)
    source_hash = source_identity["revision"]
    image_hash = digest(args.image)
    workspace = artifacts.ArtifactWorkspace(
        output=args.output, name="boot-diagnostics")
    disk = workspace.path / "MangroveUSB.img"
    variables = workspace.path / "OVMF_VARS.fd"
    report = {
        "status": "INCOMPLETE", "accel": args.accel, "vcpus": args.cpus,
        "source_revision": source_hash,
        "source_identity": source_identity,
        "build_dir": str(build),
        "image": str(args.image), "image_sha256": image_hash,
        "loader_sha256": digest(required[0]),
        "pith_sha256": digest(required[1]),
        "ovmf_code_sha256": digest(args.ovmf_code),
        "runs": [],
    }
    try:
        required_space = args.image.stat().st_size + 192 * 1024 * 1024
        workspace.require_free_space(required_space,
                                     "two-boot diagnostic verification")
        shutil.copyfile(args.image, disk)
        if digest(disk) != image_hash:
            raise RuntimeError("disposable guest image copy failed SHA-256 check")
        shutil.copyfile(args.ovmf_vars, variables)
        for run in (1, 2):
            record = boot_once(args, workspace.path, disk, variables, run)
            report["runs"].append(record)
            extraction, extracted = extract_logs(
                args, disk, workspace.path, f"extracted-after-boot-{run}")
            logs = extracted["logs"]
            current = logs.get("boot.log", {})
            if current.get("status") != "COMPLETE":
                raise RuntimeError(
                    f"boot {run} log is not complete: {current.get('reason')}")
            root_partition = extracted["root_partition"]
            with disk.open("rb") as source:
                source.seek(root_partition["first_lba"] * 512)
                root_image = source.read(root_partition["bytes"])
            if len(root_image) != root_partition["bytes"]:
                raise RuntimeError(
                    f"short post-stop root read after boot {run}")
            verify_shoot_command(
                root_image, f"diagnostic-boot-{run}", f"boot-{run}\n".encode())
            record["shoot_command_status"] = "PASS"
            record["status"] = "PASS"
            if run == 1:
                report["first_run_log_sha256"] = current["sha256"]
                report["first_run_extraction"] = str(
                    extraction.relative_to(workspace.path))
            else:
                previous = logs.get("boot.previous.log", {})
                if previous.get("status") != "COMPLETE":
                    raise RuntimeError(
                        "boot 1 diagnostic was not preserved as a complete previous log")
                if previous["sha256"] != report["first_run_log_sha256"]:
                    raise RuntimeError(
                        "boot 1 diagnostic bytes changed during boot 2 rotation")
                report["previous_log_preserved"] = True

        # Independent post-stop MGFS parsing proves each userspace command
        # actually ran; echoed input and a visible prompt are not accepted.
        root_partition = extracted["root_partition"]
        with disk.open("rb") as source:
            source.seek(root_partition["first_lba"] * 512)
            root_image = source.read(root_partition["bytes"])
        if len(root_image) != root_partition["bytes"]:
            raise RuntimeError("short post-stop read of the MGFS root partition")
        verify_shoot_command(root_image, "diagnostic-boot-1", b"boot-1\n")
        verify_shoot_command(root_image, "diagnostic-boot-2", b"boot-2\n")
        report["shoot_commands"] = {"status": "PASS", "verified": 2}

        logs = extracted["logs"]
        for name in ("boot.log", "boot.previous.log"):
            if logs.get(name, {}).get("status") != "COMPLETE":
                raise RuntimeError(f"{name} is not a complete diagnostic record")
        current = (extraction / "boot.log.trace.txt").read_text()
        previous = (extraction / "boot.previous.log.trace.txt").read_text()
        for text in (current, previous):
            if ("boot log file prepared" not in text or
                    "payload-write-ms=" not in text or
                    "BOT commands=" not in text or
                    "GOP initial=" not in text or
                    "GOP mode=" not in text):
                raise RuntimeError(
                    "extracted trace lacks persisted completion timings or GOP mode inventory")
        report["extraction"] = {
            "status": "PASS", "root_mgfsck": "PASS",
            "current_log": logs["boot.log"],
            "previous_log": logs["boot.previous.log"],
            "evidence": str(extraction.relative_to(workspace.path)),
        }
        if digest(args.image) != image_hash:
            raise RuntimeError("source image changed during disposable guest runs")
        if physical_qualification._source_identity(ROOT) != source_identity:
            raise RuntimeError("source tree changed during guest qualification")
        disk.unlink()
        variables.unlink()
        report["status"] = "PASS"
    except BaseException as failure:
        report["reason"] = str(failure)
        if disk.exists():
            workspace.set_failure_media(disk)
        raise
    finally:
        workspace.finish(report, verified=report["status"] == "PASS")
    print(json.dumps(report, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
