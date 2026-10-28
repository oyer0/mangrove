#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare Mangrove clocks across a controlled QMP VM pause."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "shared"))
sys.path.insert(0, str(ROOT / "tests" / "guest"))
import artifact_lifecycle as artifacts
import guest_mgfs_test as mgfs_guest
from qmp_client import QMP
import qemu_accel

MARKER = re.compile(
    r"\[CLOCK-QMP\] (before|after) mono=([0-9]+) wall_valid=([01]) "
    r"wall=(-?[0-9]+)(?: source=([0-9]+) quality=([0-9]+))?")


def wait_for_marker(serial, label, deadline):
    while time.monotonic() < deadline:
        try:
            text = serial.read_text(errors="replace")
        except FileNotFoundError:
            text = ""
        matches = [match for match in MARKER.finditer(text)
                   if match.group(1) == label]
        if matches:
            match = matches[-1]
            return {
                "monotonic_ms": int(match.group(2)),
                "wall_valid": match.group(3) == "1",
                "wall_seconds": int(match.group(4)),
                "source": int(match.group(5)) if match.group(5) else None,
                "quality": int(match.group(6)) if match.group(6) else None,
            }
        time.sleep(0.05)
    raise TimeoutError(f"timed out waiting for {label} clock marker")


def launch(folder, disk, args, rtc_clock):
    phase = folder / f"{rtc_clock}-guest"
    phase.mkdir()
    vars_path = phase / "vars.fd"
    with vars_path.open("wb") as destination, args.ovmf_vars.open("rb") as source:
        destination.write(source.read())
    invocation = [
        args.qemu, "-machine", "q35", "-accel", args.accel,
        "-cpu", "max" if args.accel == "tcg" else "host",
        "-smp", "2", "-m", "512M",
        "-rtc", f"base=utc,clock={rtc_clock}",
        "-display", "none", "-serial", f"file:{phase / 'serial.log'}",
        "-debugcon", f"file:{phase / 'debugcon.log'}", "-qmp", "stdio",
        "-drive", f"if=pflash,format=raw,readonly=on,file={args.ovmf_code}",
        "-drive", f"if=pflash,format=raw,file={vars_path}",
        "-drive", f"id=usb,file={disk},format=raw,if=none",
        "-device", "qemu-xhci,id=xhci",
        "-device", "usb-storage,id=boot-storage,bus=xhci.0,port=2,drive=usb,bootindex=1",
        "-device", "usb-kbd,id=keyboard,bus=xhci.0,port=1",
        "-no-reboot", "-no-shutdown",
    ]
    (phase / "command.json").write_text(json.dumps(invocation, indent=2) + "\n")
    qemu_log = (phase / "qemu.log").open("wb")
    process = subprocess.Popen(invocation, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=qemu_log,
                               bufsize=0)
    qmp = None
    try:
        qmp = QMP(process, timeout=15)
        serial = phase / "serial.log"
        before = wait_for_marker(serial, "before", time.monotonic() + 90)
        if before["source"] != 1 or before["quality"] != 2:
            raise RuntimeError(
                "Q35 guest did not select the expected full-range HPET source: "
                f"{before}")
        if not before["wall_valid"]:
            raise RuntimeError("test guest did not obtain trusted UTC before pause")

        qmp.call("stop", timeout=10)
        if qmp.query_status() != "paused":
            raise RuntimeError("QMP stop did not leave guest paused")
        pause_started = time.monotonic()
        time.sleep(args.pause_seconds)
        pause_elapsed = time.monotonic() - pause_started
        qmp.call("cont", timeout=10)
        if qmp.query_status() != "running":
            raise RuntimeError("QMP continue did not resume guest")
        after = wait_for_marker(serial, "after", time.monotonic() + 60)
        if not after["wall_valid"]:
            raise RuntimeError("test guest lost trusted UTC after pause")

        mono_delta = after["monotonic_ms"] - before["monotonic_ms"]
        wall_delta = after["wall_seconds"] - before["wall_seconds"]
        if not 4000 <= mono_delta <= 12000:
            raise RuntimeError(f"unexpected monotonic delta: {mono_delta} ms")
        guest_elapsed = mono_delta / 1000.0
        if rtc_clock == "vm":
            if abs(wall_delta - guest_elapsed) > 2.5:
                raise RuntimeError(
                    "clock=vm wall time did not track guest execution: "
                    f"wall={wall_delta}s monotonic={guest_elapsed:.3f}s")
        else:
            expected = guest_elapsed + pause_elapsed
            if abs(wall_delta - expected) > 2.5:
                raise RuntimeError(
                    "clock=host RTC did not include the QMP pause: "
                    f"wall={wall_delta}s expected={expected:.3f}s")

        qmp.call("quit", timeout=10)
        process.wait(timeout=15)
        qmp.drain_events()
        if process.returncode != 0:
            raise RuntimeError(f"QEMU exited with status {process.returncode}")
        qmp.close()
        qmp = None
        qemu_log.close()
        return {
            "rtc_clock": rtc_clock,
            "vcpus": 2,
            "clocksource": "HPET",
            "clocksource_quality": "hardware",
            "before": before,
            "after": after,
            "monotonic_delta_ms": mono_delta,
            "realtime_delta_seconds": wall_delta,
            "host_pause_seconds": pause_elapsed,
            "pause_included_in_realtime": rtc_clock == "host",
        }
    finally:
        if qmp is not None:
            qmp.close()
        if process.poll() is None:
            artifacts.terminate_and_reap(process)
        else:
            process.wait()
        if not qemu_log.closed:
            qemu_log.close()


