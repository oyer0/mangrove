#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Serial MTS orchestration. Existing tests retain their own assertions/oracles."""
import argparse
from dataclasses import dataclass
import json
import hashlib
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time
import atexit

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1] / "shared"))
import artifact_lifecycle as artifacts
import qemu_accel
HOST = ("time", "terminal", "clipboard", "reed", "shoot", "list", "mgfs-version", "password", "random", "vmm", "idt", "pmm", "heap", "slab", "net",
        "libc", "fsstress", "history", "mfqs", "sync", "storage", "dev-tools", "mts")
MEDIA = ("mkfs.fat", "fsck.fat", "mcopy", "mdir", "mtype", "mkfs.exfat", "fsck.exfat")
# These are actual paths in the unchanged guest runners, not PATH guesses.
GUEST_MEDIA = tuple("/usr/sbin/" + name for name in
                    ("sgdisk", "mkfs.fat", "fsck.fat", "mkfs.exfat", "fsck.exfat"))
LOG_LIMIT = 64 * 1024 * 1024


@dataclass
class Group:
    name: str
    command: list
    seconds: int = 1800
    tools: tuple = ()
    receipt: dict | None = None
    skip: str = ""


def plan(profile, make, output, accel, firmware):
    groups = [Group("host-" + name, [make, "--no-print-directory", "test-" + name],
                    3600 if name in ("mfqs", "storage") else 1800,
                    MEDIA if name in ("mfqs", "storage") else (),
                    skip="host suite not requested by guest profile" if profile == "guest" else "")
              for name in HOST]
    if profile == "host":
        groups.append(Group("guest", [], skip="guest qualification requires test-guest or test-all"))
    else:
        build = output / "build"
        groups.append(Group("guest-build", [make, "--no-print-directory", "-B", "-j4",
                            "BUILD_DIR=" + str(build), "RTC_UTC_TRUST=1", "binaries",
                            "fstest", "fsstress", "fsstress-host", "mkmgfs", "mgfsck"], 900))

        def guest(name, runner, arguments, collection=None, count=0, phases=(),
                  backends=(), comprehensive=False, expected="PASS"):
            receipt = dict(path=str(output / name / "report.json"), status=expected,
                           collection=collection, count=count, phases=list(phases),
                           backends=list(backends),
                           fields=["wrong_replacement"] if "--hotplug-negative-control" in arguments else [],
                           row_fields=(["wrong_content", "replacement_flush_failure"] if runner == "guest_safe" else []),
                           stress_controls=runner == "guest_stress")
            groups.append(Group(name, [sys.executable, str(ROOT / "tests" / "guest" / (runner + "_test.py")),
                                "--build-dir", str(build), "--output", str(output / name),
                                "--accel", accel, "--ovmf-code", str(firmware[0]),
                                "--ovmf-vars", str(firmware[1]), *arguments], 7200,
                                ("qemu-system-x86_64", "mmd", *MEDIA, *GUEST_MEDIA), receipt,
                                "comprehensive-only campaign; use test-all" if comprehensive and profile != "all" else ""))

        repeats = 2 if profile == "all" else 1
        backends = ("mgfs", "fat32", "exfat")
        guest("guest-root", "guest_mgfs", ["--repetitions", str(repeats), "--negative-control"],
              "runs", repeats, ("create", "verify", "negative"))
        guest("guest-secondary", "guest_secondary", ["--repetitions", str(repeats), "--timeout", "300"],
              "profiles", 3 * repeats, ("create", "verify", "controls"), backends)
        guest("guest-relative", "guest_mgfs", ["--relative-launch", "--negative-control"],
              "runs", 1, ("create", "verify", "negative"), comprehensive=True)
        guest("guest-hotplug-idle", "guest_secondary", ["--hotplug", "--backend", "mgfs",
              "--hotplug-negative-control", "--repetitions", "2", "--timeout", "300"],
              "profiles", 2, ("hotplug", "verify"), ("mgfs",), True)
        guest("guest-hotplug-references", "guest_secondary", ["--hotplug", "--hotplug-handles",
              "--backend", "mgfs", "--repetitions", "2", "--timeout", "300"],
              "profiles", 2, ("hotplug", "verify"), ("mgfs",), True)
        guest("guest-hotplug-cwd", "guest_secondary", ["--hotplug", "--hotplug-cwd",
              "--repetitions", "2", "--timeout", "300"],
              "profiles", 6, ("hotplug", "verify"), backends, True)
        guest("guest-safe", "guest_safe", ["--repetitions", "2", "--readonly", "--negative-control", "--faults"],
              "runs", 6, ("create", "verify", "readonly"), backends, True)
        for kind in ("read", "lookup", "write", "flush", "active"):
            guest("guest-lifetime-" + kind, "guest_lifetime", ["--kind", kind, "--timeout", "300"],
                  phases=("guest", "witnesses", "actions"), comprehensive=True)
            if kind != "active":
                guest("guest-lifetime-" + kind + "-control", "guest_lifetime",
                      ["--kind", kind, "--negative-control", "--timeout", "300"],
                      phases=("guest", "actions"), comprehensive=True, expected="EXPECTED_FAILURE")
        for phase, kind in (("stage-write", "write"), ("stage-flush", "flush"),
                            ("publication", "write"), ("cleanup", "write"), ("control", "write")):
            args = ["--mutation", "replace", "--kind", kind, "--timeout", "300"]
            if phase in ("publication", "cleanup"):
                args += ["--replacement-phase", phase]
            if phase == "control":
                args += ["--negative-control"]
            guest("guest-replace-" + phase, "guest_lifetime", args,
                  phases=("guest", "actions"), comprehensive=True,
                  expected="EXPECTED_FAILURE" if phase == "control" else "PASS")
        guest("guest-stress", "guest_stress", ["--host", str(build / "fsstress/fsstress-host"),
              "--controls", "--extra-controls"], "profiles", 6, ("create", "verify"), backends, True)
    for name, reason in (("network", "nettest is a manual native diagnostic; no automated acceptance runner"),
                         ("boot-stability", "specialist KVM boot probe; explicit image/hypervisor selection required"),
                         ("physical-device", "hardware-gated, separately authorized; never automatically executed"),
                         ("profiling", "optional instrumented host measurements; not routine correctness qualification")):
        groups.append(Group(name, [], skip=reason))
    return groups


