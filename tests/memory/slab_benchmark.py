#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare production heap and slab paths under one bounded host adapter."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DENSE_BASELINE_REVISION = "9d1c03a182dbc4f42337bb8accc368c958025533"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_benchmark(binary, mode, output):
    try:
        return subprocess.run([str(binary), mode], cwd=ROOT, text=True,
                              capture_output=True, check=True)
    except subprocess.CalledProcessError as error:
        failure_path = output.with_suffix(f".failure-{mode}.txt")
        failure_path.write_text(
            f"command: {error.cmd!r}\nreturncode: {error.returncode}\n"
            f"--- stdout ---\n{error.stdout or ''}"
            f"\n--- stderr ---\n{error.stderr or ''}")
        raise RuntimeError(
            f"{mode} allocator benchmark failed; output preserved at "
            f"{failure_path}") from error


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


def expected_worker_payload_checksum(worker, request, expected_pairs,
                                     expected_witness_pairs):
    mixed_sizes = (16, 72, 192, 384)
    checksum = 0
    for count in (expected_witness_pairs, expected_pairs):
        for operation in range(count):
            size = request or mixed_sizes[(operation + worker) % 4]
            first = (0x5d ^ worker ^ operation) & 0xff
            last = first ^ 0xa7
            payload = (first << 8) | last
            checksum += payload ^ (size << 16)
    return checksum


def validate_concurrent_evidence(workers, witness, expected_workers=4,
                                 expected_pairs=20000,
                                 expected_witness_pairs=2048,
                                 expected_request=16):
    if len(workers) != expected_workers:
        raise ValueError("missing concurrent worker evidence")
    indices = {worker["worker"] for worker in workers}
    logical_cpus = {worker["logical_cpu"] for worker in workers}
    host_cpus = {worker["host_cpu"] for worker in workers}
    if (indices != set(range(expected_workers)) or
            len(logical_cpus) != expected_workers or
            len(host_cpus) != expected_workers):
        raise ValueError("workers do not use distinct logical and host CPUs")
    for worker in workers:
        if (worker["host_cpu"] != worker["cpu_at_start"] or
                worker["host_cpu"] != worker["cpu_at_end"] or
                worker["witness_pairs"] != expected_witness_pairs or
                worker["timed_pairs"] != expected_pairs or
                worker["checksum"] != expected_worker_payload_checksum(
                    worker["worker"], expected_request, expected_pairs,
                    expected_witness_pairs)):
            raise ValueError("worker CPU placement or operation count is invalid")
    if (witness["workers"] != expected_workers or
            witness["distinct_cpus"] != expected_workers or
            witness["witness_pairs"] != expected_workers *
                expected_witness_pairs or
            not 2 <= witness["peak_active_pairs"] <= expected_workers):
        raise ValueError("concurrent allocator overlap was not established")


def validate_concurrent_negative_controls():
    workers = [{"worker": index, "logical_cpu": index,
                "host_cpu": 8 + index, "cpu_at_start": 8 + index,
                "cpu_at_end": 8 + index, "witness_pairs": 2048,
                "timed_pairs": 20000,
                "checksum": expected_worker_payload_checksum(
                    index, 16, 20000, 2048)}
               for index in range(4)]
    witness = {"workers": 4, "distinct_cpus": 4,
               "witness_pairs": 8192, "peak_active_pairs": 2}
    validate_concurrent_evidence(workers, witness)
    controls = []
    duplicate_cpu = [dict(record) for record in workers]
    duplicate_cpu[3]["host_cpu"] = duplicate_cpu[2]["host_cpu"]
    duplicate_cpu[3]["cpu_at_start"] = duplicate_cpu[2]["cpu_at_start"]
    duplicate_cpu[3]["cpu_at_end"] = duplicate_cpu[2]["cpu_at_end"]
    controls.append((duplicate_cpu, witness))
    controls.append((workers[:-1], witness))
    wrong_count = [dict(record) for record in workers]
    wrong_count[0]["timed_pairs"] -= 1
    controls.append((wrong_count, witness))
    no_overlap = dict(witness, peak_active_pairs=1)
    controls.append((workers, no_overlap))
    wrong_checksum = [dict(record) for record in workers]
    wrong_checksum[0]["checksum"] = 0
    controls.append((wrong_checksum, witness))
    for bad_workers, bad_witness in controls:
        try:
            validate_concurrent_evidence(bad_workers, bad_witness)
        except ValueError:
            continue
        raise SystemExit("concurrent-evidence negative control was accepted")


def expected_population_payload_checksum():
    requests = (16, 32, 72, 128, 192, 256, 384, 385)
    checksum = 0
    for index in range(1024):
        request = requests[index % len(requests)]
        tag = (index * 37 + request) & 0xff
        checksum += (tag << 8) | (tag ^ 0xa7)
        if index & 1:
            replacement_tag = (tag + 53) & 0xff
            checksum += (replacement_tag << 8) | (replacement_tag ^ 0xa7)
    return checksum


