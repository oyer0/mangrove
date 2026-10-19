#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare indexed-only and hybrid public allocation APIs with real sources."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import resource
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
REQUESTS = (16, 72, 192, 384)
SAMPLES = 128
PAIRS_PER_SAMPLE = 1000
WORKERS = 4
PAIRS_PER_WORKER = 20000
WITNESS_PAIRS = 2048
PROFILE_REPEATS = 10
PROFILE_SEED = 0x4D475246


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def percentile(values, percent):
    ordered = sorted(values)
    return ordered[max(0, (len(ordered) * percent + 99) // 100 - 1)]


def normalize_samples(samples):
    """Convert raw (pair_count, elapsed_ns) records without ambient state."""
    return {
        str(request): [elapsed_ns / pair_count
                       for pair_count, elapsed_ns in records.values()]
        for request, records in samples.items()
    }


def self_test():
    # The concurrent worker count is 20,000, but individual samples contain
    # 1,000 pairs. Keep the conversion tied to each record's own count.
    result = normalize_samples({16: {0: (1000, 110000)}})
    if result != {"16": [110.0]}:
        raise SystemExit("sample normalization used the wrong pair count")
    if (percentile([1, 2, 3, 4], 50) != 2 or
            percentile([1, 2, 3, 4], 95) != 4):
        raise SystemExit("sample percentile calculation failed")
    print("Public allocator benchmark metrics: PASS")


def expected_checksum(worker, request, pairs, witness_pairs):
    mixed = (16, 72, 192, 384)
    checksum = 0
    for count in (witness_pairs, pairs):
        for operation in range(count):
            size = request or mixed[(operation + worker) % len(mixed)]
            first = (0x5D ^ worker ^ operation) & 0xFF
            last = first ^ 0xA7
            checksum += (((first << 8) | last) ^ (size << 16))
    return checksum


def parse_run(path, mode, variant):
    lines = Path(path).read_text().splitlines()
    samples = {request: {} for request in REQUESTS}
    warmups = {}
    concurrent = {}
    witnesses = {}
    workers = {}
    host_cpus = {}
    public_classes = {}
    resources = None
    for line in lines:
        fields = line.split(",")
        if fields[0] == "warmup":
            if len(fields) != 4 or fields[1] != mode:
                raise ValueError("invalid warm-up record")
            request, count = map(int, fields[2:])
            if request in warmups:
                raise ValueError("duplicate warm-up record")
            warmups[request] = count
        elif fields[0] == "sample":
            if len(fields) != 6 or fields[1] != mode:
                raise ValueError("invalid sample record")
            request, index, pairs, elapsed = map(int, fields[2:])
            if request not in samples or index in samples[request] or elapsed <= 0:
                raise ValueError("invalid or duplicate sample")
            samples[request][index] = (pairs, elapsed)
        elif fields[0] == "host_cpu":
            worker, cpu = map(int, fields[1:])
            if worker in host_cpus:
                raise ValueError("duplicate host CPU record")
            host_cpus[worker] = cpu
        elif fields[0] == "concurrent_witness":
            request, count, peak, distinct, pairs = map(int, fields[1:])
            if request in witnesses:
                raise ValueError("duplicate overlap witness")
            witnesses[request] = {
                "workers": count, "peak_active_pairs": peak,
                "distinct_cpus": distinct, "witness_pairs": pairs,
            }
        elif fields[0] == "concurrent_worker":
            (request, worker, logical_cpu, host_cpu, cpu_start, cpu_end,
             witness_pairs, timed_pairs, checksum) = map(int, fields[1:])
            records = workers.setdefault(request, {})
            if worker in records:
                raise ValueError("duplicate worker record")
            records[worker] = {
                "logical_cpu": logical_cpu, "host_cpu": host_cpu,
                "cpu_start": cpu_start, "cpu_end": cpu_end,
                "witness_pairs": witness_pairs, "timed_pairs": timed_pairs,
                "checksum": checksum,
            }
        elif fields[0] == "concurrent":
            if len(fields) != 6 or fields[1] != mode:
                raise ValueError("invalid concurrent record")
            request, count, pairs, elapsed = map(int, fields[2:])
            if request in concurrent or elapsed <= 0:
                raise ValueError("duplicate or invalid concurrent record")
            concurrent[request] = {
                "workers": count, "pairs_per_worker": pairs,
                "elapsed_ns": elapsed,
            }
        elif fields[0] == "public_class":
            values = list(map(int, fields[1:]))
            if len(values) != 11 or values[0] in public_classes:
                raise ValueError("invalid public class accounting")
            public_classes[values[0]] = values[1:]
        elif fields[0] == "public_resources":
            if resources is not None or len(fields) != 4:
                raise ValueError("invalid public resource record")
            resources = dict(zip(("heap_used_payload_bytes",
                                  "classifier_payload_bytes",
                                  "committed_virtual_bytes"),
                                 map(int, fields[1:])))
        elif fields[0] in ("cache_configuration", "heap_memory", "class",
                           "cache", "concurrent_class", "concurrent_cache"):
            continue
        else:
            raise ValueError(f"unrecognized benchmark output: {line!r}")

    if warmups != {request: 64 for request in REQUESTS}:
        raise ValueError("missing or invalid warm-up")
    if any(len(records) != SAMPLES for records in samples.values()):
        raise ValueError("missing timing samples")
    for request in REQUESTS:
        if any(samples[request][index][0] != PAIRS_PER_SAMPLE
               for index in range(SAMPLES)):
            raise ValueError("timed pair count mismatch")
    if (set(concurrent) != {16, 72, 0} or set(witnesses) != set(concurrent) or
            set(workers) != set(concurrent)):
        raise ValueError("incomplete concurrent worker evidence")
    for request, witness in witnesses.items():
        records = workers[request]
        if (witness["workers"] != WORKERS or
                witness["distinct_cpus"] != WORKERS or
                witness["witness_pairs"] != WORKERS * WITNESS_PAIRS or
                witness["peak_active_pairs"] < 2 or
                set(records) != set(range(WORKERS)) or
                concurrent[request]["workers"] != WORKERS or
                concurrent[request]["pairs_per_worker"] !=
                    PAIRS_PER_WORKER):
            raise ValueError("concurrent participation or overlap missing")
        logical = set()
        physical = set()
        for worker, record in records.items():
            if (record["witness_pairs"] != WITNESS_PAIRS or
                    record["timed_pairs"] != PAIRS_PER_WORKER or
                    record["checksum"] != expected_checksum(
                        worker, request, PAIRS_PER_WORKER, WITNESS_PAIRS) or
                    record["host_cpu"] != record["cpu_start"] or
                    record["host_cpu"] != record["cpu_end"] or
                    record["host_cpu"] != host_cpus.get(worker)):
                raise ValueError("worker identity, checksum, or count invalid")
            logical.add(record["logical_cpu"])
            physical.add(record["host_cpu"])
        if len(logical) != WORKERS or len(physical) != WORKERS:
            raise ValueError("workers did not use distinct CPUs")
    if variant == "hybrid":
        if len(public_classes) != 20 or resources is None:
            raise ValueError("hybrid path lacks allocator counters")
    return {
        "mode": mode,
        "sample_ns_per_pair": normalize_samples(samples),
        "concurrent": {
            str(request): {
                **record,
                "pairs_per_second":
                    WORKERS * PAIRS_PER_WORKER * 1e9 / record["elapsed_ns"],
                "overlap_witness": witnesses[request],
            }
            for request, record in concurrent.items()},
        "public_class_counters": public_classes,
        "public_resources_before_cache_drain": resources,
        "raw_path": str(path.relative_to(ROOT)),
    }


def build_binary(compiler, output, hybrid, assembly):
    flags = ["-std=gnu11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
             "-pthread", "-Iinclude", "-Ikernel/include",
             "-idirafter", "libc/include", "-DSLAB_DEEP_VALIDATION=0"]
    if hybrid:
        flags.append("-DKERNEL_HYBRID_ALLOCATOR=1")
    subprocess.run([compiler, *flags, "-S", "tests/allocator_benchmark.c",
                    "-o", str(assembly)], cwd=ROOT, check=True)
    subprocess.run([compiler, *flags, "tests/allocator_benchmark.c",
                    "-o", str(output)], cwd=ROOT, check=True)


def run(binary, arguments, output):
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    result = subprocess.run([str(binary), *arguments], cwd=ROOT, text=True,
                            capture_output=True, preexec_fn=no_core)
    output.write_text(result.stdout + result.stderr)
    return result


def summarize_trials(records):
    summary = {}
    distributions = {}
    for request in REQUESTS:
        trial_medians = [statistics.median(record["sample_ns_per_pair"][
            str(request)]) for record in records]
        samples = [sample for record in records
                   for sample in record["sample_ns_per_pair"][str(request)]]
        summary[str(request)] = {
            "median_ns_per_pair": statistics.median(trial_medians),
            "trial_median_min_ns_per_pair": min(trial_medians),
            "trial_median_max_ns_per_pair": max(trial_medians),
            "trial_medians_ns_per_pair": trial_medians,
        }
        distributions[str(request)] = {
            "sample_count": len(samples),
            "p50_ns_per_pair": percentile(samples, 50),
            "p95_ns_per_pair": percentile(samples, 95),
            "p99_ns_per_pair": percentile(samples, 99),
        }
    concurrent = {}
    for request in ("16", "72", "0"):
        values = [record["concurrent"][request]["pairs_per_second"]
                  for record in records]
        concurrent[request] = {
            "median_pairs_per_second": statistics.median(values),
            "trial_min_pairs_per_second": min(values),
            "trial_max_pairs_per_second": max(values),
            "trial_values_pairs_per_second": values,
        }
    return {"single_worker_ns_per_pair": summary,
            "single_worker_latency_distributions": distributions,
            "four_worker_pairs_per_second": concurrent}


parser = argparse.ArgumentParser(description=__doc__)
if sys.argv[1:] == ["--self-test"]:
    self_test()
    raise SystemExit(0)
parser.add_argument("compiler", nargs="?", default="gcc")
parser.add_argument("--profile-summary", type=Path, required=True,
                    help="accepted heap-profile summary.json from Pith")
parser.add_argument("--output", type=Path,
                    default=Path("build/stage4_6-hybrid-public.json"))
parser.add_argument("--trials", type=int, default=5)
args = parser.parse_args()
if args.trials < 3:
    parser.error("at least three independent trials are required")
output = args.output if args.output.is_absolute() else ROOT / args.output
profile_summary = (args.profile_summary if args.profile_summary.is_absolute()
                   else ROOT / args.profile_summary)
if output.exists():
    parser.error(f"refusing to overwrite evidence: {output}")
build_dir = output.with_suffix("").with_name(output.stem + "-build")
if build_dir.exists():
    parser.error(f"refusing to overwrite build artifacts: {build_dir}")
output.parent.mkdir(parents=True, exist_ok=True)
build_dir.mkdir(parents=True)
raw_dir = build_dir / "raw"
raw_dir.mkdir()
trace_path = build_dir / "profile-size-trace.txt"

profile = json.loads(profile_summary.read_text())
if profile.get("status") != "PASS" or \
        profile.get("format") != "PITH-HEAP-PROFILE-3":
    parser.error("source profile is not independently accepted Pith evidence")
size_counts = {int(row["requested_bytes"]): int(row["successes"])
               for row in profile["exact_size_histogram"]
               if 0 < int(row["requested_bytes"]) <= 384 and
               int(row["successes"])}
if not size_counts:
    parser.error("source profile has no exact small-allocation histogram")
trace = [size for size, count in sorted(size_counts.items())
         for _ in range(count)]
import random
random.Random(PROFILE_SEED).shuffle(trace)
trace_path.write_text("".join(f"{size}\n" for size in trace))

version = subprocess.run([args.compiler, "--version"], cwd=ROOT, text=True,
                         capture_output=True, check=True).stdout.splitlines()[0]
revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True,
                          capture_output=True, check=True).stdout.strip()
source_diff = subprocess.run(["git", "diff", "HEAD", "--"], cwd=ROOT,
                             capture_output=True, check=True).stdout
indexed_binary = build_dir / "indexed-only"
hybrid_binary = build_dir / "hybrid"
build_binary(args.compiler, indexed_binary, False,
             build_dir / "indexed-only.s")
build_binary(args.compiler, hybrid_binary, True, build_dir / "hybrid.s")

# Both API configurations must preserve invalid-free and double-free failures.
api_controls = {}
for name, binary in (("indexed", indexed_binary), ("hybrid", hybrid_binary)):
    check_path = raw_dir / f"{name}-api-check.txt"
    check = run(binary, ["api-check"], check_path)
    if check.returncode or "public_allocator_api_check,PASS" not in check.stdout:
        raise SystemExit(f"{name} public allocation contract failed")
    controls = {}
    for mode in ("api-invalid", "api-double-free", "api-interior-free"):
        control_path = raw_dir / f"{name}-{mode}.txt"
        result = run(binary, [mode], control_path)
        controls[mode] = {"rejected": result.returncode != 0,
                          "returncode": result.returncode,
                          "raw_path": str(control_path.relative_to(ROOT))}
        if result.returncode == 0:
            raise SystemExit(f"{name} negative control accepted {mode}")
    api_controls[name] = controls

pressure_path = raw_dir / "hybrid-pressure-recovery.txt"
pressure = run(hybrid_binary, ["pressure-reclaim"], pressure_path)
pressure_rows = [line.split(",") for line in pressure.stdout.splitlines()
                 if line.startswith("public_pressure_recovery,")]
if pressure.returncode or len(pressure_rows) != 1 or \
        pressure_rows[0][1] != "PASS" or \
        any(int(value) <= 0 for value in pressure_rows[0][2:]):
    raise SystemExit("hybrid pressure recovery evidence failed")
pressure_result = {
    "record": pressure_rows[0],
    "raw_path": str(pressure_path.relative_to(ROOT)),
}

standard = {"indexed": [], "shared_slab": [], "per_cpu_slab": [],
            "hybrid_public": []}
for trial in range(args.trials):
    for variant, binary, command, key, mode in (
        ("indexed", indexed_binary, "heap", "indexed", "heap"),
        ("indexed", indexed_binary, "slab", "shared_slab", "slab"),
        ("indexed", indexed_binary, "cpu-slab", "per_cpu_slab", "cpu-slab"),
        ("hybrid", hybrid_binary, "public-api", "hybrid_public", "public-api"),
    ):
        raw_path = raw_dir / f"{key}-trial-{trial}.csv"
        result = run(binary, [command], raw_path)
        if result.returncode:
            raise SystemExit(f"{key} benchmark failed: {raw_path}")
        standard[key].append(parse_run(raw_path, mode, variant))

profile_results = {"indexed": [], "shared_slab": [], "per_cpu_slab": [],
                   "hybrid_public": []}
for trial in range(args.trials):
    for variant, binary, command, key in (
        ("indexed", indexed_binary, "profile-heap", "indexed"),
        ("indexed", indexed_binary, "profile-slab", "shared_slab"),
        ("indexed", indexed_binary, "profile-cpu-slab", "per_cpu_slab"),
        ("hybrid", hybrid_binary, "profile-public-api", "hybrid_public"),
    ):
        raw_path = raw_dir / f"profile-{key}-trial-{trial}.csv"
        result = run(binary, [command, str(trace_path)], raw_path)
        if result.returncode:
            raise SystemExit(f"profile replay failed: {raw_path}")
        lines = result.stdout.splitlines()
        replay = [line.split(",") for line in lines
                  if line.startswith("profile_replay,")]
        if len(replay) != 1 or replay[0][1] != command:
            raise SystemExit("missing or misidentified profile replay")
        count, repeats, elapsed, checksum = map(int, replay[0][2:])
        if (count != len(trace) or repeats != PROFILE_REPEATS or
                elapsed <= 0 or checksum == 0):
            raise SystemExit("profile replay work/count/checksum invalid")
        class_rows = [line.split(",") for line in lines
                      if line.startswith("profile_class,")]
        classifier_rows = [line.split(",") for line in lines
                           if line.startswith("profile_classifier,")]
        if key == "indexed":
            if class_rows or classifier_rows:
                raise SystemExit("indexed replay unexpectedly used slabs")
        else:
            if len(class_rows) != 20 or len(classifier_rows) != 1:
                raise SystemExit("slab replay lacks ownership counters")
            if any(row[1] != command for row in class_rows + classifier_rows):
                raise SystemExit("slab replay counters identify another path")
            class_allocations = sum(int(row[4]) for row in class_rows)
            class_frees = sum(int(row[5]) for row in class_rows)
            expected_operations = count * (PROFILE_REPEATS + 1)
            if (class_allocations != expected_operations or
                    class_frees != expected_operations):
                raise SystemExit("slab replay counters disagree with requests")
            if key in ("per_cpu_slab", "hybrid_public"):
                drains = [line.split(",") for line in lines
                          if line.startswith("profile_drain,")]
                if len(drains) != 1 or int(drains[0][2]) < 0:
                    raise SystemExit("per-CPU replay lacks cache drain")
        profile_results[key].append({
            "elapsed_ns": elapsed,
            "pairs": count * repeats,
            "ns_per_pair": elapsed / (count * repeats),
            "checksum": checksum,
            "raw_path": str(raw_path.relative_to(ROOT)),
        })

# One simultaneously-live mixed-size population per public configuration.
population_results = {}
for variant, binary, command in (
    ("indexed", indexed_binary, "population-heap"),
    ("hybrid", hybrid_binary, "population-public-api"),
):
    raw_path = raw_dir / f"population-{variant}.csv"
    result = run(binary, [command], raw_path)
    if result.returncode:
        raise SystemExit(f"live population benchmark failed: {raw_path}")
    lines = result.stdout.splitlines()
    phases = [line.split(",") for line in lines
              if line.startswith("population_phase,")]
    results = [line.split(",") for line in lines
               if line.startswith("population_result,")]
    resources = [line.split(",") for line in lines
                 if line.startswith("population_resources,")]
    classifier = [line.split(",") for line in lines
                  if line.startswith("population_classifier,")]
    if (len(phases) != 4 or len(results) != 1 or len(resources) != 2 or
            len(classifier) != 2):
        raise SystemExit("population evidence incomplete")
    if int(results[0][2]) != 1024 or int(results[0][3]) == 0:
        raise SystemExit("population payload/operation validation failed")
    population_results[variant] = {
        "phase_records": phases,
        "resource_records": resources,
        "classifier_records": classifier,
        "payload_checksum": int(results[0][3]),
        "raw_path": str(raw_path.relative_to(ROOT)),
    }

# Negative control: the evidence validator rejects a missing timing sample.
negative_control = {"missing_sample_rejected": False}
sample_lines = [line for line in (raw_dir / "indexed-trial-0.csv")
                .read_text().splitlines() if line.startswith("sample,")]
try:
    parse_run(raw_dir / "indexed-trial-0.csv", "heap", "indexed")
    if sample_lines:
        mutated = raw_dir / "missing-sample-negative-control.csv"
        original = (raw_dir / "indexed-trial-0.csv").read_text().splitlines()
        removed = False
        for index, line in enumerate(original):
            if line.startswith("sample,"):
                del original[index]
                removed = True
                break
        mutated.write_text("\n".join(original) + "\n")
        try:
            parse_run(mutated, "heap", "indexed")
        except ValueError:
            negative_control["missing_sample_rejected"] = removed
except ValueError as error:
    raise SystemExit(f"valid sample control rejected: {error}") from error
if not negative_control["missing_sample_rejected"]:
    raise SystemExit("missing-sample negative control was not rejected")

summary = {
    "format": "MANGROVE-PUBLIC-ALLOCATOR-BENCHMARK-1",
    "status": "PASS",
    "source_profile": {
        "path": str(profile_summary.relative_to(ROOT)),
        "sha256": digest(profile_summary),
        "format": profile["format"],
        "cpu_online_count": profile["cpu_online_count"],
        "phase_boundary": profile["phase_boundary"],
        "small_successes_le_384": len(trace),
        "small_request_count_by_exact_size": size_counts,
        "trace_seed": PROFILE_SEED,
        "trace_path": str(trace_path.relative_to(ROOT)),
        "trace_sha256": digest(trace_path),
        "lifetime_limitation": "The Pith profile provides size and operation-count lifetime histograms, not an ordered pointer/lifetime trace. Replay preserves the exact size multiset but uses immediate allocation/free pairs.",
    },
    "build": {
        "compiler": version,
        "optimization": "-O2",
        "flags": ["-std=gnu11", "-O2", "-g", "-Wall", "-Wextra",
                  "-Werror", "-pthread", "SLAB_DEEP_VALIDATION=0"],
        "host": platform.platform(),
        "python": sys.version,
        "git_head": revision,
        "working_diff_sha256": hashlib.sha256(source_diff).hexdigest(),
        "source_sha256": {
            path: digest(ROOT / path) for path in (
                "tests/allocator_benchmark.c",
                "tests/hybrid_public_benchmark.py",
                "kernel/src/heap.c", "kernel/src/slab.c",
                "kernel/include/heap.h", "kernel/include/slab.h",
                "kernel/include/spinlock.h")},
        "indexed_binary": str(indexed_binary.relative_to(ROOT)),
        "hybrid_binary": str(hybrid_binary.relative_to(ROOT)),
        "assembly": [str((build_dir / "indexed-only.s").relative_to(ROOT)),
                     str((build_dir / "hybrid.s").relative_to(ROOT))],
    },
    "method": {
        "trials": args.trials,
        "standard": "Each fresh process warms each request with 64 pairs, takes 128 samples of 1,000 public API or direct-core pairs at 16/72/192/384 bytes, then runs pinned four-worker 20,000-pair tests for 16, 72, and mixed sizes. Worker CPU identity, exact counts, checksums, distinct host CPUs, and an overlap witness are validated.",
        "profile_replay": f"Each fresh process warms one exact-size histogram pass then runs {PROFILE_REPEATS} timed passes; only requests <=384 bytes are included. Lifetime is immediate; the replay is size-frequency weighted, not a full Pith trace.",
        "population": "A 1,024-object simultaneously-live mixed request set includes 385-byte heap-boundary objects; alternates freeing/replacing half and verifies payload sentinels, live ranges, and production allocator resource counters.",
        "timing": "Linux CLOCK_MONOTONIC wall time on the host; workers pin to four distinct CPUs if available. Host adapter measurements are not Pith bare-metal results.",
    },
    "public_api_contract_controls": api_controls,
    "pressure_recovery": pressure_result,
    "standard_results": {
        key: {"trials": records, "summary": summarize_trials(records)}
        for key, records in standard.items()},
    "profile_replay_results": {
        key: {"trials": records,
              "median_ns_per_pair": statistics.median(
                  record["ns_per_pair"] for record in records),
              "trial_min_ns_per_pair": min(
                  record["ns_per_pair"] for record in records),
              "trial_max_ns_per_pair": max(
                  record["ns_per_pair"] for record in records)}
        for key, records in profile_results.items()},
    "live_population": population_results,
    "negative_controls": negative_control,
    "limitations": [
        "Host adapters reproduce allocator logic and bounded synchronization interfaces but not Pith scheduler, interrupt, remote-TLB, or DMA behavior.",
        "Profile-size replay preserves exact sizes/frequencies only for requests at most 384 bytes; it does not replay pointer lifetimes, ordering, call contexts, or larger requests.",
        "The Pith profile covers the captured boot, Shoot, Reed, and filesystem scenario only; e1000 was configured but no receive traffic was generated.",
        "The live-population footprint is an adapter workload, not physical-hardware memory use. Heap committed bytes are virtual mappings in the adapter and do not imply PMM frame return.",
    ],
    "raw_directory": str(raw_dir.relative_to(ROOT)),
}
output.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
print(f"Public allocator benchmark: PASS ({args.trials} independent trials)")
print(f"summary: {output.relative_to(ROOT)}")