def validate_receipt(spec):
    path = Path(spec["path"])
    if not path.is_file() or path.stat().st_size > LOG_LIMIT:
        raise ValueError("missing or oversized runner report: " + str(path))
    data = json.loads(path.read_text())
    if not isinstance(data, dict) or data.get("status") != spec["status"]:
        raise ValueError("runner report did not confirm " + spec["status"])
    if any(not isinstance(data.get(key), dict) or not data[key] for key in spec.get("fields", [])):
        raise ValueError("missing requested control evidence")
    rows = data.get(spec["collection"]) if spec["collection"] else [data]
    if not isinstance(rows, list) or len(rows) != (spec["count"] or 1):
        raise ValueError("missing or incorrect campaign count")
    if any(not isinstance(row, dict) for row in rows):
        raise ValueError("malformed campaign")
    if spec["backends"]:
        observed = [row.get("backend") for row in rows]
        expected = sorted(spec["backends"] * (len(rows) // len(spec["backends"])))
        if any(not isinstance(b, str) for b in observed) or sorted(observed) != expected:
            raise ValueError("incorrect backend campaign membership")
    for row in rows:
        if not isinstance(row, dict) or row.get("active_phase"):
            raise ValueError("incomplete campaign")
        phases = row.get("phases", row)
        if not isinstance(phases, dict) or any(not phases.get(p) for p in spec["phases"]):
            raise ValueError("missing completed campaign phase")
        if any(not isinstance(row.get(key), dict) or not row[key] for key in spec.get("row_fields", [])):
            raise ValueError("missing requested per-campaign control evidence")
        if "controls" in spec["phases"] and (not isinstance(phases["controls"], dict) or
                set(phases["controls"]) != {"readonly", "unauthorized", "wrong-label", "missing-service",
                                           "missing-device", "wrong-content"}):
            raise ValueError("incomplete secondary control set")
        default = None if spec["backends"] else "PASS"  # Root runner has no per-run status field.
        if spec["collection"] and row.get("status", default) != "PASS":
            raise ValueError("campaign did not pass")
    if spec.get("stress_controls"):
        controls = data.get("controls")
        if not isinstance(controls, list) or len(controls) != 6 or any(
                not isinstance(c, dict) or not isinstance(c.get("mode"), str) or
                not isinstance(c.get("kind"), str) for c in controls) or {
                (c["mode"], c["kind"]) for c in controls} != {
                ("model", "write"), ("status", "write"), ("prefix", "write"), ("incomplete", "write"),
                ("fault", "write"), ("fault", "flush")}:
            raise ValueError("missing stress oracle/fault controls")
        if sum(bool(row.get("wrong_content")) for row in rows) != len(spec["backends"]):
            raise ValueError("missing stress persisted-content controls")
    return data["status"]


def terminate(process):
    # Stop the whole owned session, including QEMU, before returning. No image
    # reading or repair occurs here. A killed runner's artifacts remain incomplete.
    handlers = {sig: signal.signal(sig, signal.SIG_IGN) for sig in (signal.SIGINT, signal.SIGTERM)}
    remaining = False
    try:
        for signum in (signal.SIGTERM, signal.SIGKILL):
            try:
                os.killpg(process.pid, signum)
                remaining = True
            except ProcessLookupError:
                break
            if signum == signal.SIGTERM:
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
        process.wait()
    finally:
        for sig, handler in handlers.items():
            signal.signal(sig, handler)
    return remaining


def execute(group, output, env):
    result = dict(name=group.name, status="INCOMPLETE", command=group.command,
                  deadline_seconds=group.seconds, reason="not completed",
                  executed=False, log=str(output / (group.name + ".log")))
    if group.skip:
        return {**result, "status": "SKIPPED", "reason": group.skip}
    missing = [tool for tool in group.tools if not shutil.which(tool, path=env["PATH"])]
    if missing:
        return {**result, "reason": "missing prerequisite(s): " + ", ".join(missing)}
    started = time.monotonic()
    process = None
    try:
        with Path(result["log"]).open("wb") as log:
            process = subprocess.Popen(group.command, cwd=ROOT, env=env, stdout=log,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            result["executed"] = True
            while process.poll() is None:
                if time.monotonic() - started >= group.seconds:
                    raise TimeoutError("group deadline exceeded")
                if log.tell() > LOG_LIMIT:
                    raise TimeoutError("group log exceeded 64 MiB bound")
                try:
                    process.wait(timeout=.25)
                except subprocess.TimeoutExpired:
                    pass
        result["returncode"] = process.returncode
        if Path(result["log"]).stat().st_size > LOG_LIMIT:
            raise TimeoutError("group log exceeded monitored 64 MiB limit")
        text = Path(result["log"]).read_text(errors="replace")
        resource = re.search(r"No space left on device|insufficient .*capacity|requires .*free artifact|"
                             r"TimeoutError|timed out|KeyboardInterrupt|Error 130|Error 143", text, re.I)
        omissions = re.search(r"^(?:SKIP\b|UNAVAILABLE\b|INCOMPLETE (?!broader qualification:)).*", text, re.M)
        if resource or omissions:
            result["reason"] = (resource or omissions)[0]
        elif process.returncode:
            result.update(status="INCOMPLETE" if process.returncode < 0 else "FAIL",
                          reason="command exited " + str(process.returncode))
        elif not text.strip():
            result["reason"] = "missing command completion output"
        else:
            if group.receipt:
                result["runner_status"] = validate_receipt(group.receipt)
                result["evidence"] = group.receipt["path"]
            result.update(status="PASS", reason="confirmed command completion" +
                          (" and existing runner evidence" if group.receipt else ""))
    except (OSError, ValueError, TimeoutError) as error:
        result["reason"] = str(error)
    except (KeyboardInterrupt, InterruptedError):
        result["reason"] = "orchestration interrupted"
        result["interrupted"] = True
    finally:
        if process is not None:
            if terminate(process) and result["status"] == "PASS":
                result.update(status="INCOMPLETE", reason="owned child session outlived command completion")
        result["seconds"] = round(time.monotonic() - started, 3)
    return result


def overall(results):
    if any(r["status"] == "FAIL" for r in results):
        return "FAIL"
    required = [r for r in results if not r.get("optional")]
    if not required or any(r["status"] != "PASS" for r in required):
        return "INCOMPLETE"
    return "PASS"


def save(output, report):
    temporary = output / "summary.tmp"
    temporary.write_text(json.dumps(report, indent=2) + "\n")
    temporary.replace(output / "summary.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", choices=("host", "guest", "all"))
    parser.add_argument("--make", default="make")
    parser.add_argument("--output", type=Path)
    artifacts.add_retention_argument(parser)
    parser.add_argument("--accel", choices=("tcg", "kvm"),
                        default=qemu_accel.default_accel())
    parser.add_argument("--ovmf-code", type=Path, default=Path("/usr/share/OVMF/OVMF_CODE_4M.fd"))
    parser.add_argument("--ovmf-vars", type=Path, default=Path("/usr/share/OVMF/OVMF_VARS_4M.fd"))
    args = parser.parse_args()
    if args.profile != "host":
        error = qemu_accel.accel_error(args.accel)
        if error:
            parser.error(error)
    if args.output is not None and any(c in str(args.output) for c in (",", "\n")):
        parser.error("output must be new and contain no comma or newline")
    try:
        workspace = artifacts.ArtifactWorkspace(
            args.output, name="mts-" + args.profile,
            retain=args.retain_artifacts)
    except FileExistsError:
        parser.error("output must be a new directory")
    output = workspace.path
    report = dict(suite="MTS", profile=args.profile, status="INCOMPLETE", output=str(output),
                  asan_options=os.environ.get("ASAN_OPTIONS", "detect_leaks=0"), groups=[],
                  orchestrator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    finished = False

    def finalize():
        nonlocal finished, report
        if finished:
            return
        report["status"] = report.get("status", "INCOMPLETE")
        report["reason"] = report.get("reason", "orchestration exited before completion")
        if workspace.temporary and not workspace.retain:
            report["output"] = "temporary output removed after completion"
        save(output, report)
        workspace.finish(report, verified=report.get("status") == "PASS")
        finished = True

    atexit.register(finalize)
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", TMPDIR=str(output / "tmp"))
    # Debian media tools often live outside an ordinary user's PATH.
    env["PATH"] = env.get("PATH", "") + os.pathsep + "/usr/sbin" + os.pathsep + "/sbin"
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    Path(env["TMPDIR"]).mkdir()
    groups = plan(args.profile, args.make, output, args.accel, (args.ovmf_code, args.ovmf_vars))
    report["revision"] = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    report["tracked_diff_sha256"] = hashlib.sha256(subprocess.check_output(
        ["git", "diff", "HEAD", "--"], cwd=ROOT)).hexdigest()
    report["working_tree"] = subprocess.check_output(
        ["git", "status", "--porcelain=v1"], cwd=ROOT, text=True).splitlines()
    required_gib = {"host": 8, "guest": 8, "all": 64}[args.profile]
    unavailable = ""
    try:
        workspace.require_free_space(required_gib * 1024**3,
                                     f"MTS {args.profile} profile")
    except artifacts.InsufficientArtifactSpace as error:
        unavailable = str(error)
    guest_unavailable = ""
    if args.profile != "host" and any(not p.is_file() for p in (args.ovmf_code, args.ovmf_vars)):
        guest_unavailable = "missing OVMF firmware; select --ovmf-code and --ovmf-vars"
    def interrupted(signum, frame):
        raise InterruptedError("signal " + str(signum))
    signal.signal(signal.SIGTERM, interrupted)
    print(f"MTS {args.profile}: serial execution; {required_gib} GiB free-space admission; evidence {output}", flush=True)
    for group in groups:
        pending = dict(name=group.name, status="INCOMPLETE", reason="not started",
                       executed=False, optional=bool(group.skip))
        report["groups"].append(pending)
    save(output, report)
    stopped = False
    build_failed = False
    for index, group in enumerate(groups):
        print(f"MTS {group.name}: {'SKIPPED' if group.skip else 'RUN'}", flush=True)
        blocked = (guest_unavailable if group.name.startswith("guest-") else "") or unavailable
        if not group.skip and (blocked or stopped or build_failed and group.receipt):
            result = dict(name=group.name, status="INCOMPLETE", reason=blocked or
                          ("not run after interruption" if stopped else "guest build did not pass"), executed=False)
        else:
            result = execute(group, output, env)
        result.setdefault("command", group.command)
        result.setdefault("deadline_seconds", group.seconds)
        result["optional"] = bool(group.skip)
        report["groups"][index] = result
        stopped |= result.get("interrupted", False)
        build_failed |= group.name == "guest-build" and result["status"] != "PASS"
        save(output, report)
        print(f"MTS {group.name}: {result['status']} — {result['reason']}", flush=True)
    report["status"] = overall(report["groups"])
    report["seconds"] = round(sum(r.get("seconds", 0) for r in report["groups"]), 3)
    save(output, report)
    finalize()
    atexit.unregister(finalize)
    final_report = dict(report)
    print(f"MTS {final_report['status']}: final summary follows", flush=True)
    print(json.dumps(final_report, indent=2, sort_keys=True), flush=True)
    return {"PASS": 0, "FAIL": 1, "INCOMPLETE": 2}[final_report["status"]]


if __name__ == "__main__":
    sys.exit(main())