def validate_population_result(mode, result):
    expected_phases = {
        "fill-live-set": (1024, 187520),
        "free-short-lived-half": (512, 84992),
        "replace-short-lived-half": (512, 187520),
        "release-all": (1024, 0),
    }
    phases = {record["phase"]: record for record in result["phases"]}
    if (set(phases) != set(expected_phases) or
            result.get("checksum") != expected_population_payload_checksum()):
        raise ValueError("population phases or payload checksum are invalid")
    for phase, (operations, live_requested) in expected_phases.items():
        record = phases[phase]
        if (record["operations"] != operations or
                record["live_requested_bytes"] != live_requested or
                record["elapsed_ns"] <= 0 or
                min(record["slab_free_slot_capacity_bytes"],
                    record["slab_cached_slot_capacity_bytes"],
                    record["slab_backing_bytes"]) < 0 or
                record["heap_used_payload_bytes"] +
                    record["heap_free_payload_bytes"] !=
                    record["heap_total_payload_bytes"] or
                record["heap_largest_free_block_bytes"] >
                    record["heap_free_payload_bytes"] or
                (record["heap_free_payload_bytes"] == 0) !=
                    (record["heap_free_block_count"] == 0) or
                record["heap_mapped_virtual_bytes"] <
                    record["heap_total_payload_bytes"] or
                record["slab_backing_bytes"] % 4096 != 0 or
                record["slab_backing_bytes"] >
                    record["heap_used_payload_bytes"] or
                record["slab_free_slot_capacity_bytes"] +
                    record["slab_cached_slot_capacity_bytes"] >
                    record["slab_backing_bytes"]):
            raise ValueError(
                "population/fragmentation accounting is inconsistent")
        if mode == "heap" and any(record[field] != 0 for field in (
                "slab_free_slot_capacity_bytes",
                "slab_cached_slot_capacity_bytes", "slab_backing_bytes")):
            raise ValueError("heap phase reported slab-owned capacity")

    before = result["resources"]["before-drain"]
    after = result["resources"]["after-drain"]
    for resources in (before, after):
        if (resources["heap_used_payload_bytes"] >
                resources["heap_total_payload_bytes"] or
                resources["heap_mapped_virtual_bytes"] <
                resources["heap_total_payload_bytes"] or
                resources["backing_allocations"] <
                resources["backing_releases"]):
            raise ValueError("population resource counters are impossible")
    classifier = result["classifier"]
    for phase, resources in (("before-drain", before), ("after-drain", after)):
        snapshot = classifier[phase]
        if (snapshot["retained_payload_bytes"] !=
                resources["classifier_retained_bytes"] or
                snapshot["retained_payload_bytes"] >
                snapshot["peak_payload_bytes"]):
            raise ValueError("classifier memory accounting is inconsistent")
    if (before["heap_mapped_virtual_bytes"] !=
            after["heap_mapped_virtual_bytes"]):
        raise ValueError("draining unexpectedly changed mapped heap extent")

    slab_state_fields = ("live_slots", "cached_slots", "partial_slabs",
                         "full_slabs", "empty_slabs")
    if mode == "heap":
        for resources in (before, after):
            if (resources["heap_used_payload_bytes"] != 0 or
                    resources["classifier_retained_bytes"] != 0 or
                    any(resources[field] != 0 for field in slab_state_fields) or
                    resources["backing_allocations"] != 0 or
                    resources["backing_releases"] != 0):
                raise ValueError("heap mode reported slab-owned memory")
        if result.get("drained_cache_objects", 0) != 0:
            raise ValueError("heap mode reported a slab cache drain")
    else:
        if before["classifier_retained_bytes"] == 0:
            raise ValueError("slab mode omitted classifier ownership")
        if before["live_slots"] != 0:
            raise ValueError("caller objects remain live after population")
        if mode == "slab":
            if (any(before[field] != 0 for field in slab_state_fields) or
                    before["backing_allocations"] !=
                    before["backing_releases"] or
                    any(after[field] != 0 for field in slab_state_fields) or
                    after["backing_allocations"] != after["backing_releases"]):
                raise ValueError("shared slab retirement is incomplete")
        elif mode == "cpu-slab":
            if (before["cached_slots"] == 0 or before["partial_slabs"] == 0 or
                    before["backing_allocations"] -
                    before["backing_releases"] != before["partial_slabs"] or
                    result.get("drained_cache_objects") !=
                    before["cached_slots"]):
                raise ValueError("per-CPU retained ownership is inconsistent")
            if (any(after[field] != 0 for field in slab_state_fields) or
                    after["backing_allocations"] !=
                    after["backing_releases"]):
                raise ValueError("per-CPU drain left slab ownership behind")
        else:
            raise ValueError("unknown population allocator mode")


