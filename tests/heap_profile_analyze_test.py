#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Negative controls for the guest heap-profile format validator."""
import importlib.util
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "heap_profile_analyze", ROOT / "tests/heap_profile_analyze.py")
analyzer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analyzer)

data = bytearray(analyzer.expected_bytes())
header = [0] * analyzer.HEAP_PROFILE_FIELD_COUNT
header[0] = analyzer.MAGIC
header[1] = analyzer.VERSION
header[2] = len(data)
header[5] = 1
struct.pack_into("<32Q", data, 0, *header)

# Fill the architecture-scenario descriptor rows independently. All candidate
# allocation counters remain zero in this clean, empty negative-control image.
candidate_offset = (
    analyzer.HEAP_PROFILE_FIELD_COUNT * 8
    + analyzer.HEAP_PROFILE_SIZE_SLOTS * analyzer.HEAP_PROFILE_SIZE_FIELDS * 8
    + analyzer.HEAP_PROFILE_RANGE_COUNT * analyzer.HEAP_PROFILE_RANGE_FIELDS * 8
    + analyzer.HEAP_PROFILE_PHASES * analyzer.HEAP_PROFILE_SIZE_SLOTS
      * analyzer.HEAP_PROFILE_PHASE_FIELDS * 8
    + analyzer.HEAP_PROFILE_LOG_BUCKETS * analyzer.HEAP_PROFILE_LIFETIME_BUCKETS * 8
    + analyzer.HEAP_PROFILE_CPU_SLOTS * 3 * 8
)
class_tables = []
for candidate in range(analyzer.HEAP_PROFILE_CANDIDATES):
    classes = analyzer.candidate_classes(candidate)
    class_tables.append(classes)
    for index, size in enumerate(classes):
        at = candidate_offset + ((candidate * analyzer.HEAP_PROFILE_MAX_CLASSES + index)
                                 * analyzer.HEAP_PROFILE_CANDIDATE_FIELDS * 8)
        struct.pack_into("<Q", data, at, size)
candidate_offset += (analyzer.HEAP_PROFILE_CANDIDATES
                     * analyzer.HEAP_PROFILE_MAX_CLASSES
                     * analyzer.HEAP_PROFILE_CANDIDATE_FIELDS * 8)
candidate_offset += (analyzer.HEAP_PROFILE_PHASES
                     * analyzer.HEAP_PROFILE_CANDIDATES
                     * analyzer.HEAP_PROFILE_MAX_CLASSES
                     * analyzer.HEAP_PROFILE_PHASE_FIELDS * 8)
summary_offset = candidate_offset
for candidate, cutoff in enumerate(analyzer.CUTOFFS):
    at = candidate_offset + candidate * analyzer.HEAP_PROFILE_CANDIDATE_SUMMARY_FIELDS * 8
    struct.pack_into("<14Q", data, at, cutoff, len(class_tables[candidate]),
                     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)

summary = analyzer.parse_snapshot(data, expected_phase=1)
assert summary["status"] == "PASS"
assert [item["cutoff"] for item in summary["slab_candidates"]] == list(analyzer.CUTOFFS)

def make_alloc_live_mismatch(copy):
    struct.pack_into("<Q", copy, 6 * 8, 1)  # one committed allocation
    struct.pack_into("<Q", copy, 7 * 8, 1)  # one attempt
    struct.pack_into("<Q", copy, 8 * 8, 1)  # one success
    size_row = analyzer.HEAP_PROFILE_FIELD_COUNT * 8 + 16 * analyzer.HEAP_PROFILE_SIZE_FIELDS * 8
    struct.pack_into("<Q", copy, size_row, 1)
    struct.pack_into("<Q", copy, size_row + 8, 1)
    cpu_offset = (analyzer.HEAP_PROFILE_FIELD_COUNT * 8
                  + analyzer.HEAP_PROFILE_SIZE_SLOTS * analyzer.HEAP_PROFILE_SIZE_FIELDS * 8
                  + analyzer.HEAP_PROFILE_RANGE_COUNT * analyzer.HEAP_PROFILE_RANGE_FIELDS * 8
                  + analyzer.HEAP_PROFILE_PHASES * analyzer.HEAP_PROFILE_SIZE_SLOTS * analyzer.HEAP_PROFILE_PHASE_FIELDS * 8
                  + analyzer.HEAP_PROFILE_LOG_BUCKETS * analyzer.HEAP_PROFILE_LIFETIME_BUCKETS * 8)
    struct.pack_into("<Q", copy, cpu_offset, 1)

for mutate, expected in (
    (lambda copy: copy.__delitem__(slice(-1, None)), "length"),
    (lambda copy: struct.pack_into("<Q", copy, 24, 1), "incomplete"),
    (lambda copy: struct.pack_into("<Q", copy, 18 * 8, 1), "in flight"),
    (lambda copy: struct.pack_into("<Q", copy, 7 * 8, 1), "outcomes"),
    (lambda copy: struct.pack_into("<Q", copy, 5 * 8, analyzer.HEAP_PROFILE_PHASES),
     "phase index"),
    (make_alloc_live_mismatch,
     "allocations, frees, and live"),
    (lambda copy: struct.pack_into("<Q", copy, summary_offset + 2 * 8, 1),
     "candidate routed count"),
):
    corrupt = bytearray(data)
    mutate(corrupt)
    try:
        analyzer.parse_snapshot(corrupt)
    except ValueError as error:
        assert expected in str(error).lower(), str(error)
    else:
        raise AssertionError(f"negative control accepted: {expected}")

print("Heap-profile format completeness and corruption controls: PASS")
