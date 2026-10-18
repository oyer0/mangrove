#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare production heap and slab paths under one bounded host adapter."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DENSE_BASELINE_REVISION = "9d1c03a182dbc4f42337bb8accc368c958025533"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def percentile(values, percent):
    ordered = sorted(values)
    rank = max(0, (len(ordered) * percent + 99) // 100 - 1)
    return ordered[rank]


def validate_classifier_samples(samples, sample_count, lookups):
    """Reject missing work and candidates that do not classify the same pages."""
    for mode in ("sparse", "direct_root", "dense"):
        records = samples.get(mode, [])
        if len(records) != sample_count:
            raise ValueError(f"incomplete {mode} classifier samples")
        for index, record in enumerate(records):
            if (record["sample"] != index or
                    record["lookups"] != lookups):
                raise ValueError(f"invalid {mode} sample metadata")
    for index in range(sample_count):
        sparse = samples["sparse"][index]
        root = samples["direct_root"][index]
        dense = samples["dense"][index]
        if sparse["checksum"] != root["checksum"]:
            raise ValueError("classifier candidate checksums differ")
        if dense["checksum"] != 5 * lookups:
            raise ValueError("dense classifier checksum is incorrect")


def validate_classifier_negative_control():
    """Prove the result validator rejects a deliberately wrong lookup result."""
    good = {
        mode: [{"sample": 0, "lookups": 7,
                "checksum": 123 if mode != "dense" else 35}]
        for mode in ("sparse", "direct_root", "dense")
    }
    validate_classifier_samples(good, 1, 7)
    bad = {mode: [dict(record) for record in records]
           for mode, records in good.items()}
    bad["direct_root"][0]["checksum"] += 1
    try:
        validate_classifier_samples(bad, 1, 7)
    except ValueError:
        return
    raise SystemExit("classifier checksum negative control was not rejected")


def validate_bitmap_record(record, expected_slot, expected_found, iterations):
    if (record["found"] != expected_found * iterations or
            record["checksum"] != expected_slot * expected_found * iterations):
        raise ValueError("bitmap selector result is incorrect")


def validate_bitmap_negative_control():
    good = {"found": 7, "checksum": 7 * 249}
    validate_bitmap_record(good, 249, 1, 7)
    bad = dict(good, checksum=good["checksum"] - 7)
    try:
        validate_bitmap_record(bad, 249, 1, 7)
    except ValueError:
        return
    raise SystemExit("bitmap result negative control was not rejected")


validate_classifier_negative_control()
validate_bitmap_negative_control()


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("compiler", nargs="?", default="gcc")
parser.add_argument("--output", type=Path,
                    default=ROOT / "build/stage4e3a-slab-benchmark.json")
args = parser.parse_args()
output = args.output if args.output.is_absolute() else ROOT / args.output
output.parent.mkdir(parents=True, exist_ok=True)
temp_root = ROOT / "build/stage4e3a-tmp"
temp_root.mkdir(parents=True, exist_ok=True)
flags = ["-std=gnu11", "-O2", "-g", "-save-temps=obj",
         "-Wall", "-Wextra", "-Werror",
         "-pthread", "-Iinclude", "-Ikernel/include",
         "-idirafter", "libc/include"]
compiler_version = subprocess.run([args.compiler, "--version"], cwd=ROOT,
                                  text=True, capture_output=True,
                                  check=True).stdout.splitlines()[0]

with tempfile.TemporaryDirectory(prefix="allocator-bench-", dir=temp_root) as directory:
    binary = Path(directory) / "allocator-bench"
    command = [args.compiler, *flags, "tests/allocator_benchmark.c",
               "-o", str(binary)]
    subprocess.run(command, cwd=ROOT, check=True)
    raw = {}
    for mode in ("heap", "slab"):
        run = subprocess.run([str(binary), mode], cwd=ROOT, text=True,
                             capture_output=True, check=True)
        raw[mode] = run.stdout.splitlines()
    classifier_run = subprocess.run([str(binary), "classifier"], cwd=ROOT,
                                     text=True, capture_output=True,
                                     check=True)
    bitmap_run = subprocess.run([str(binary), "bitmap"], cwd=ROOT,
                                text=True, capture_output=True, check=True)
    phase_runs = {
        mode: subprocess.run([str(binary), mode], cwd=ROOT, text=True,
                             capture_output=True, check=True).stdout.splitlines()
        for mode in ("phases-heap", "phases-slab")
    }
    lifecycle_run = subprocess.run([str(binary), "lifecycle"], cwd=ROOT,
                                   text=True, capture_output=True,
                                   check=True)

samples = {mode: {16: [], 72: [], 384: []} for mode in ("heap", "slab")}
raw_elapsed = {mode: {16: [], 72: [], 384: []}
               for mode in ("heap", "slab")}
concurrent = {mode: {} for mode in ("heap", "slab")}
classes = {}
for mode, lines in raw.items():
    for line in lines:
        fields = line.split(",")
        if fields[0] == "sample":
            _, row_mode, request, _sample, pairs, elapsed = fields
            assert row_mode == mode
            request, pairs, elapsed = map(int, (request, pairs, elapsed))
            samples[mode][request].append(elapsed / pairs)
            raw_elapsed[mode][request].append(elapsed)
        elif fields[0] == "concurrent":
            _, row_mode, request, workers, pairs, elapsed = fields
            assert row_mode == mode
            request, workers, pairs, elapsed = map(
                int, (request, workers, pairs, elapsed))
            concurrent[mode][request] = {
                "workers": workers,
                "pairs_per_worker": pairs,
                "elapsed_ns": elapsed,
                "pairs_per_second": workers * pairs * 1e9 / elapsed,
            }
        elif fields[0] == "concurrent_class":
            (request, locks, words, backing, live, peak, partial) = \
                map(int, fields[1:])
            concurrent["slab"][request].update({
                "lock_acquisitions": locks,
                "bitmap_words_examined": words,
                "backing_allocations": backing,
                "live_slots_at_snapshot": live,
                "peak_live_slots": peak,
                "partial_slabs_at_snapshot": partial,
            })
        elif fields[0] == "class":
            (request, class_bytes, slots, words, locks, backing, partial,
             live, peak, full) = \
                map(int, fields[1:])
            classes[request] = {
                "class_bytes": class_bytes,
                "slots_per_page": slots,
                "bitmap_words_examined": words,
                "lock_acquisitions": locks,
                "backing_allocations": backing,
                "partial_slabs_at_snapshot": partial,
                "live_slots_at_snapshot": live,
                "peak_live_slots": peak,
                "full_slabs_at_snapshot": full,
            }
        else:
            raise SystemExit(f"malformed benchmark output: {line!r}")

classifier_samples = {"sparse": [], "direct_root": [], "dense": []}
classifier_growth_ns = None
classifier_population_ns = None
classifier_payload = None
classifier_heap_used = None
classifier_pages = None
for line in classifier_run.stdout.splitlines():
    fields = line.split(",")
    if fields[0] == "classifier_growth" and len(fields) == 2:
        classifier_growth_ns = int(fields[1])
    elif fields[0] == "classifier_population" and len(fields) == 2:
        classifier_population_ns = int(fields[1])
    elif fields[0] == "lookup" and len(fields) == 6:
        _, mode, sample, lookups, elapsed, checksum = fields
        sample, lookups, elapsed, checksum = map(
            int, (sample, lookups, elapsed, checksum))
        if mode not in classifier_samples or lookups != 500000:
            raise SystemExit("classifier comparison workload mismatch")
        classifier_samples[mode].append({
            "sample": sample,
            "lookups": lookups,
            "ns_per_lookup": elapsed / lookups,
            "elapsed_ns": elapsed,
            "checksum": checksum,
        })
    elif fields[0] == "classifier_pages" and len(fields) == 4:
        classifier_pages = list(map(int, fields[1:]))
    elif fields[0] == "classifier_payload" and len(fields) == 7:
        classifier_payload = list(map(int, fields[1:]))
    elif fields[0] == "classifier_heap_used" and len(fields) == 2:
        classifier_heap_used = int(fields[1])
    else:
        raise SystemExit(f"malformed classifier output: {line!r}")
if (classifier_growth_ns is None or classifier_population_ns is None or
        classifier_payload is None or classifier_heap_used is None or
        classifier_pages is None or
        any(len(values) != 32 for values in classifier_samples.values())):
    raise SystemExit("incomplete classifier benchmark output")
try:
    validate_classifier_samples(classifier_samples, 32, 500000)
except ValueError as error:
    raise SystemExit(str(error)) from error
if classifier_heap_used < classifier_payload[4]:
    raise SystemExit("classifier heap accounting is below retained node payload")

dense_source = subprocess.run(
    ["git", "show", f"{DENSE_BASELINE_REVISION}:kernel/src/slab.c"],
    cwd=ROOT, check=True, capture_output=True).stdout

summary = {
    "description": "production heap and slab allocator/free pairs under the same host arena, atomic spinlock, PMM, and VMM adapters",
    "limitations": [
        "host pthread scheduling and clock; not a Pith or hardware timing",
        "steady-state samples keep one anchor live and exclude backing refill",
        "initial heap capacity is 16 KiB; timed requests do not grow it",
        "four-worker throughput is a host contention proxy, not SMP qualification",
        "when affinity has one CPU, all four workers share that CPU and do not measure multicore throughput",
        "profile is fixed at -O2 without sanitizers; correctness tests run separately with ASan/UBSan",
    ],
    "compiler": args.compiler,
    "compiler_version": compiler_version,
    "flags": flags,
    "timer": "clock_gettime(CLOCK_MONOTONIC); timing brackets include timer-read overhead",
    "warmup": "no dedicated cache warm-up; heap initialization and anchor allocation occur before steady-state samples",
    "platform": platform.platform(),
    "machine": platform.machine(),
    "revision": subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                               text=True, capture_output=True,
                               check=True).stdout.strip(),
    "cpu_affinity": sorted(os.sched_getaffinity(0))
        if hasattr(os, "sched_getaffinity") else None,
    "source_sha256": {
        "kernel/src/heap.c": digest(ROOT / "kernel/src/heap.c"),
        "kernel/src/slab.c": digest(ROOT / "kernel/src/slab.c"),
        "tests/allocator_benchmark.c": digest(ROOT / "tests/allocator_benchmark.c"),
        "tests/slab_benchmark.py": digest(Path(__file__).resolve()),
    },
    "dense_baseline": {
        "revision": DENSE_BASELINE_REVISION,
        "source_path": "kernel/src/slab.c",
        "source_sha256": hashlib.sha256(dense_source).hexdigest(),
        "lookup_contract": "historical acquire-load of one atomic class byte by heap page index",
    },
    "batch_pairs": 1000,
    "samples_per_size": 128,
    "units": "nanoseconds per allocation/free pair",
    "layout": {
        "page_bytes": 4096,
        "heap_block_header_bytes": 48,
        "slab_descriptor_bytes": 64,
        "slab_bitmap_bytes": 32,
        "slab_metadata_bytes": 96,
        "historical_dense_classifier_target_bytes": 134217728,
        "sparse_classifier_lookup_window_bytes": 2 * 1024 * 1024,
        "sparse_classifier_node_payload_bytes": 4112,
        "sparse_classifier_node_heap_header_bytes": 48,
    },
    "results": {},
    "raw_batch_ns": raw_elapsed,
    "concurrent_results": concurrent,
    "classifier_comparison": {
        "method": "32 samples of 500000 lookups across a deterministic pseudo-random permutation of 1024 live slab pages; all candidates use the same addresses, sample order, process, compiler build, and timer",
        "limitations": [
            "the dense reference measures only an atomic byte load and does not perform generation validation; it is a lower-bound reference, not equivalent ownership validation",
            "the direct-root candidate walks a copied root directory inline, bypassing slab_page_token_load's index guard and function overhead; it retains mapped-range and identity validation for middle and leaf nodes and is a lower bound",
            "the benchmark arena has 8 MiB capacity and commits allocator pages on demand; the page lookup working set spans two sparse leaves",
            "host hot-cache lookup timing is not Pith guest or physical-hardware performance",
            "classifier population includes 1024 slab pages and is not a steady-state latency",
        ],
        "live_slab_pages": classifier_pages[0],
        "requested_slab_pages": classifier_pages[1],
        "checksum_crosscheck": classifier_pages[2],
        "lookups_per_sample": 500000,
        "samples_per_lookup_kind": 32,
        "growth_first_use_ns": classifier_growth_ns,
        "population_ns": classifier_population_ns,
        "payload_bytes": {
            "root": classifier_payload[0],
            "static_root": classifier_payload[1],
            "middle": classifier_payload[2],
            "leaf": classifier_payload[3],
            "retained_dynamic": classifier_payload[4],
            "peak_dynamic": classifier_payload[5],
        },
        "heap_used_after_slab_retirement_bytes": classifier_heap_used,
        "lookup_ns": {},
    },
}
for lookup_kind, values in classifier_samples.items():
    timings = [sample["ns_per_lookup"] for sample in values]
    summary["classifier_comparison"]["lookup_ns"][lookup_kind] = {
        "p50": statistics.median(timings),
        "p95": percentile(timings, 95),
        "p99": percentile(timings, 99),
        "minimum": min(timings),
        "maximum": max(timings),
        "raw_samples": values,
    }