def validate_population_negative_controls(result):
    validate_population_result("cpu-slab", result)
    bad = copy.deepcopy(result)
    bad["resources"]["after-drain"]["cached_slots"] = 1
    try:
        validate_population_result("cpu-slab", bad)
    except ValueError:
        pass
    else:
        raise SystemExit("population drain negative control was not rejected")
    bad = copy.deepcopy(result)
    bad["phases"][1]["slab_free_slot_capacity_bytes"] = (
        bad["phases"][1]["slab_backing_bytes"] + 1)
    try:
        validate_population_result("cpu-slab", bad)
    except ValueError:
        pass
    else:
        raise SystemExit("slab-capacity negative control was not rejected")
    bad = copy.deepcopy(result)
    bad["checksum"] ^= 1
    try:
        validate_population_result("cpu-slab", bad)
    except ValueError:
        return
    raise SystemExit("population-payload negative control was not rejected")


validate_concurrent_negative_controls()


validate_classifier_negative_control()
validate_bitmap_negative_control()


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("compiler", nargs="?", default="gcc")
parser.add_argument("--output", type=Path,
                    default=ROOT / "build/stage4e3a-slab-benchmark.json")
parser.add_argument("--assembly-output", type=Path)
parser.add_argument("--deep-validation", type=int, choices=(0, 1),
                    default=0)
parser.add_argument("--cache-capacity", type=int, choices=(2, 4, 8, 16),
                    default=2)
parser.add_argument("--refill-batch", type=int, choices=(2, 4, 8, 16),
                    default=2)
args = parser.parse_args()
output = args.output if args.output.is_absolute() else ROOT / args.output
output.parent.mkdir(parents=True, exist_ok=True)
assembly_output = args.assembly_output or output.with_suffix(".s")
if not assembly_output.is_absolute():
    assembly_output = ROOT / assembly_output
assembly_output.parent.mkdir(parents=True, exist_ok=True)
if output.exists() or assembly_output.exists():
    parser.error("refusing to overwrite benchmark evidence or assembly")
temp_root = ROOT / "build/stage4e3a-tmp"
temp_root.mkdir(parents=True, exist_ok=True)
flags = ["-std=gnu11", "-O2", "-g", "-save-temps=obj",
         "-Wall", "-Wextra", "-Werror",
         "-pthread", "-Iinclude", "-Ikernel/include",
         "-idirafter", "libc/include",
         f"-DSLAB_DEEP_VALIDATION={args.deep_validation}",
         f"-DSLAB_LOCAL_CACHE_CAPACITY={args.cache_capacity}",
         f"-DSLAB_LOCAL_REFILL_BATCH={args.refill_batch}"]
compiler_version = subprocess.run([args.compiler, "--version"], cwd=ROOT,
                                  text=True, capture_output=True,
                                  check=True).stdout.splitlines()[0]

with tempfile.TemporaryDirectory(prefix="allocator-bench-", dir=temp_root) as directory:
    binary = Path(directory) / "allocator-bench"
    assembly_command = [args.compiler, *flags, "-S",
                        "tests/memory/allocator_benchmark.c", "-o",
                        str(assembly_output)]
    subprocess.run(assembly_command, cwd=ROOT, check=True)
    command = [args.compiler, *flags, "tests/memory/allocator_benchmark.c",
               "-o", str(binary)]
    subprocess.run(command, cwd=ROOT, check=True)
    raw = {}
    for mode in ("heap", "slab", "cpu-slab"):
        run = run_benchmark(binary, mode, output)
        raw[mode] = run.stdout.splitlines()
    classifier_run = run_benchmark(binary, "classifier", output)
    bitmap_run = run_benchmark(binary, "bitmap", output)
    phase_runs = {
        mode: run_benchmark(binary, mode, output).stdout.splitlines()
        for mode in ("phases-heap", "phases-slab", "phases-cpu-slab")
    }
    population_runs = {
        mode: run_benchmark(binary, mode, output).stdout.splitlines()
        for mode in ("population-heap", "population-slab",
                     "population-cpu-slab")
    }
    lifecycle_run = run_benchmark(binary, "lifecycle", output)

sample_modes = ("heap", "slab", "cpu-slab")
samples = {mode: {16: [], 72: [], 192: [], 384: []} for mode in sample_modes}
raw_elapsed = {mode: {16: [], 72: [], 192: [], 384: []}
               for mode in sample_modes}
