#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare production heap and slab paths under one bounded host adapter."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def percentile(values, percent):
    ordered = sorted(values)
    rank = max(0, (len(ordered) * percent + 99) // 100 - 1)
    return ordered[rank]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("compiler", nargs="?", default="gcc")
parser.add_argument("--output", type=Path,
                    default=ROOT / "build/stage4e2-slab-benchmark.json")
args = parser.parse_args()
output = args.output if args.output.is_absolute() else ROOT / args.output
output.parent.mkdir(parents=True, exist_ok=True)
temp_root = ROOT / "build/stage4e2-tmp"
temp_root.mkdir(parents=True, exist_ok=True)
flags = ["-std=gnu11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
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

summary = {
    "description": "production heap and slab allocator/free pairs under the same host arena, atomic spinlock, PMM, and VMM adapters",
    "limitations": [
        "host pthread scheduling and clock; not a Pith or hardware timing",
        "steady-state samples keep one anchor live and exclude backing refill",
        "initial heap capacity is 16 KiB; timed requests do not grow it",
        "four-worker throughput is a host contention proxy, not SMP qualification",
        "profile is fixed at -O2 without sanitizers; correctness tests run separately with ASan/UBSan",
    ],
    "compiler": args.compiler,
    "compiler_version": compiler_version,
    "flags": flags,
    "platform": platform.platform(),
    "machine": platform.machine(),
    "source_sha256": {
        "kernel/src/heap.c": digest(ROOT / "kernel/src/heap.c"),
        "kernel/src/slab.c": digest(ROOT / "kernel/src/slab.c"),
        "tests/allocator_benchmark.c": digest(ROOT / "tests/allocator_benchmark.c"),
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
        "classifier_bytes_target": 134217728,
    },
    "results": {},
    "raw_batch_ns": raw_elapsed,
    "concurrent_results": concurrent,
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
