#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded production-code MFQS foundation and exact independent comparison."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import resource
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from model import Model, fixed_trace

REPOSITORY = ROOT.parent
VERSION = 1
MAX_COMMANDS = 256
MAX_OUTPUT = 5 * 1024 * 1024
FORMAT_ARGS = ["--blocks", "3200", "--uuid",
               "11111111-2222-3333-4444-555555555555", "--format-time-ns", "1"]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def build(directory, compiler, mgfs_source=None):
    common = [compiler, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror"]
    # heap.h has an unused static declaration; use the established fixture flag.
    adapter = Path(directory) / "adapter"
    subprocess.run(common + ["-Wno-unused-function", "-fsanitize=address,undefined",
                   "-I" + str(REPOSITORY / "kernel/include"),
                   "-I" + str(REPOSITORY / "include"), "-idirafter",
                   str(REPOSITORY / "libc/include"),
                   str(ROOT / "mfqs/adapter.c"), str(ROOT / "mfqs/host.c"),
                   str(REPOSITORY / "kernel/src/vfs.c"),
                   str(mgfs_source or REPOSITORY / "kernel/src/storage/mgfs.c"), "-o", str(adapter)],
                   check=True, timeout=120)
    tools = {}
    for name in ("mkmgfs", "mgfsck"):
        tools[name] = Path(directory) / name
        # Match the checker target's existing warning selection.
        flags = ["-Wno-parentheses", "-Wno-unused-parameter"] if name == "mgfsck" else []
        subprocess.run(common + flags + [str(REPOSITORY / "tools" / (name + ".c")),
                                "-o", str(tools[name])], check=True, timeout=120)
    return adapter, tools


def encode(command, sequence):
    op = command["op"]
    if op in ("CREATE", "OPEN", "STAT"):
        a = command["path"].encode("ascii").hex()
    elif op == "READ":
        a = str(command["size"])
    elif op == "SEEK":
        a = str(command["offset"])
    else:
        a = "-"
    b = (command["data"] or "-") if op == "WRITE" else "-"
    return f"MFQS1\t{sequence}\t{op}\t{a}\t{b}\n"


def validate_trace(trace):
    if not isinstance(trace, list) or not 1 <= len(trace) <= MAX_COMMANDS:
        raise ValueError("trace must contain 1..256 commands")
    if trace[0] != {"op": "HELLO"}:
        raise ValueError("first command must negotiate MFQS1")
    fields = {"HELLO": set(), "LIST": set(), "CLOSE": set(), "REMOUNT": set(),
              "CREATE": {"path"}, "OPEN": {"path"}, "STAT": {"path"},
              "READ": {"size"}, "SEEK": {"offset"}, "WRITE": {"data"}}
    for command in trace:
        if not isinstance(command, dict) or command.get("op") not in fields:
            raise ValueError("unsupported foundation operation")
        if set(command) != {"op"} | fields[command["op"]]:
            raise ValueError("unexpected command fields")
        if "path" in command:
            path = command["path"]
            if (not isinstance(path, str) or not path.startswith("/") or
                not 1 <= len(path[1:]) <= 255 or "/" in path[1:] or
                any(ord(c) < 32 or ord(c) > 126 for c in path) or
                path[1:] in (".", "..")):
                raise ValueError("profile requires a root-level printable ASCII name")
        for name, limit in (("size", 8192), ("offset", 65536)):
            if name in command and (type(command[name]) is not int or
                                    not 0 <= command[name] <= limit):
                raise ValueError("command bound exceeded: " + name)
        if "data" in command:
            data = command["data"]
            if (not isinstance(data, str) or len(data) > 16384 or len(data) % 2 or
                any(c not in "0123456789abcdef" for c in data)):
                raise ValueError("WRITE must contain bounded lowercase hexadecimal")
    model = Model()
    expected = []
    for command in trace:
        expected.append(model.apply(command))
        if len(model.files) > 31:
            raise ValueError("namespace exceeds the bounded enumeration profile")
    if model.opened is not None:
        raise ValueError("trace must close its handle")
    return expected


def compare(trace, expected, output):
    lines = output.splitlines()
    if len(lines) != len(trace):
        return dict(kind="protocol", expected_responses=len(trace), observed_responses=len(lines))
    for sequence, (command, want, line) in enumerate(zip(trace, expected, lines), 1):
        try:
            fields = line.split("\t")
            if len(fields) != 8 or fields[0] != "MFQS1" or fields[1] != str(sequence):
                raise ValueError("invalid response envelope")
            got = dict(zip(("status", "size", "offset", "count", "pins"),
                           map(int, fields[2:7])))
            got["data"] = "" if fields[7] == "-" else fields[7]
            data = bytes.fromhex(got["data"])
            if command["op"] == "LIST":
                # Enumeration order is not an oracle rule. Preserve duplicate
                # names/counts when comparing membership; never convert to set.
                if data and not data.endswith(b"\0"):
                    raise ValueError("unterminated namespace observation")
                got["data"] = b"".join(n + b"\0" for n in sorted(data.split(b"\0")[:-1])).hex()
        except (ValueError, TypeError) as error:
            return dict(kind="protocol", sequence=sequence, error=str(error), response=line)
        if got != want:
            return dict(kind="model-mismatch", sequence=sequence, command=command,
                        expected=want, observed=got)
    return None


def qualify(directory, adapter, tools, compiler, trace, negative=None, mgfs_source=None,
            expected_initial=None):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    expected = validate_trace(trace)
    if negative:
        # Deliberately corrupt an oracle expectation, never adapter or image.
        for command, want in zip(trace, expected):
            if command["op"] == "READ" and want["count"]:
                if negative == "byte":
                    wrong = bytearray.fromhex(want["data"])
                    wrong[0] ^= 1
                    want["data"] = wrong.hex()
                else:
                    want["size"] += 1
                break
        else:
            raise ValueError("negative control requires a nonempty READ")
    image = directory / "media.img"
    initial = directory / "initial.img"
    if image.exists() or initial.exists():
        raise ValueError("qualification will not overwrite existing images")
    subprocess.run([str(tools["mkmgfs"]), *FORMAT_ARGS, str(initial)],
                   check=True, capture_output=True, timeout=30)
    if expected_initial and digest(initial) != expected_initial:
        raise ValueError("replay initial-image hash differs; image provenance is not equivalent")
    shutil.copyfile(initial, image)
    environment = dict(platform=platform.platform(), machine=platform.machine(),
                       python=sys.version, compiler=compiler,
                       compiler_version=subprocess.check_output([compiler, "--version"], text=True),
                       head=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPOSITORY, text=True).strip(),
                       production_sha256={"kernel/src/vfs.c": digest(REPOSITORY / "kernel/src/vfs.c"),
                           "kernel/src/storage/mgfs.c": digest(mgfs_source or REPOSITORY / "kernel/src/storage/mgfs.c")},
                       harness_sha256={"tests/mfqs/" + name: digest(ROOT / "mfqs" / name)
                           for name in ("adapter.c", "host.c", "host.h", "model.py", "runner.py")},
                       adapter_sha256=digest(adapter), sanitizers="address,undefined")
    report = dict(version=VERSION, profile="mgfs-fixed-foundation", status="INCOMPLETE",
                  trace=trace, expected=expected, negative_control=negative,
                  initial_image=dict(formatter="tools/mkmgfs.c", arguments=FORMAT_ARGS,
                                     formatter_sha256=digest(tools["mkmgfs"]),
                                     sha256=digest(initial), bytes=initial.stat().st_size),
                  environment=environment, unqualified=["fat32", "exfat", "random-sequences",
                      "fault-injection", "concurrency", "crash-recovery", "hardware", "authorization",
                      "imported-layouts", "rename", "unlink", "mkdir", "rmdir", "resize", "safe-replace"])
    wire = "".join(encode(c, i) for i, c in enumerate(trace, 1))
    (directory / "commands.tsv").write_text(wire)
    try:
        start = time.perf_counter()
        cpu = resource.getrusage(resource.RUSAGE_CHILDREN)
        with (directory / "observations.tsv").open("w") as stdout, (directory / "adapter.stderr").open("w") as stderr:
            result = subprocess.run([str(adapter), str(image)], input=wire, text=True,
                                    stdout=stdout, stderr=stderr, timeout=60)
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        report["timing"] = dict(adapter_wall_seconds=time.perf_counter() - start,
            adapter_cpu_seconds=after.ru_utime + after.ru_stime - cpu.ru_utime - cpu.ru_stime)
        output = (directory / "observations.tsv")
        if output.stat().st_size > MAX_OUTPUT:
            report["failure"] = dict(kind="protocol", error="output exceeds bound")
        elif result.returncode:
            report["failure"] = dict(kind="adapter", returncode=result.returncode)
        else:
            report["failure"] = compare(trace, expected, output.read_text())
        metrics = [line[len("MFQS_METRICS "):] for line in
                   (directory / "adapter.stderr").read_text().splitlines()
                   if line.startswith("MFQS_METRICS ")]
        if not report["failure"]:
            if len(metrics) != 1:
                report["failure"] = dict(kind="protocol", error="missing teardown metrics")
            else:
                report["metrics"] = json.loads(metrics[0])
        report["status"] = "FAIL" if report["failure"] else "PASS"
        checker = subprocess.run([str(tools["mgfsck"]), str(image)],
                                 text=True, capture_output=True, timeout=30)
        report["mgfsck"] = dict(returncode=checker.returncode, stdout=checker.stdout, stderr=checker.stderr)
        if checker.returncode:
            report["status"] = "FAIL"
            if not report["failure"]:
                report["failure"] = dict(kind="independent-image-check")
    except subprocess.TimeoutExpired as error:
        report.update(status="FAIL", failure=dict(kind="timeout", seconds=error.timeout,
                                                 command=error.cmd))
    report["final_image_sha256"] = digest(image)
    (directory / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--backend", choices=("mgfs", "fat32", "exfat"), default="mgfs")
    parser.add_argument("--replay", type=Path, help="replay the trace in a saved report.json")
    parser.add_argument("--artifacts", type=Path, help="new empty directory; retains images and complete report")
    parser.add_argument("--negative-control", choices=("byte", "length"))
    args = parser.parse_args()
    if args.backend != "mgfs":
        print(f"INCOMPLETE: {args.backend} production adapter is not implemented")
        return 2
    directory = args.artifacts or Path(tempfile.mkdtemp(prefix="mangrove-mfqs-"))
    if args.artifacts:
        if directory.exists():
            parser.error("--artifacts must name a new directory (existing data is preserved)")
        directory.mkdir(parents=True)
    try:
        trace = fixed_trace()
        expected_initial = None
        if args.replay:
            if args.replay.stat().st_size > MAX_OUTPUT:
                raise ValueError("replay report exceeds bound")
            saved = json.loads(args.replay.read_text())
            if (type(saved["version"]) is not int or saved["version"] != VERSION or
                saved["profile"] != "mgfs-fixed-foundation"):
                raise ValueError("unsupported replay version/profile")
            trace = saved["trace"]
            expected_initial = saved["initial_image"]["sha256"]
        validate_trace(trace)
        adapter, tools = build(directory, args.cc)
        report = qualify(directory, adapter, tools, args.cc, trace, args.negative_control,
                         expected_initial=expected_initial)
    except (OSError, ValueError, TypeError, KeyError, AssertionError, subprocess.SubprocessError) as error:
        (directory / "incomplete.json").write_text(json.dumps(dict(
            version=VERSION, status="INCOMPLETE", error=str(error),
            environment=dict(platform=platform.platform(), compiler=args.cc)), indent=2) + "\n")
        print(f"INCOMPLETE: harness/environment failure: {error}; artifacts: {directory}")
        return 2
    print(f"{report['status']}: mgfs-fixed-foundation ({len(trace)} commands); "
          "FAT32/exFAT and broader qualification INCOMPLETE")
    if report["status"] != "PASS" or args.artifacts:
        print(f"Artifacts: {directory}")
        if report.get("failure"):
            print(json.dumps(report["failure"], sort_keys=True))
    else:
        shutil.rmtree(directory)
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