concurrent = {mode: {} for mode in sample_modes}
concurrent_workers = {mode: {} for mode in sample_modes}
concurrent_witnesses = {mode: {} for mode in sample_modes}
host_cpu_map = {mode: {} for mode in sample_modes}
warmups = {mode: {} for mode in sample_modes}
classes = {}
cache_configuration = None
heap_memory = {}
for mode, lines in raw.items():
    for line in lines:
        fields = line.split(",")
        if fields[0] == "cache_configuration":
            if len(fields) != 6 or mode != "cpu-slab":
                raise SystemExit("invalid per-CPU cache configuration record")
            cache_configuration = {
                "capacity_objects": int(fields[1]),
                "refill_batch_objects": int(fields[2]),
                "record_bytes": int(fields[3]),
                "cpu_records": int(fields[4]),
                "metadata_payload_bytes": int(fields[5]),
            }
        elif fields[0] == "heap_memory":
            if len(fields) != 4 or fields[1] != mode or mode in heap_memory:
                raise SystemExit("invalid or duplicate heap memory record")
            heap_memory[mode] = {
                "used_payload_bytes": int(fields[2]),
                "total_payload_bytes": int(fields[3]),
            }
        elif fields[0] == "sample":
            _, row_mode, request, _sample, pairs, elapsed = fields
            assert row_mode == mode
            request, pairs, elapsed = map(int, (request, pairs, elapsed))
            samples[mode][request].append(elapsed / pairs)
            raw_elapsed[mode][request].append(elapsed)
        elif fields[0] == "warmup":
            if len(fields) != 4 or fields[1] != mode:
                raise SystemExit("malformed allocator warm-up record")
            request, pairs = map(int, fields[2:])
            if request in warmups[mode] or pairs != 64:
                raise SystemExit("invalid or duplicate allocator warm-up")
            warmups[mode][request] = pairs
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
        elif fields[0] == "host_cpu":
            if len(fields) != 3:
                raise SystemExit("malformed host CPU record")
            worker, host_cpu = map(int, fields[1:])
            if worker in host_cpu_map[mode]:
                raise SystemExit("duplicate host CPU record")
            host_cpu_map[mode][worker] = host_cpu
        elif fields[0] == "concurrent_witness":
            if len(fields) != 6:
                raise SystemExit("malformed concurrent overlap witness")
            request, workers, peak, distinct, witness_pairs = map(int,
                                                                  fields[1:])
            if request in concurrent_witnesses[mode]:
                raise SystemExit("duplicate concurrent overlap witness")
            concurrent_witnesses[mode][request] = {
                "workers": workers, "peak_active_pairs": peak,
                "distinct_cpus": distinct, "witness_pairs": witness_pairs,
            }
        elif fields[0] == "concurrent_worker":
            if len(fields) != 10:
                raise SystemExit("malformed concurrent worker record")
            (request, worker, logical_cpu, host_cpu, cpu_start, cpu_end,
             witness_pairs, timed_pairs, checksum) = map(int, fields[1:])
            records = concurrent_workers[mode].setdefault(request, [])
            if any(record["worker"] == worker for record in records):
                raise SystemExit("duplicate concurrent worker record")
            records.append({
                "worker": worker, "logical_cpu": logical_cpu,
                "host_cpu": host_cpu, "cpu_at_start": cpu_start,
                "cpu_at_end": cpu_end, "witness_pairs": witness_pairs,
                "timed_pairs": timed_pairs, "checksum": checksum,
            })
        elif fields[0] == "concurrent_unavailable":
            if len(fields) != 2 or int(fields[1]) != 4:
                raise SystemExit("malformed unavailable-concurrency record")
            concurrent[mode]["status"] = "INCOMPLETE"
        elif fields[0] == "concurrent_class":
            (request, locks, words, backing, live, peak, partial) = \
                map(int, fields[1:])
            concurrent[mode][request].update({
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
            classes[(mode, request)] = {
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
        elif fields[0] == "cache":
            (request, cached, hits, misses, refills, refill_conflicts,
             refill, overflows, retained_pages, heap_used) = map(
                int, fields[1:])
            classes.setdefault((mode, request), {})["cache_counters"] = {
                "cached_slots": cached, "hits": hits, "misses": misses,
                "refills": refills,
                "refill_conflicts": refill_conflicts,
                "refill_objects": refill, "overflows": overflows,
                "retained_backing_pages": retained_pages,
                "heap_used_payload_bytes": heap_used,
            }
        elif fields[0] == "concurrent_cache":
            (request, cached, hits, misses, refills, refill_conflicts,
             refill_objects, overflows, retained_pages, heap_used) = map(
                 int, fields[1:])
            concurrent[mode][request].update({
                "cached_slots": cached, "cache_hits": hits,
                "cache_misses": misses, "cache_overflows": overflows,
                "cache_refills": refills,
                "cache_refill_conflicts": refill_conflicts,
                "cache_refill_objects": refill_objects,
                "retained_backing_pages": retained_pages,
                "heap_used_payload_bytes": heap_used,
            })
        else:
            raise SystemExit(f"malformed benchmark output: {line!r}")

if (not cache_configuration or
        cache_configuration["capacity_objects"] != args.cache_capacity or
        cache_configuration["refill_batch_objects"] != args.refill_batch or
        cache_configuration["metadata_payload_bytes"] !=
            cache_configuration["cpu_records"] * 20 *
            cache_configuration["record_bytes"]):
    raise SystemExit("missing or mismatched per-CPU cache configuration")
if set(heap_memory) != set(sample_modes):
    raise SystemExit("missing or incomplete final heap memory snapshots")

population = {"heap": {"phases": []}, "slab": {"phases": []},
              "cpu-slab": {"phases": []}}
for input_mode, lines in population_runs.items():
    expected_mode = input_mode.removeprefix("population-")
    for line in lines:
        fields = line.split(",")
        if fields[0] == "cache_configuration" and expected_mode == "cpu-slab":
            if (len(fields) != 6 or int(fields[1]) != args.cache_capacity or
                    int(fields[2]) != args.refill_batch):
                raise SystemExit("population per-CPU cache config mismatch")
        elif fields[0] == "population_phase" and len(fields) == 15:
            (_, mode, phase, operations, elapsed, live_requested, heap_used,
             heap_free, largest_free, free_blocks, heap_mapped,
             heap_total, slab_free_slots, slab_cached_slots,
             slab_backing) = fields
            if mode != expected_mode:
                raise SystemExit("population phase mode mismatch")
            population[mode]["phases"].append({
                "phase": phase, "operations": int(operations),
                "elapsed_ns": int(elapsed),
                "live_requested_bytes": int(live_requested),
                "heap_used_payload_bytes": int(heap_used),
                "heap_free_payload_bytes": int(heap_free),
                "heap_largest_free_block_bytes": int(largest_free),
                "heap_free_block_count": int(free_blocks),
                "heap_mapped_virtual_bytes": int(heap_mapped),
                "heap_total_payload_bytes": int(heap_total),
                "slab_free_slot_capacity_bytes": int(slab_free_slots),
                "slab_cached_slot_capacity_bytes": int(slab_cached_slots),
                "slab_backing_bytes": int(slab_backing),
            })
        elif fields[0] == "population_resources" and len(fields) == 14:
            (_, mode, phase, used, total, mapped_bytes, backing_allocations,
             backing_releases, live_slots, cached_slots, partial_slabs,
             full_slabs, empty_slabs, classifier_bytes) = fields
            if mode != expected_mode:
                raise SystemExit("invalid or duplicate population resources")
            resources = population[mode].setdefault("resources", {})
            if phase in resources:
                raise SystemExit("duplicate population resource phase")
            resources[phase] = {
                "heap_used_payload_bytes": int(used),
                "heap_total_payload_bytes": int(total),
                "heap_mapped_virtual_bytes": int(mapped_bytes),
                "backing_allocations": int(backing_allocations),
                "backing_releases": int(backing_releases),
                "live_slots": int(live_slots), "cached_slots": int(cached_slots),
                "partial_slabs": int(partial_slabs),
                "full_slabs": int(full_slabs), "empty_slabs": int(empty_slabs),
                "classifier_retained_bytes": int(classifier_bytes),
            }
        elif fields[0] == "population_class" and len(fields) == 12:
            (_, mode, phase, class_index, class_size, backing_allocations,
             backing_releases, live_slots, cached_slots, partial_slabs,
             full_slabs, empty_slabs) = fields
            if mode != expected_mode:
                raise SystemExit("population class mode mismatch")
            population_class_rows = population[mode].setdefault("classes", {}).setdefault(
                phase, {})
            class_index = int(class_index)
            if class_index in population_class_rows:
                raise SystemExit("duplicate population class record")
            population_class_rows[class_index] = {
                "class_size": int(class_size),
                "backing_allocations": int(backing_allocations),
                "backing_releases": int(backing_releases),
                "live_slots": int(live_slots),
                "cached_slots": int(cached_slots),
                "partial_slabs": int(partial_slabs),
                "full_slabs": int(full_slabs),
                "empty_slabs": int(empty_slabs),
            }
        elif fields[0] == "population_classifier" and len(fields) == 5:
            _, mode, phase, retained, peak = fields
            snapshots = population[mode].setdefault("classifier", {})
            if mode != expected_mode or phase in snapshots:
                raise SystemExit("duplicate or misidentified classifier snapshot")
            snapshots[phase] = {
                "retained_payload_bytes": int(retained),
                "peak_payload_bytes": int(peak),
            }
        elif fields[0] == "population_drain" and len(fields) == 3:
            _, mode, drained = fields
            if (mode != expected_mode or mode == "heap" or
                    "drained_cache_objects" in population[mode]):
                raise SystemExit("invalid population drain evidence")
            population[mode]["drained_cache_objects"] = int(drained)
        elif fields[0] == "population_result" and len(fields) == 4:
            _, mode, object_count, checksum = fields
            if (mode != expected_mode or int(object_count) != 1024 or
                    int(checksum) == 0 or
                    "object_count" in population[mode]):
                raise SystemExit("population ownership/payload evidence failed")
            population[mode]["object_count"] = int(object_count)
            population[mode]["checksum"] = int(checksum)
        else:
            raise SystemExit(f"malformed population output: {line!r}")
    if (len(population[expected_mode]["phases"]) != 4 or
            population[expected_mode].get("object_count") != 1024 or
            set(population[expected_mode].get("resources", {})) !=
                {"before-drain", "after-drain"}):
        raise SystemExit(f"incomplete {expected_mode} population evidence")
    if set(population[expected_mode].get("classifier", {})) != \
            {"before-drain", "after-drain"}:
        raise SystemExit("slab population lacks classifier accounting")
    population_classes = population[expected_mode].get("classes", {})
    if expected_mode == "heap":
        if population_classes:
            raise SystemExit("indexed population unexpectedly used slab classes")
    elif set(population_classes) != {"before-drain", "after-drain"} or any(
            len(rows) != 20 for rows in population_classes.values()):
        raise SystemExit("slab population lacks per-class backing accounting")
    try:
        validate_population_result(expected_mode, population[expected_mode])
    except ValueError as error:
        raise SystemExit(f"{expected_mode} population accounting failed: "
                         f"{error}") from error

validate_population_negative_controls(population["cpu-slab"])

host_affinity = sorted(os.sched_getaffinity(0)) \
    if hasattr(os, "sched_getaffinity") else []
for mode in sample_modes:
    if concurrent[mode].get("status") == "INCOMPLETE":
        continue
    if (set(host_cpu_map[mode]) != set(range(4)) or
            any(cpu not in host_affinity for cpu in host_cpu_map[mode].values())):
        raise SystemExit(f"{mode} workers were not pinned to allowed host CPUs")
    if set(concurrent[mode]) != {16, 72, 0}:
        raise SystemExit(f"{mode} concurrent workloads are incomplete")
    for request in (16, 72, 0):
        try:
            validate_concurrent_evidence(
                concurrent_workers[mode].get(request, []),
                concurrent_witnesses[mode].get(request, {}),
                expected_request=request)
        except ValueError as error:
            raise SystemExit(f"{mode} request {request}: {error}") from error
        concurrent[mode][request]["overlap_witness"] = \
            concurrent_witnesses[mode][request]
        concurrent[mode][request]["workers_evidence"] = \
            sorted(concurrent_workers[mode][request],
                   key=lambda item: item["worker"])

concurrency_status = "PASS"
if any(concurrent[mode].get("status") == "INCOMPLETE"
       for mode in sample_modes):
    concurrency_status = "INCOMPLETE"
if any(set(warmups[mode]) != {16, 72, 192, 384} or
       any(count != 64 for count in warmups[mode].values())
       for mode in sample_modes):
    raise SystemExit("steady-state benchmark warm-up evidence is incomplete")

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
    "description": "production heap, shared slab, and per-CPU slab allocator/free pairs under the same host arena and PMM/VMM adapters",
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
    "slab_deep_validation": args.deep_validation,
    "per_cpu_cache_configuration": cache_configuration,
    "final_heap_memory": heap_memory,
    "timer": "clock_gettime(CLOCK_MONOTONIC); timing brackets include timer-read overhead",
    "warmup": "each allocator/request performs and validates 64 allocation/free pairs while an anchor remains live before 128 timed batches; concurrent workers each perform and validate one pair before the separate overlap witness and timed phase",
    "warmup_pairs_per_allocator_and_request": warmups,
    "platform": platform.platform(),
    "machine": platform.machine(),
    "revision": subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                               text=True, capture_output=True,
                               check=True).stdout.strip(),
    "cpu_affinity": sorted(os.sched_getaffinity(0))
        if hasattr(os, "sched_getaffinity") else None,
    "host_multicore_validation": {
        "status": concurrency_status,
        "method": "four Linux pthread workers pinned to distinct CPUs from the process affinity mask; each worker's start/end CPU is recorded; a separate instrumented phase measures overlapping allocate/free pair brackets; timed throughput excludes overlap-counter updates",
        "workloads": {"16": "same size class", "72": "same size class",
                      "0": "mixed 16/72/192/384 byte requests, rotated by worker and operation"},
        "negative_controls": [
            "duplicate host CPU rejected", "missing worker rejected",
            "incorrect operation count rejected",
            "no overlapping allocator brackets rejected",
            "missing payload-derived checksum rejected",
        ],
    },
    "source_sha256": {
        "kernel/src/heap.c": digest(ROOT / "kernel/src/heap.c"),
        "kernel/src/slab.c": digest(ROOT / "kernel/src/slab.c"),
        "tests/memory/allocator_benchmark.c": digest(ROOT / "tests/memory/allocator_benchmark.c"),
        "tests/memory/slab_benchmark.py": digest(Path(__file__).resolve()),
        "generated_host_assembly": digest(assembly_output),
    },
    "dense_baseline": {
        "revision": DENSE_BASELINE_REVISION,
        "source_path": "kernel/src/slab.c",
        "source_sha256": hashlib.sha256(dense_source).hexdigest(),
        "lookup_contract": "historical acquire-load of one atomic class byte by heap page index",
    },
    "batch_pairs": 1000,
    "generated_host_assembly_path": str(assembly_output),
    "samples_per_size": 128,
    "units": "nanoseconds per allocation/free pair",
    "layout": {
        "page_bytes": 4096,
        "heap_block_header_bytes": 48,
        "slab_descriptor_bytes": 72,
        "slab_bitmap_bytes": 32,
        "slab_object_area_offset": 112,
        "slab_per_slot_state_and_owner_bytes": 2,
        "historical_dense_classifier_target_bytes": 134217728,
        "sparse_classifier_lookup_window_bytes": 2 * 1024 * 1024,
        "sparse_classifier_node_payload_bytes": 4112,
        "sparse_classifier_node_heap_header_bytes": 48,
    },
    "results": {},
    "raw_batch_ns": raw_elapsed,
    "concurrent_results": concurrent,
    "live_population_workload": {
        "description": "1,024 simultaneously live mixed requests at 16, 32, 72, 128, 192, 256, 384, and 385 bytes; frees and replaces alternating short-lived objects while retaining the others; 385-byte requests route to the indexed heap for both slab variants",
        "ownership_validation": "volatile first/last payload sentinels, pairwise live-range overlap checks after each phase, and an offline-reconstructed payload checksum",
        "fragmentation_validation": "after each untimed population phase, the production heap physical-block chain is traversed to reconstruct free payload, largest free block, and free-block count; the summed payload is cross-checked against heap_get_free_size and used+free against total",
        "results": population,
    },
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
bitmap_slots = {0: (4096 - 112) // 18,
                4: (4096 - 112) // 82,
                19: (4096 - 112) // 386}
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
phase_values = {mode: {operation: {16: [], 72: [], 192: [], 384: []}
                       for operation in ("alloc", "free")}
                for mode in ("heap", "slab", "cpu-slab")}
phase_operations = {}
for mode, lines in phase_runs.items():
    expected_mode = mode.split("-", 1)[1]
    for line in lines:
        fields = line.split(",")
        if fields[0] == "cache_configuration":
            if (expected_mode != "cpu-slab" or len(fields) != 6 or
                    tuple(map(int, fields[1:])) != (
                        cache_configuration["capacity_objects"],
                        cache_configuration["refill_batch_objects"],
                        cache_configuration["record_bytes"],
                        cache_configuration["cpu_records"],
                        cache_configuration["metadata_payload_bytes"])):
                raise SystemExit("separated phase cache configuration mismatch")
            continue
        if len(fields) != 7 or fields[0] != "phase":
            raise SystemExit(f"malformed separated-operation output: {line!r}")
        _, row_mode, operation, request, sample, operations, elapsed = fields
        request, sample, operations, elapsed = map(
            int, (request, sample, operations, elapsed))
        if row_mode != expected_mode or operation not in ("alloc", "free"):
            raise SystemExit("separated-operation mode mismatch")
        phase_values[row_mode][operation][request].append(elapsed / operations)
        operation_data = phase_operations.setdefault(row_mode, {}).setdefault(
            str(request), {"operations_per_sample": operations,
                           "samples": 0})
        if operation_data["operations_per_sample"] != operations:
            raise SystemExit("separated phase operation count changed")
        operation_data["samples"] += 1
for mode in ("heap", "slab", "cpu-slab"):
    for operation in ("alloc", "free"):
        for request in (16, 72, 192, 384):
            if len(phase_values[mode][operation][request]) != 64:
                raise SystemExit("incomplete separated-operation samples")

summary["separated_operations"] = {
    "method": "heap, shared-slab, and one simulated CPU-0 local-slab allocation/free batches measured independently while a high-occupancy live set and anchor remain allocated; batch is bounded by class capacity, excludes first-use backing creation, but can include local refill and overflow work",
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
                for request in (16, 72, 192, 384)
            }
            for operation in ("alloc", "free")
        }
        for mode in ("heap", "slab", "cpu-slab")
    },
    "batch_sizes_and_samples": phase_operations,
}