bitmap_samples = {}
bitmap_slots = {0: 250, 4: 50, 19: 10}
bitmap_occupancies = (0, 1, 2, 5, 10, 31, 63, 64, 65,
                      127, 128, 200, 249, 250)
for line in bitmap_run.stdout.splitlines():
    fields = line.split(",")
    if len(fields) != 9 or fields[0] != "bitmap":
        raise SystemExit(f"malformed bitmap benchmark output: {line!r}")
    kind = fields[1]
    class_index, occupancy, sample, iterations, elapsed, found, checksum = map(
        int, fields[2:])
    if (kind not in ("production", "historical") or
            class_index not in bitmap_slots or
            iterations != 10000 or occupancy > bitmap_slots[class_index]):
        raise SystemExit("bitmap search workload mismatch")
    expected_found = int(occupancy < bitmap_slots[class_index])
    try:
        validate_bitmap_record({"found": found, "checksum": checksum},
                               occupancy, expected_found, iterations)
    except ValueError as error:
        raise SystemExit(str(error)) from error
    bitmap_samples.setdefault(class_index, {}).setdefault(kind, {}).setdefault(
        occupancy, []).append(elapsed / iterations)
expected_bitmap_cases = {
    class_index: tuple(occupancy for occupancy in bitmap_occupancies
                       if occupancy <= slots)
    for class_index, slots in bitmap_slots.items()
}
if any(len(bitmap_samples.get(class_index, {}).get(kind, {}).get(
           occupancy, [])) != 17
       for class_index, occupancies in expected_bitmap_cases.items()
       for kind in ("production", "historical")
       for occupancy in occupancies):
    raise SystemExit("incomplete bitmap search samples")