def check_media(folder, disk, args):
    root = folder / "root-after-pause.img"
    with disk.open("rb") as source, root.open("wb") as destination:
        source.seek(mgfs_guest.ROOT_OFFSET)
        remaining = mgfs_guest.PART_BYTES
        while remaining:
            block = source.read(min(1024 * 1024, remaining))
            if not block:
                raise RuntimeError("short read while extracting guest MGFS")
            destination.write(block)
            remaining -= len(block)
    with (folder / "mgfsck.log").open("wb") as log:
        subprocess.run([str(args.mgfsck), str(root)], cwd=ROOT,
                       stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=120)
    checked = (folder / "mgfsck.log").read_text()
    if "Errors: 0\n" not in checked or "Warnings: 0\n" not in checked:
        raise RuntimeError("independent MGFS check found an error or warning")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    artifacts.add_retention_argument(parser)
    parser.add_argument("--output", type=Path,
                        help="new evidence directory (default: temporary workspace)")
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--test-kernel", type=Path, required=True)
    parser.add_argument("--mgfsck", type=Path, required=True)
    parser.add_argument("--ovmf-code", type=Path, required=True)
    parser.add_argument("--ovmf-vars", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--accel", choices=("kvm", "tcg"), required=True)
    parser.add_argument("--pause-seconds", type=float, default=3.0)
    args = parser.parse_args()
    if qemu_accel.accel_error(args.accel, qemu=args.qemu):
        parser.error(qemu_accel.accel_error(args.accel, qemu=args.qemu))
    if not 2 <= args.pause_seconds <= 10:
        parser.error("--pause-seconds must be between 2 and 10")
    if not args.build_dir.is_dir():
        parser.error(f"build directory does not exist: {args.build_dir}")
    for path in (args.test_kernel, args.mgfsck,
                 args.ovmf_code, args.ovmf_vars):
        if not path.is_file():
            parser.error(f"required file does not exist: {path}")

    workspace = artifacts.ArtifactWorkspace(
        args.output, name="time-pause", retain=args.retain_artifacts)
    args.output = workspace.path
    report = {"status": "FAIL", "accel": args.accel,
              "pause_seconds_requested": args.pause_seconds,
              "runs": []}
    try:
        for rtc_clock in ("vm", "host"):
            run_name = f"clock-{rtc_clock}"
            folder = args.output / run_name
            folder.mkdir()
            disk = mgfs_guest.create_disk(
                folder, args.build_dir, rtc_clock, power_history_smoke=True,
                kernel_override=args.test_kernel)
            report["runs"].append(launch(folder, disk, args, rtc_clock))
            check_media(folder, disk, args)
        pause_gap = (report["runs"][1]["realtime_delta_seconds"] -
                     report["runs"][0]["realtime_delta_seconds"])
        if pause_gap < args.pause_seconds - 2:
            raise RuntimeError(
                "host RTC did not advance materially more than virtual RTC "
                f"across pause (delta difference {pause_gap}s)")
        report["realtime_pause_policy_difference_seconds"] = pause_gap
        report["status"] = "PASS"
    except Exception as error:
        report["error"] = str(error)
        for run in report["runs"]:
            report.setdefault("retained_artifacts", []).extend(
                [f"{run.get('rtc_clock')}-guest/serial.log",
                 f"{run.get('rtc_clock')}-guest/qemu.log"])
        # Preserve concise logs, but never copy the disposable disk image.
        selected = []
        for relative in report.get("retained_artifacts", []):
            path = args.output / relative
            if path.is_file():
                selected.append(path)
        workspace.set_failure_media(*selected)
        raise
    finally:
        report = workspace.finish(
            report, verified=report.get("status") == "PASS")
        print(json.dumps(report, indent=2, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