lifecycle_values = {request: {"alloc": [], "free": []}
                    for request in (16, 72, 192, 384)}
lifecycle_stats = {request: {"backing_allocations": 0,
                             "backing_releases": 0,
                             "empty_slab_reuses": 0,
                             "retirements": 0}
                   for request in (16, 72, 192, 384)}
lifecycle_setup_drain = None
lifecycle_final_drain = None
lifecycle_classifier = None
for line in lifecycle_run.stdout.splitlines():
    fields = line.split(",")
    if fields[0] == "lifecycle_setup_drain" and len(fields) == 2:
        lifecycle_setup_drain = int(fields[1])
        continue
    if fields[0] == "lifecycle_final_drain" and len(fields) == 2:
        lifecycle_final_drain = int(fields[1])
        continue
    if fields[0] == "lifecycle_classifier" and len(fields) == 5:
        lifecycle_classifier = list(map(int, fields[1:]))
        continue
    if fields[0] == "lifecycle_stats" and len(fields) == 8:
        (_, request, sample, backing_allocations, backing_releases,
         empty_reuses, _empty_slabs, retirements) = fields
        request, sample, backing_allocations, backing_releases, \
            empty_reuses, retirements = map(
                int, (request, sample, backing_allocations,
                      backing_releases, empty_reuses, retirements))
        if sample != len(lifecycle_values[request]["alloc"]) - 1:
            raise SystemExit("lifecycle counter sample is out of order")
        lifecycle_stats[request]["backing_allocations"] += backing_allocations
        lifecycle_stats[request]["backing_releases"] += backing_releases
        lifecycle_stats[request]["empty_slab_reuses"] += empty_reuses
        lifecycle_stats[request]["retirements"] += retirements
        continue
    if len(fields) != 5 or fields[0] != "lifecycle":
        raise SystemExit(f"malformed lifecycle output: {line!r}")
    _, request, _sample, allocation, free = fields
    request, allocation, free = map(int, (request, allocation, free))
    lifecycle_values[request]["alloc"].append(allocation)
    lifecycle_values[request]["free"].append(free)