summary["bitmap_search"] = {
    "method": "paired production selector and exact pre-change four-word linear-bit selector over the same 1024 runtime-addressed descriptors for the 16-, 80-, and 384-byte classes; bitmap setup outside timed regions; a varying descriptor index and result checksum keep lookup observable",
    "units": "nanoseconds per selector call",
    "iterations_per_sample": 10000,
    "samples_per_occupancy": 17,
    "negative_control": "parser rejects an intentionally incorrect slot checksum",
    "results": {
        str(class_index): {
            kind: {
                str(occupancy): {
                    "p50": statistics.median(values),
                    "p95": percentile(values, 95),
                    "p99": percentile(values, 99),
                    "minimum": min(values),
                    "maximum": max(values),
                }
                for occupancy, values in sorted(
                    bitmap_samples[class_index][kind].items())
            }
            for kind in ("historical", "production")
        }
        for class_index in sorted(expected_bitmap_cases)
    },
}
phase_values = {mode: {operation: {16: [], 72: [], 384: []}
                       for operation in ("alloc", "free")}
                for mode in ("heap", "slab")}
phase_operations = {}
for mode, lines in phase_runs.items():
    expected_mode = mode.split("-", 1)[1]
    for line in lines:
        fields = line.split(",")
        if len(fields) != 7 or fields[0] != "phase":
            raise SystemExit(f"malformed separated-operation output: {line!r}")
        _, row_mode, operation, request, sample, operations, elapsed = fields
        request, sample, operations, elapsed = map(
            int, (request, sample, operations, elapsed))
        if row_mode != expected_mode or operation not in ("alloc", "free"):
            raise SystemExit("separated-operation mode mismatch")
        phase_values[row_mode][operation][request].append(elapsed / operations)
        phase_operations.setdefault(row_mode, {}).setdefault(
            str(request), {"operations_per_sample": operations,
                           "samples": 0})["samples"] += 1
