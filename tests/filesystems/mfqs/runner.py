#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded production-code MFQS foundation and exact independent comparison."""
import argparse
import hashlib
import os
from collections import Counter
import json
from pathlib import Path
import platform
import resource
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "filesystems/mfqs"))
sys.path.insert(0, str(ROOT / "shared"))
import artifact_lifecycle as artifacts
from model import Model, fixed_trace
from generator import generated_trace
from witnesses import evidence
from images import Unavailable, check, create, external_tools, initial_files

REPOSITORY = ROOT.parent
VERSION = 1
MAX_COMMANDS = 8192
MAX_OUTPUT = 32 * 1024 * 1024
LAYOUTS = {"mgfs": ("empty", "preallocated-directory"), "fat32": ("empty", "slack"),
           "exfat": ("empty", "slack", "chained-slack", "short-valid")}
FORMAT_ARGS = ["--blocks", "3200", "--uuid",
               "11111111-2222-3333-4444-555555555555", "--format-time-ns", "1"]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def build(directory, compiler, driver_source=None, backend="mgfs"):
    tools = external_tools(backend) if backend != "mgfs" else {}
    common = [compiler, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror"]
    # heap.h has an unused static declaration; use the established fixture flag.
    adapter = Path(directory) / "adapter"
    backend_flags = [] if backend == "mgfs" else ["-DMFQS_" + backend.upper(), "-Wno-type-limits"]
    sources = [str(driver_source or REPOSITORY / ("kernel/src/storage/" + backend + ".c")),
               str(REPOSITORY / "libc/src/utf8.c")]
    if backend == "exfat":
        sources.append(str(REPOSITORY / "libc/src/time_convert.c"))
    subprocess.run(common + backend_flags + ["-Wno-unused-function", "-fsanitize=address,undefined",
                   "-fno-sanitize-recover=all",
                   "-I" + str(REPOSITORY / "kernel/include"),
                   "-I" + str(REPOSITORY / "include"), "-idirafter",
                   str(REPOSITORY / "libc/include"),
                   "-I" + str(REPOSITORY / "kernel/src/storage"),
                   str(ROOT / "filesystems/mfqs/adapter.c"), str(ROOT / "filesystems/mfqs/host.c"),
                   str(REPOSITORY / "kernel/src/vfs.c"),
                   *sources, "-o", str(adapter)],
                   check=True, timeout=120)
    for name in (("mkmgfs", "mgfsck") if backend == "mgfs" else ()):
        tools[name] = Path(directory) / name
        # Match the checker target's existing warning selection.
        flags = ["-Wno-parentheses", "-Wno-unused-parameter"] if name == "mgfsck" else []
        sources = [str(REPOSITORY / "tools" / (name + ".c"))]
        includes = []
        if name == "mkmgfs":
            includes = ["-I" + str(REPOSITORY / "include")]
            sources.append(str(REPOSITORY / "libc/src/utf8.c"))
        subprocess.run(common + flags + includes + sources +
                       ["-o", str(tools[name])], check=True, timeout=120)
    return adapter, tools


def encode(command, sequence):
    op = command["op"]
    if op in ("CREATE", "CREATE_ORDINARY", "OPEN", "STAT", "MKDIR",
              "RMDIR", "UNLINK", "RENAME", "LIST", "CREATE_CONTENTS"):
        a = command["path"].encode("ascii").hex() if "path" in command else "-"
    elif op in ("READ", "TRUNCATE", "RESIZE"):
        a = str(command["size"])
    elif op == "SEEK":
        a = str(command["offset"])
    else:
        a = "-"
    b = ((command["data"] or "-") if op in ("WRITE", "REPLACE", "CREATE_CONTENTS") else
         command["destination"].encode("ascii").hex() if op == "RENAME" else "-")
    return f"MFQS1\t{sequence}\t{op}\t{a}\t{b}\n"


def validate_trace(trace, backend="mgfs", layout="empty"):
    if not isinstance(trace, list) or not 1 <= len(trace) <= MAX_COMMANDS:
        raise ValueError("trace must contain 1..8192 commands")
    if trace[0] != {"op": "HELLO"}:
        raise ValueError("first command must negotiate MFQS1")
    fields = {"HELLO": set(), "LIST": set(), "CLOSE": set(), "REMOUNT": set(),
              "SNAPSHOT": set(), "CHECK": set(), "REPLACE": {"data"},
              "CREATE_CONTENTS": {"path", "data"},
              "CREATE": {"path"}, "OPEN": {"path"}, "STAT": {"path"},
              "CREATE_ORDINARY": {"path"}, "MKDIR": {"path"}, "RMDIR": {"path"},
              "UNLINK": {"path"}, "RENAME": {"path", "destination"},
              "TRUNCATE": {"size"}, "RESIZE": {"size"},
              "READ": {"size"}, "SEEK": {"offset"}, "WRITE": {"data"}}
    for command in trace:
        if not isinstance(command, dict) or command.get("op") not in fields:
            raise ValueError("unsupported foundation operation")
        if (set(command) != {"op"} | fields[command["op"]] and
            not (command["op"] == "LIST" and set(command) == {"op", "path"})):
            raise ValueError("unexpected command fields")
        if command["op"] == "TRUNCATE" and command["size"] != 0:
            raise ValueError("native truncate is empty-only")
        for field in ("path", "destination"):
            if field not in command:
                continue
            path = command[field]
            if (not isinstance(path, str) or not path.startswith("/") or
                not 1 <= len(path) <= 256 or
                (path != "/" and any(not 1 <= len(part) <= 255 or part in (".", "..") for part in path[1:].split("/"))) or
                any(c not in "/abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-" for c in path) or
                any(part.endswith(".") for part in path[1:].split("/"))):
                raise ValueError("profile requires bounded absolute ASCII components")
            if path == "/" and command["op"] not in ("STAT", "LIST"):
                raise ValueError("root mutation is outside the profile")
        for name, limit in (("size", 65536 if command["op"] in ("TRUNCATE", "RESIZE") else 8192), ("offset", 65536)):
            if name in command and (type(command[name]) is not int or
                                    not 0 <= command[name] <= limit):
                raise ValueError("command bound exceeded: " + name)
        if "data" in command:
            data = command["data"]
            if (not isinstance(data, str) or len(data) > 16384 or len(data) % 2 or
                any(c not in "0123456789abcdef" for c in data)):
                raise ValueError("WRITE must contain bounded lowercase hexadecimal")
    model = Model(backend, initial_files(layout))
    expected = []
    projected_output = 0
    for command in trace:
        try:
            expected.append(model.apply(command))
        except (AssertionError, KeyError) as error:
            raise ValueError("operation outside the oracle domain: " + str(command)) from error
        projected_output += len(expected[-1]["data"]) + 128  # Bounded scalar envelope.
        if projected_output > MAX_OUTPUT:
            raise ValueError("expected response volume exceeds the qualification budget")
        if len(model.files) + len(model.directories) - 1 > 31:
            raise ValueError("namespace exceeds the bounded enumeration profile")
    if model.opened is not None:
        raise ValueError("trace must close its handle")
    return expected


def response(command, line, sequence, backend):
    fields = line.split("\t")
    if len(fields) != 8 or fields[0] != "MFQS1" or fields[1] != str(sequence):
        raise ValueError("invalid response envelope")
    got = dict(zip(("status", "size", "offset", "count", "pins"), map(int, fields[2:7])))
    got["data"] = "" if fields[7] == "-" else fields[7]
    data = bytes.fromhex(got["data"])
    if command["op"] == "LIST":
        if data and not data.endswith(b"\0"):
            raise ValueError("unterminated namespace observation")
        names = data.split(b"\0")[:-1]
        if backend != "mgfs": names = [name.lower() for name in names]  # ASCII only.
        got["data"] = b"".join(name + b"\0" for name in sorted(names)).hex()
    return got


def compare(trace, expected, output, backend="mgfs"):
    lines = output.splitlines()
    for sequence, (command, want, line) in enumerate(zip(trace, expected, lines), 1):
        try:
            got = response(command, line, sequence, backend)
        except (ValueError, TypeError) as error:
            return dict(kind="protocol", sequence=sequence, error=str(error), response=line)
        # Directory size has no portable expected value. No other field is masked.
        keys = set(want)
        if command["op"] == "STAT" and want["count"] == 2 and want["size"] is None:
            keys.remove("size")
        if any(got[key] != want[key] for key in keys):
            return dict(kind="model-mismatch", sequence=sequence, command=command,
                        expected=want, observed=got)
    if len(lines) != len(trace):
        return dict(kind="protocol", sequence=min(len(lines) + 1, len(trace)),
                    expected_responses=len(trace), observed_responses=len(lines))
    return None


def corrupt_observation(trace, output, backend, layout, control):
    """Comparator-only negative controls. Never alter driver I/O or media."""
    lines = output.splitlines()
    model = Model(backend, initial_files(layout))
    overwritten = False
    for index, (command, line) in enumerate(zip(trace, lines)):
        op = command["op"]
        if op == "WRITE" and command["data"]:
            overwritten |= model.offset < len(model.files[model.opened])
        model.apply(command)
        fields = line.split("\t")
        if control in ("status", "pins") and op == "WRITE" and int(fields[5]):
            fields[2 if control == "status" else 6] = "-3" if control == "status" else "1"
        elif control in ("offset", "count", "middle", "tail") and op == "READ" and int(fields[5]):
            if control in ("middle", "tail"):
                if not overwritten or int(fields[5]) < 2: continue
                wrong = bytearray.fromhex(fields[7])
                wrong[len(wrong) // 2 if control == "middle" else len(wrong) - 1] ^= 1
                fields[7] = wrong.hex()
            else:
                at = 4 if control == "offset" else 5
                fields[at] = str(int(fields[at]) + 1)
        else: continue
        lines[index] = "\t".join(fields)
        return "\n".join(lines) + "\n"
    raise ValueError("observation control lacks a suitable operation")


def qualify(directory, adapter, tools, compiler, trace, negative=None, driver_source=None,
            expected_initial=None, backend="mgfs", layout="empty", campaign=None, seed=None):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    expected = validate_trace(trace, backend, layout)
    if negative in ("byte", "length", "namespace"):
        # Historical controls corrupt an expectation; production remains intact.
        for command, want in zip(trace, expected):
            if negative == "namespace" and command["op"] == "LIST" and want["count"]:
                want["data"] = (bytes.fromhex(want["data"]) + b"oracle-only\0").hex()
                want["count"] += 1
                break
            if negative != "namespace" and command["op"] == "READ" and want["count"]:
                if negative == "byte":
                    wrong = bytearray.fromhex(want["data"]); wrong[0] ^= 1
                    want["data"] = wrong.hex()
                else: want["size"] += 1
                break
        else:
            raise ValueError("negative control lacks its required observation")
    image = directory / "media.img"
    initial = directory / "initial.img"
    if image.exists() or initial.exists():
        raise ValueError("qualification will not overwrite existing images")
    provenance = create(initial, backend, layout, tools, FORMAT_ARGS)
    if expected_initial and digest(initial) != expected_initial:
        raise ValueError("replay initial-image hash differs; image provenance is not equivalent")
    shutil.copyfile(initial, image)
    initial_check = check(initial, backend, tools)
    if initial_check["returncode"]:
        (directory / "initial-check.json").write_text(json.dumps(initial_check, indent=2))
        raise ValueError("independent initial-image validation failed")
    source = REPOSITORY / ("kernel/src/storage/" + backend + ".c")
    environment = dict(platform=platform.platform(), machine=platform.machine(),
                       python=sys.version, compiler=compiler,
                       compiler_version=subprocess.check_output([compiler, "--version"], text=True),
                       head=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPOSITORY, text=True).strip(),
                       production_sha256={"kernel/src/vfs.c": digest(REPOSITORY / "kernel/src/vfs.c"),
                           "libc/src/utf8.c": digest(REPOSITORY / "libc/src/utf8.c"),
                           str(source.relative_to(REPOSITORY)): digest(driver_source or source)},
                       harness_sha256={"tests/filesystems/mfqs/" + name: digest(ROOT / "filesystems/mfqs" / name)
                           for name in ("adapter.c", "host.c", "host.h", "model.py", "runner.py", "images.py", "mgfs_images.py", "generator.py", "witnesses.py")},
                       adapter_sha256=digest(adapter), sanitizers="address,undefined",
                       sanitizer_options={name: os.environ.get(name, "default") for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS")})
    if backend == "mgfs":
        driver_text = Path(driver_source or source).read_text()
        for include in ("mgfs_ownership.inc", "mgfs_ordinary.inc"):
            if '"' + include + '"' in driver_text:
                path = source.parent / include
                environment["production_sha256"][str(path.relative_to(REPOSITORY))] = digest(path)
    if backend == "exfat":
        environment["production_sha256"]["libc/src/time_convert.c"] = digest(REPOSITORY / "libc/src/time_convert.c")
        environment["simulated_realtime"] = "trusted UTC 2000-01-01 00:00:00, whole-second resolution"
    report = dict(version=VERSION, profile=backend + ("-generated-" + campaign if campaign else "-fixed-foundation"),
                  campaign=campaign, seed=seed, backend=backend,
                  layout=layout, status="INCOMPLETE", initial_check=initial_check,
                  coverage_scope="generated single-threaded fault-free host profile" if campaign else "fixed single-threaded fault-free host profile",
                  trace=trace, expected=expected, negative_control=negative,
                  initial_image=dict(**provenance,
                                     formatter_sha256=digest(tools["mkmgfs"] if backend == "mgfs" else tools["formatter"]),
                                     sha256=digest(initial), bytes=initial.stat().st_size),
                  environment=environment, unqualified=["random-sequences",
                      "fault-injection", "concurrency", "crash-recovery", "hardware", "authorization",
                      "broader-imported-layouts", "unicode-names", "rename", "unlink", "mkdir", "rmdir", "resize", "safe-replace"])
    wire = "".join(encode(c, i) for i, c in enumerate(trace, 1))
    (directory / "commands.tsv").write_text(wire)
    try:
        start = time.perf_counter()
        cpu = resource.getrusage(resource.RUSAGE_CHILDREN)
        adapter_env = os.environ.copy()
        adapter_env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
        with (directory / "observations.tsv").open("w") as stdout, (directory / "adapter.stderr").open("w") as stderr:
            result = subprocess.run([str(adapter), str(image)], input=wire, text=True,
                                    stdout=stdout, stderr=stderr, timeout=120,
                                    env=adapter_env)
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        report["timing"] = dict(adapter_wall_seconds=time.perf_counter() - start,
            adapter_cpu_seconds=after.ru_utime + after.ru_stime - cpu.ru_utime - cpu.ru_stime)
        output = (directory / "observations.tsv")
        comparison_output = output.read_text() if output.stat().st_size <= MAX_OUTPUT else ""
        if comparison_output and not result.returncode and negative in ("offset", "count", "status", "pins", "middle", "tail"):
            comparison_output = corrupt_observation(trace, comparison_output, backend, layout, negative)
            (directory / "negative-observations.tsv").write_text(comparison_output)
        if output.stat().st_size > MAX_OUTPUT:
            report["failure"] = dict(kind="protocol", error="output exceeds bound")
        elif result.returncode:
            report["failure"] = compare(trace, expected, comparison_output, backend) or dict(kind="adapter", returncode=result.returncode)
            report["adapter_returncode"] = result.returncode
        else:
            report["failure"] = compare(trace, expected, comparison_output, backend)
        metrics = [line[len("MFQS_METRICS "):] for line in
                   (directory / "adapter.stderr").read_text().splitlines()
                   if line.startswith("MFQS_METRICS ")]
        if not report["failure"] and len(metrics) != 1:
            report["failure"] = dict(kind="protocol", error="missing teardown metrics")
        if len(metrics) == 1:
            report["metrics"] = json.loads(metrics[0])
        report["execution_status"] = "FAIL" if report["failure"] else "PASS"
        report["status"] = report["execution_status"]
        if not report["failure"]:
            observed = [response(command, line, i, backend) for i, (command, line) in
                        enumerate(zip(trace, output.read_text().splitlines()), 1)]
            report["coverage"] = evidence(trace, observed, backend, layout, campaign)
            if report["coverage"]["missing"]: report["status"] = "INCOMPLETE"
        report["image_check"] = check(image, backend, tools)
        if backend == "mgfs":
            report["mgfsck"] = report["image_check"]  # Existing report consumers.
        if report["image_check"]["returncode"]:
            report["status"] = "FAIL"
            if not report["failure"]:
                report["failure"] = dict(kind="independent-image-check")
    except subprocess.TimeoutExpired as error:
        report.update(status="FAIL", failure=dict(kind="timeout", seconds=error.timeout,
                                                 command=error.cmd))
    report["trace_bytes"] = len(wire.encode("ascii"))
    report["bounds"] = dict(commands=MAX_COMMANDS, transfer_bytes=8192, file_bytes=65536,
        path_bytes=256, namespace_objects=31, image_bytes=64 * 1024 * 1024,
        kernel_heap_bytes=8 * 1024 * 1024, heap_slots=8192, retained_nodes=4096,
        output_bytes=MAX_OUTPUT, adapter_seconds=120)
    report["operation_counts"] = dict(Counter(command["op"] for command in trace))
    report["expected_status_counts"] = dict(Counter(str(item["status"]) for item in expected))
    report["capabilities"] = dict(resize="arbitrary" if backend == "fat32" else "safely-unsupported-nonzero",
        gaps="zero-filled" if backend != "mgfs" else "sparse-writes-not-generated",
        names="ASCII domain only; explicit backend matching",
        unlink_open_handles="not-qualified; handles closed before retirement")
    witnessed = set(report.get("coverage", {}).get("witnesses", []))
    report["qualified_behaviors"] = sorted(witnessed) if report["status"] == "PASS" else []
    for operation in ("rename", "unlink", "mkdir", "rmdir", "safe-replace"):
        if operation in witnessed: report["unqualified"].remove(operation)
    if "resize-grow" in witnessed and "shrink" in witnessed:
        report["unqualified"].remove("resize")
    report["unqualified"] += ["directory-size-accounting", "directory-moves",
        "open-handle-namespace-mutations", "allocation-exhaustion", "large-files"]
    report["unqualified"].remove("random-sequences")
    report["unqualified"] += ["distribution-completeness", "directory-entry-types"]
    if (report.get("failure") or {}).get("sequence"):
        prefix = trace[:report["failure"]["sequence"]]
        (directory / "failing-prefix.json").write_text(json.dumps(prefix, indent=2) + "\n")
        (directory / "failing-prefix.tsv").write_text("".join(encode(c, i) for i, c in enumerate(prefix, 1)))
        report["failure"]["prefix_commands"] = len(prefix)
    report["final_image_sha256"] = digest(image)
    (directory / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--backend", choices=("mgfs", "fat32", "exfat"), default="mgfs")
    parser.add_argument("--layout", choices=("empty", "slack", "chained-slack", "short-valid", "preallocated-directory"), default="empty")
    parser.add_argument("--replay", type=Path, help="replay the trace in a saved report.json")
    parser.add_argument("--artifacts", type=Path, help="new empty directory; retains images and complete report")
    parser.add_argument("--negative-control", choices=("byte", "length", "namespace", "offset", "count", "status", "pins", "middle", "tail"))
    parser.add_argument("--campaign", choices=("quick", "extended"))
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0)
    args = parser.parse_args(argv)
    try:
        workspace = artifacts.ArtifactWorkspace(args.artifacts, name="mfqs-run")
    except FileExistsError:
        parser.error("--artifacts must name a new directory (existing data is preserved)")
    directory = workspace.path
    try:
        workspace.require_free_space(256 * 1024 * 1024,
                                     "MFQS single-run artifacts")
        backend, layout = args.backend, args.layout
        expected_initial = None
        campaign, seed = args.campaign, args.seed
        if args.replay:
            if args.replay.stat().st_size > MAX_OUTPUT:
                raise ValueError("replay report exceeds bound")
            saved = json.loads(args.replay.read_text())
            backend = saved.get("backend", "mgfs")
            layout = saved.get("layout", "empty")
            if (type(saved["version"]) is not int or saved["version"] != VERSION or
                saved["profile"] not in (backend + "-fixed-foundation", backend + "-generated-quick", backend + "-generated-extended")):
                raise ValueError("unsupported replay version/profile")
            expected_initial = saved["initial_image"]["sha256"]
            campaign, seed = saved.get("campaign"), saved.get("seed")
            if campaign:
                if (campaign not in ("quick", "extended") or
                    saved["profile"] != backend + "-generated-" + campaign or
                    type(seed) is not int or not 0 <= seed < (1 << 64)):
                    raise ValueError("inconsistent generated replay provenance")
        if backend not in LAYOUTS or layout not in LAYOUTS[backend]:
            raise ValueError("unsupported backend/fixture layout")
        trace = (saved["trace"] if args.replay else generated_trace(backend, layout, seed, campaign)
                 if campaign else fixed_trace(backend, initial_files(layout)))
        validate_trace(trace, backend, layout)
        adapter, tools = build(directory, args.cc, backend=backend)
        report = qualify(directory, adapter, tools, args.cc, trace, args.negative_control,
                         expected_initial=expected_initial, backend=backend, layout=layout, campaign=campaign, seed=seed)
    except (Unavailable, OSError, ValueError, TypeError, KeyError, AssertionError, subprocess.SubprocessError) as error:
        (directory / "incomplete.json").write_text(json.dumps(dict(
            version=VERSION, status="INCOMPLETE", error=str(error),
            environment=dict(platform=platform.platform(), compiler=args.cc)), indent=2) + "\n")
        outcome = dict(version=VERSION, status="INCOMPLETE", error=str(error),
                       environment=dict(platform=platform.platform(), compiler=args.cc))
        report = workspace.finish(outcome, verified=False)
        location = (report.get("failure_evidence") or report.get("retained_output") or
                    str(directory))
        print(f"INCOMPLETE: harness/environment failure: {error}; evidence: {location}")
        return 2
    except KeyboardInterrupt:
        outcome = dict(version=VERSION, status="INCOMPLETE", error="interrupted",
                       environment=dict(platform=platform.platform(), compiler=args.cc))
        (directory / "incomplete.json").write_text(json.dumps(outcome, indent=2) + "\n")
        result = workspace.finish(outcome, verified=False)
        location = result.get("failure_evidence") or result.get("retained_output") or str(directory)
        print(f"INCOMPLETE: interrupted; evidence: {location}")
        return 130
    except BaseException as error:
        # Keep unexpected programming failures visible while still publishing
        # bounded diagnostics and removing the owned scratch workspace.
        outcome = dict(version=VERSION, status="FAIL",
                       error=f"{type(error).__name__}: {error}",
                       environment=dict(platform=platform.platform(), compiler=args.cc))
        image = directory / "media.img"
        if image.is_file():
            workspace.set_failure_media(image)
        try:
            workspace.finish(outcome, verified=False)
        finally:
            print(f"MFQS runner failed; owned evidence: " +
                  str(outcome.get("failure_evidence", directory)), file=sys.stderr)
        raise

    status = report.get("status", "INCOMPLETE")
    image = directory / "media.img"
    if status == "FAIL" and image.is_file():
        workspace.set_failure_media(image)
    result = workspace.finish(report, verified=status in {"PASS", "EXPECTED_FAILURE"})
    print(f"{status}: {report['profile']}/{layout} ({len(trace)} commands); "
          "broader qualification INCOMPLETE")
    if status != "PASS" or args.artifacts:
        print(f"Artifacts: {result.get('retained_output') or result.get('failure_evidence') or directory}")
        if report.get("failure"):
            print(json.dumps(report["failure"], sort_keys=True))
        if report.get("coverage", {}).get("missing"):
            print("Missing qualification witnesses: " + ", ".join(report["coverage"]["missing"]))
    return 0 if report["status"] == "PASS" else 2 if report["status"] == "INCOMPLETE" else 1


if __name__ == "__main__":
    sys.exit(main())
