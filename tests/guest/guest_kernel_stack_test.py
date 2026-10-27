#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prove guarded kernel-stack overflow fail-stops in disposable QEMU media."""
import argparse
import json
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "tests/shared"))
import artifact_lifecycle as artifacts
sys.path.insert(0, str(ROOT / "tests/guest"))
import guest_mgfs_test as mgfs
import qemu_accel


def run_once(folder: Path, args: argparse.Namespace, ordinal: int,
             state: dict) -> dict:
    run_id = secrets.token_hex(8)
    run_folder = folder / run_id
    run_folder.mkdir()
    disk = mgfs.create_disk(run_folder, args.build_dir, run_id)
    state["disk"] = disk
    vars_copy = run_folder / "vars.fd"
    shutil.copyfile(args.ovmf_vars, vars_copy)
    debug_log = run_folder / "debugcon.log"
    serial_log = run_folder / "serial.log"
    qemu_log = run_folder / "qemu.log"
    command = [
        args.qemu, "-machine", "q35", "-accel", args.accel,
        "-cpu", "host" if args.accel == "kvm" else "max",
        "-smp", "2", "-m", "512M", "-rtc", "base=utc,clock=vm",
        "-display", "none", "-serial", f"file:{serial_log}",
        "-debugcon", f"file:{debug_log}",
        "-drive", f"if=pflash,format=raw,readonly=on,file={args.ovmf_code}",
        "-drive", f"if=pflash,format=raw,file={vars_copy}",
        "-drive", f"id=usb,file={disk},format=raw,if=none",
        "-device", "qemu-xhci,id=xhci",
        "-device", "usb-storage,id=boot-storage,bus=xhci.0,port=2,drive=usb,bootindex=1",
        "-device", "usb-kbd,id=keyboard,bus=xhci.0,port=1",
        "-no-reboot", "-no-shutdown", "-d", "guest_errors,cpu_reset",
        "-D", str(qemu_log),
    ]
    (run_folder / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    started = time.monotonic()
    with qemu_log.open("wb") as qemu_output:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.DEVNULL,
                                   stderr=qemu_output)
        try:
            deadline = started + args.timeout
            expected_panic = "PITH_PANIC kernel stack guard page fault"
            text = ""
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(
                        f"QEMU exited {process.returncode} before expected panic")
                log_parts = []
                for log_path in (serial_log, debug_log):
                    if log_path.exists():
                        if log_path.stat().st_size > 8 * 1024 * 1024:
                            raise RuntimeError(
                                "kernel stack guest log exceeded bound")
                        log_parts.append(log_path.read_text(errors="replace"))
                text = "\n".join(log_parts)
                if expected_panic in text:
                    break
                if qemu_log.exists() and qemu_log.stat().st_size > 8 * 1024 * 1024:
                    raise RuntimeError("QEMU diagnostic log exceeded bound")
                time.sleep(0.1)
            else:
                diagnostic = qemu_log.read_text(errors="replace")[-4000:] \
                    if qemu_log.exists() else ""
                raise TimeoutError(
                    "no guarded-stack panic before guest deadline; "
                    f"serial/debug tail={text[-4000:]!r}; "
                    f"QEMU diagnostic tail={diagnostic!r}")
        finally:
            artifacts.terminate_and_reap(process, timeout=10)

    required = (
        "[SMP] final discovered=2 online=2",
        "[STACK-TEST] deliberate guarded thread-stack overflow",
        "[STACK] kernel stack guard fault thread=",
        "PITH_PANIC kernel stack guard page fault",
    )
    missing = [marker for marker in required if marker not in text]
    if missing:
        raise RuntimeError("missing overflow evidence: " + ", ".join(missing))
    stack_stats = {
        kind: {"samples": int(samples), "peak_bytes": int(peak)}
        for kind, samples, peak in re.findall(
            r"\[STACK\] kind=([a-z-]+) samples=(\d+) peak=(\d+) bytes",
            text)
    }
    for kind in ("thread", "ap-startup", "ap-idle", "ist-double-fault"):
        if kind not in stack_stats or stack_stats[kind]["samples"] == 0:
            raise RuntimeError(f"missing runtime stack measurement for {kind}")
    if stack_stats["thread"]["peak_bytes"] < 8 * 1024:
        raise RuntimeError("overflow did not produce a meaningful thread high-water mark")

    root_image = run_folder / "mgfs-root-check.img"
    with disk.open("rb") as source, root_image.open("wb") as destination:
        source.seek(mgfs.ROOT_OFFSET)
        root_bytes = source.read(mgfs.PART_BYTES)
        if len(root_bytes) != mgfs.PART_BYTES:
            raise RuntimeError("disk image ended before the complete MGFS partition")
        destination.write(root_bytes)
    checker_log = run_folder / "mgfsck.log"
    mgfs.check_image(args.build_dir, root_image, checker_log)
    return {
        "run": run_id,
        "ordinal": ordinal,
        "guest_seconds": round(time.monotonic() - started, 3),
        "two_cpus_online": True,
        "guard_fault_classified": True,
        "kernel_failed_stopped": True,
        "stack_usage": stack_stats,
        "mgfs_errors": 0,
        "mgfs_warnings": 0,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    artifacts.add_retention_argument(parser)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--accel", choices=("kvm", "tcg"), default="kvm")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--ovmf-code", type=Path,
                        default=Path("/usr/share/OVMF/OVMF_CODE_4M.fd"))
    parser.add_argument("--ovmf-vars", type=Path,
                        default=Path("/usr/share/OVMF/OVMF_VARS_4M.fd"))
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--repetitions", type=int, default=2)
    args = parser.parse_args()
    if not 30 <= args.timeout <= 300 or not 1 <= args.repetitions <= 4:
        parser.error("timeout must be 30..300 seconds and repetitions 1..4")
    args.build_dir = args.build_dir.resolve()
    for path in (args.ovmf_code, args.ovmf_vars):
        if not path.is_file() or "," in str(path) or "\n" in str(path):
            parser.error(f"invalid firmware path: {path}")
    error = qemu_accel.accel_error(args.accel, qemu=args.qemu)
    if error:
        parser.error(error)

    workspace = artifacts.ArtifactWorkspace(
        args.output, name="guest-kernel-stack", retain=args.retain_artifacts)
    report = {"status": "FAIL", "accel": args.accel, "cpus": 2, "runs": []}
    try:
        for ordinal in range(1, args.repetitions + 1):
            state = {}
            try:
                report["runs"].append(
                    run_once(workspace.path, args, ordinal, state))
            except Exception:
                if state.get("disk") and state["disk"].is_file():
                    workspace.set_failure_media(state["disk"])
                raise
        report["status"] = "PASS"
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        workspace.finish(report, verified=report["status"] == "PASS")
        print(json.dumps(report, indent=2), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
