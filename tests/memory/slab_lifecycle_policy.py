#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare immediate slab retirement with one retained empty slab per class."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def percentile(values, percent):
    ordered = sorted(values)
    rank = max(0, (len(ordered) * percent + 99) // 100 - 1)
    return ordered[rank]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("compiler", nargs="?", default="gcc")
parser.add_argument("--reserve", action="store_true",
                    help="compile the bounded empty-slab reserve candidate")
parser.add_argument("--trials", type=int, default=5)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
if not 1 <= args.trials <= 20:
    parser.error("--trials must be between 1 and 20")

output = args.output if args.output.is_absolute() else ROOT / args.output
if output.exists():
    parser.error(f"refusing to overwrite existing evidence: {output}")
output.parent.mkdir(parents=True, exist_ok=True)
temp_root = ROOT / "build/stage4.3-tmp"
temp_root.mkdir(parents=True, exist_ok=True)
flags = ["-std=gnu11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
         "-pthread", "-Iinclude", "-Ikernel/include", "-idirafter",
         "libc/include"]
if args.reserve:
    flags.append("-DSLAB_EMPTY_RESERVE_PER_CLASS=1")
version = subprocess.run([args.compiler, "--version"], cwd=ROOT, text=True,
                         capture_output=True, check=True).stdout.splitlines()[0]
all_samples = {16: [], 72: [], 384: []}
counter_totals = {request: {"backing_allocations": 0, "backing_releases": 0,
                           "empty_slab_reuses": 0, "retirements": 0}
                  for request in all_samples}
raw_trials = []
classifier_stats = []

with tempfile.TemporaryDirectory(prefix="slab-lifecycle-", dir=temp_root) as d:
    binary = Path(d) / "allocator-lifecycle"
    command = [args.compiler, *flags, "tests/memory/allocator_benchmark.c",
               "-o", str(binary)]
    subprocess.run(command, cwd=ROOT, check=True)
    for trial in range(args.trials):
        run = subprocess.run([str(binary), "lifecycle"], cwd=ROOT,
                             text=True, capture_output=True, check=True)
        trial_rows = []
        sample_numbers = {request: 0 for request in all_samples}
        trial_counts = {request: {"backing_allocations": 0,
                                  "backing_releases": 0,
                                  "empty_slab_reuses": 0,
                                  "retirements": 0}
                        for request in all_samples}
        classifier = None
        setup_drain = final_drain = None
        for line in run.stdout.splitlines():
            fields = line.split(",")
            if fields[0] == "lifecycle" and len(fields) == 5:
                _, request_text, sample_text, alloc_text, free_text = fields
                request, sample, alloc, free = map(
                    int, (request_text, sample_text, alloc_text, free_text))
                if request not in all_samples or sample != sample_numbers[request]:
                    raise SystemExit("missing, duplicate, or unexpected lifecycle sample")
                sample_numbers[request] += 1
                row = {"trial": trial, "request": request, "sample": sample,
                       "allocation_ns": alloc, "free_ns": free}
                all_samples[request].append(row)
                trial_rows.append(row)
            elif fields[0] == "lifecycle_stats" and len(fields) == 8:
                (_, request_text, sample_text, allocs, releases, reuses,
                 _empty, retired) = fields
                request, sample, allocs, releases, reuses, retired = map(
                    int, (request_text, sample_text, allocs, releases,
                          reuses, retired))
                if request not in trial_counts or sample != sample_numbers[request] - 1:
                    raise SystemExit("lifecycle counters do not match sample")
                for key, value in (("backing_allocations", allocs),
                                   ("backing_releases", releases),
                                   ("empty_slab_reuses", reuses),
                                   ("retirements", retired)):
                    trial_counts[request][key] += value
                    counter_totals[request][key] += value
            elif fields[0] == "lifecycle_setup_drain" and len(fields) == 2:
                setup_drain = int(fields[1])
            elif fields[0] == "lifecycle_final_drain" and len(fields) == 2:
                final_drain = int(fields[1])
            elif fields[0] == "lifecycle_classifier" and len(fields) == 5:
                classifier = list(map(int, fields[1:]))
            else:
                raise SystemExit(f"malformed lifecycle output: {line!r}")
        if any(value != 32 for value in sample_numbers.values()):
            raise SystemExit("lifecycle trial has missing samples")
        expected = ({"backing_allocations": 1, "backing_releases": 0,
                     "empty_slab_reuses": 31, "retirements": 0}
                    if args.reserve else
                    {"backing_allocations": 32, "backing_releases": 32,
                     "empty_slab_reuses": 0, "retirements": 32})
        if any(trial_counts[request] != expected for request in all_samples):
            raise SystemExit("lifecycle ownership counters disagree with policy")
        if setup_drain != 0 or final_drain != (3 if args.reserve else 0):
            raise SystemExit("lifecycle reserve drain count is incorrect")
        if classifier is None:
            raise SystemExit("missing classifier memory report")
        raw_trials.append({"trial": trial, "samples": trial_rows,
                           "counters": trial_counts,
                           "setup_drain": setup_drain,
                           "final_drain": final_drain})
        classifier_stats.append(classifier)

if any(len(rows) != 32 * args.trials for rows in all_samples.values()):
    raise SystemExit("aggregate lifecycle sample count is incomplete")

summary = {
    "policy": "one empty slab per class" if args.reserve else "immediate retirement",
    "source_revision": subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True,
        capture_output=True, check=True).stdout.strip(),
    "source_sha256": {
        "kernel/src/slab.c": sha256(ROOT / "kernel/src/slab.c"),
        "tests/memory/allocator_benchmark.c": sha256(ROOT / "tests/memory/allocator_benchmark.c"),
        "tests/memory/slab_lifecycle_policy.py": sha256(Path(__file__).resolve()),
    },
    "compiler": args.compiler,
    "compiler_version": version,
    "flags": flags,
    "platform": platform.platform(),
    "machine": platform.machine(),
    "workload": "five independent processes by default; each process measures 32 allocation/final-free cycles for 16, 72, and 384-byte requests after a fresh heap initialization. Per-operation timing brackets exclude stats reads. Reserve mode reuses the retained slab after the first cycle.",
    "timer": "production host adapter clock_gettime(CLOCK_MONOTONIC); nanoseconds are host-only, not Pith or hardware latency",
    "requested_bytes": [16, 72, 384],
    "raw_samples": {str(request): rows for request, rows in all_samples.items()},
    "summary_ns": {
        str(request): {
            operation: {
                "p50": statistics.median([row[f"{operation}_ns"] for row in rows]),
                "p95": percentile([row[f"{operation}_ns"] for row in rows], 95),
                "p99": percentile([row[f"{operation}_ns"] for row in rows], 99),
            }
            for operation in ("allocation", "free")
        }
        for request, rows in all_samples.items()
    },
    "ownership_counters": counter_totals,
    "classifier_payload_bytes_per_trial": classifier_stats,
    "maximum_retained_slab_bytes": 20 * 4096 if args.reserve else 0,
    "raw_trials": raw_trials,
    "limitations": [
        "host adapter measurements exercise production slab.c but not Pith interrupt, scheduler, PMM, or VMM costs",
        "the workload isolates repeated single-object final-free churn; it is not a claim about workload frequency in the kernel",
        "QEMU TCG and physical-hardware performance are not measured by this fixture",
    ],
}
output.write_text(json.dumps(summary, indent=2) + "\n")
print(f"wrote {output.relative_to(ROOT)}")