for mode in ("heap", "slab"):
    for operation in ("alloc", "free"):
        for request in (16, 72, 384):
            if len(phase_values[mode][operation][request]) != 64:
                raise SystemExit("incomplete separated-operation samples")

summary["separated_operations"] = {
    "method": "allocation and free batches measured independently while a high-occupancy live set and anchor remain allocated; batch is bounded by class capacity and is not a cold/refill measurement",
    "units": "nanoseconds per allocation or free",
    "results": {
        mode: {
            operation: {
                str(request): {
                    "p50": statistics.median(phase_values[mode][operation][request]),
                    "p95": percentile(phase_values[mode][operation][request], 95),
                    "p99": percentile(phase_values[mode][operation][request], 99),
                    "samples": phase_values[mode][operation][request],
                }
                for request in (16, 72, 384)
            }
            for operation in ("alloc", "free")
        }
        for mode in ("heap", "slab")
    },
    "batch_sizes_and_samples": phase_operations,
}

lifecycle_values = {request: {"alloc": [], "free": []}
                    for request in (16, 72, 384)}
for line in lifecycle_run.stdout.splitlines():
    fields = line.split(",")
    if len(fields) != 5 or fields[0] != "lifecycle":
        raise SystemExit(f"malformed lifecycle output: {line!r}")
    _, request, _sample, allocation, free = fields
    request, allocation, free = map(int, (request, allocation, free))
    lifecycle_values[request]["alloc"].append(allocation)
    lifecycle_values[request]["free"].append(free)