if any(len(lifecycle_values[request][op]) != 32
       for request in lifecycle_values for op in ("alloc", "free")) or \
        lifecycle_setup_drain is None or lifecycle_final_drain is None or \
        lifecycle_classifier is None:
    raise SystemExit("incomplete refill/retirement samples")
if any(values["backing_allocations"] != 32 or
       values["backing_releases"] != 32 or
       values["empty_slab_reuses"] or values["retirements"] != 32
       for values in lifecycle_stats.values()):
    raise SystemExit("unexpected immediate-retirement lifecycle counters")
summary["refill_retirement"] = {
    "method": "one allocation from an empty class followed by freeing its last live slot; first sample includes classifier node creation, subsequent samples reuse retained classification metadata",
    "units": "nanoseconds per operation, timer overhead included",
    "setup_empty_reserves_drained": lifecycle_setup_drain,
    "final_empty_reserves_drained": lifecycle_final_drain,
    "classifier_payload_bytes_and_peak_middle_leaf": lifecycle_classifier,
    "results": {
        str(request): {
            operation: {
                "p50": statistics.median(values),
                "p95": percentile(values, 95),
                "p99": percentile(values, 99),
                "samples": values,
            }
            for operation, values in lifecycle_values[request].items()
        } | lifecycle_stats[request]
        for request in lifecycle_values
    },
}
for mode in sample_modes:
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
        if mode != "heap":
            class_data = classes[(mode, request)]
            slots = class_data["slots_per_page"]
            class_bytes = class_data["class_bytes"]
            live = class_data["live_slots_at_snapshot"]
            cached = class_data.get("cache_counters", {}).get(
                "cached_slots", 0)
            class_data.update({
                "in_page_metadata_bytes": 104 + 2 * slots,
                "object_area_bytes": 4096 - 112 - 2 * slots,
                "slot_capacity_bytes": slots * class_bytes,
                "unused_page_tail_bytes": 4096 - 112 - 2 * slots -
                    slots * class_bytes,
                "requested_bytes_at_snapshot": (live + cached) * request,
                "class_rounding_waste_at_snapshot":
                    (live + cached) * (class_bytes - request),
                "unoccupied_slots_at_snapshot": slots - live - cached,
                "unoccupied_slot_capacity_bytes":
                    (slots - live - cached) * class_bytes,
                "backing_payload_bytes_at_snapshot":
                    4096 * (class_data["partial_slabs_at_snapshot"] +
                            class_data["full_slabs_at_snapshot"]),
            })
            result.update(class_data)
        summary["results"][mode][str(request)] = result

summary["comparison"] = {}
for request in (16, 72, 192, 384):
    heap_p50 = summary["results"]["heap"][str(request)]["p50"]
    slab_p50 = summary["results"]["slab"][str(request)]["p50"]
    cpu_slab_p50 = summary["results"]["cpu-slab"][str(request)]["p50"]
    summary["comparison"][str(request)] = {
        "slab_to_heap_p50_ratio": slab_p50 / heap_p50,
        "lower_p50": "slab" if slab_p50 < heap_p50 else "heap",
        "cpu_slab_to_heap_p50_ratio": cpu_slab_p50 / heap_p50,
        "cpu_slab_to_shared_slab_p50_ratio": cpu_slab_p50 / slab_p50,
    }

output.write_text(json.dumps(summary, indent=2) + "\n")
for request in (16, 72, 192, 384):
    for mode in sample_modes:
        result = summary["results"][mode][str(request)]
        print(f"{mode} {request} bytes: p50={result['p50']:.1f} ns, "
              f"p95={result['p95']:.1f} ns, p99={result['p99']:.1f} ns/pair")
    print(f"  slab/heap p50 ratio: "
          f"{summary['comparison'][str(request)]['slab_to_heap_p50_ratio']:.3f}")
for request in (16, 72):
    for mode in sample_modes:
        if request not in concurrent[mode]:
            continue
        rate = concurrent[mode][request]["pairs_per_second"]
        print(f"{mode} {request} bytes: {rate:.0f} pinned four-CPU pairs/s")
for mode in sample_modes:
    if 0 in concurrent[mode]:
        rate = concurrent[mode][0]["pairs_per_second"]
        witness = concurrent[mode][0]["overlap_witness"]
        print(f"{mode} mixed sizes: {rate:.0f} pinned four-CPU pairs/s, "
              f"overlap={witness['peak_active_pairs']}")
print(f"raw samples and source hashes: {output}")