if any(len(lifecycle_values[request][op]) != 32
       for request in lifecycle_values for op in ("alloc", "free")):
    raise SystemExit("incomplete refill/retirement samples")
summary["refill_retirement"] = {
    "method": "one allocation from an empty class followed by freeing its last live slot; first sample includes classifier node creation, subsequent samples reuse retained classification metadata",
    "units": "nanoseconds per operation, timer overhead included",
    "results": {
        str(request): {
            operation: {
                "p50": statistics.median(values),
                "p95": percentile(values, 95),
                "p99": percentile(values, 99),
                "samples": values,
            }
            for operation, values in lifecycle_values[request].items()
        }
        for request in lifecycle_values
    },
}
for mode in ("heap", "slab"):
    summary["results"][mode] = {}
    for request, values in samples[mode].items():
        if len(values) != 128:
            raise SystemExit(f"{mode} {request}: incomplete sample count {len(values)}")
        result = {
            "p50": statistics.median(values),
            "p95": percentile(values, 95),
            "p99": percentile(values, 99),
            "minimum": min(values),
            "maximum": max(values),
        }
        if mode == "slab":
            class_data = classes[request]
            slots = class_data["slots_per_page"]
            class_bytes = class_data["class_bytes"]
            live = class_data["live_slots_at_snapshot"]
            class_data.update({
                "in_page_metadata_bytes": 96,
                "object_area_bytes": 4000,
                "slot_capacity_bytes": slots * class_bytes,
                "unused_page_tail_bytes": 4000 - slots * class_bytes,
                "requested_bytes_at_snapshot": live * request,
                "class_rounding_waste_at_snapshot": live * (class_bytes - request),
                "unoccupied_slots_at_snapshot": slots - live,
                "unoccupied_slot_capacity_bytes": (slots - live) * class_bytes,
                "backing_payload_bytes_at_snapshot":
                    4096 * (class_data["partial_slabs_at_snapshot"] +
                            class_data["full_slabs_at_snapshot"]),
            })
            result.update(class_data)
        summary["results"][mode][str(request)] = result

summary["comparison"] = {}
for request in (16, 72, 384):
    heap_p50 = summary["results"]["heap"][str(request)]["p50"]
    slab_p50 = summary["results"]["slab"][str(request)]["p50"]
    summary["comparison"][str(request)] = {
        "slab_to_heap_p50_ratio": slab_p50 / heap_p50,
        "lower_p50": "slab" if slab_p50 < heap_p50 else "heap",
    }

output.write_text(json.dumps(summary, indent=2) + "\n")
for request in (16, 72, 384):
    for mode in ("heap", "slab"):
        result = summary["results"][mode][str(request)]
        print(f"{mode} {request} bytes: p50={result['p50']:.1f} ns, "
              f"p95={result['p95']:.1f} ns, p99={result['p99']:.1f} ns/pair")
    print(f"  slab/heap p50 ratio: "
          f"{summary['comparison'][str(request)]['slab_to_heap_p50_ratio']:.3f}")
for request in (16, 72):
    for mode in ("heap", "slab"):
        rate = concurrent[mode][request]["pairs_per_second"]
        print(f"{mode} {request} bytes: {rate:.0f} four-worker pairs/s")
print(f"raw samples and source hashes: {output}")
