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

assert analyzer.VERSION == 6
assert analyzer.slab_slots_per_page(16) == 221
assert analyzer.slab_slots_per_page(80) == 48
assert analyzer.slab_slots_per_page(288) == 13
assert analyzer.slab_slots_per_page(320) == 12
assert analyzer.slab_slots_per_page(384) == 10

def make_snapshot(version):
    data = bytearray(analyzer.expected_bytes(
        profile_version=version))
    header = [0] * analyzer.HEAP_PROFILE_FIELD_COUNT
    header[0] = analyzer.MAGIC
    header[1] = version
    header[2] = len(data)
    header[5] = 1
    header[31] = analyzer.HEAP_PROFILE_EVENT_CAPACITY
    struct.pack_into("<32Q", data, 0, *header)

    # Fill the architecture-scenario descriptor rows independently. All
    # candidate allocation counters remain zero in this clean fixture.
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
    for candidate, cutoff in enumerate(analyzer.CUTOFFS):
        at = candidate_offset + candidate * analyzer.HEAP_PROFILE_CANDIDATE_SUMMARY_FIELDS * 8
        struct.pack_into("<14Q", data, at, cutoff, len(class_tables[candidate]),
                         0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)

    if version >= 6:
        extension_at = analyzer.expected_base_bytes() + (
            analyzer.HEAP_PROFILE_EVENT_CAPACITY
            * analyzer.HEAP_PROFILE_EVENT_BYTES)
        extension_bytes = analyzer.expected_hybrid_bytes()
        extension = [0] * (extension_bytes // 8)
        hybrid_header = extension[:analyzer.HEAP_PROFILE_HYBRID_HEADER_FIELDS]
        hybrid_header[0] = analyzer.HYBRID_MAGIC
        hybrid_header[1] = 2
        hybrid_header[2] = extension_bytes
        hybrid_header[4] = 1
        hybrid_header[5] = 1
        hybrid_header[6] = 1
        hybrid_header[14] = (analyzer.HEAP_PROFILE_EVENT_CAPACITY
                             * analyzer.HEAP_PROFILE_EVENT_BYTES)
        hybrid_header[15] = extension_bytes
        hybrid_header[16] = analyzer.HEAP_PROFILE_HYBRID_CLASS_COUNT
        hybrid_header[17] = ((hybrid_header[14] + 15) & ~15) + (
            (extension_bytes + 15) & ~15)
        route_at = analyzer.HEAP_PROFILE_HYBRID_HEADER_FIELDS
        classifier_at = route_at + analyzer.HEAP_PROFILE_HYBRID_ROUTE_FIELDS
        class_at = classifier_at + analyzer.HEAP_PROFILE_HYBRID_CLASSIFIER_FIELDS
        extension[route_at + 0] = 2
        extension[route_at + 1] = 1
        extension[route_at + 3] = 1
        extension[route_at + 5] = 1
        extension[route_at + 6] = 1
        extension[route_at + 7] = 2
        extension[route_at + 10] = 2
        extension[route_at + 13] = 1
        extension[route_at + 15] = 2
        extension[route_at + 17] = 1
        extension[route_at + 18] = 1
        extension[route_at + 19] = 2
        expected_sizes = [16 * index for index in range(1, 17)] + [
            288, 320, 352, 384]
        for index, size in enumerate(expected_sizes):
            extension[class_at + index * analyzer.HEAP_PROFILE_HYBRID_CLASS_FIELDS] = size
        extension[:analyzer.HEAP_PROFILE_HYBRID_HEADER_FIELDS] = hybrid_header
        struct.pack_into("<" + "Q" * len(extension), data, extension_at,
                         *extension)
    return data


data = make_snapshot(6)
summary_offset = (
    analyzer.HEAP_PROFILE_FIELD_COUNT * 8
    + analyzer.HEAP_PROFILE_SIZE_SLOTS * analyzer.HEAP_PROFILE_SIZE_FIELDS * 8
    + analyzer.HEAP_PROFILE_RANGE_COUNT * analyzer.HEAP_PROFILE_RANGE_FIELDS * 8
    + analyzer.HEAP_PROFILE_PHASES * analyzer.HEAP_PROFILE_SIZE_SLOTS
      * analyzer.HEAP_PROFILE_PHASE_FIELDS * 8
    + analyzer.HEAP_PROFILE_LOG_BUCKETS * analyzer.HEAP_PROFILE_LIFETIME_BUCKETS * 8
    + analyzer.HEAP_PROFILE_CPU_SLOTS * 3 * 8
    + analyzer.HEAP_PROFILE_CANDIDATES
      * analyzer.HEAP_PROFILE_MAX_CLASSES
      * analyzer.HEAP_PROFILE_CANDIDATE_FIELDS * 8
    + analyzer.HEAP_PROFILE_PHASES * analyzer.HEAP_PROFILE_CANDIDATES
      * analyzer.HEAP_PROFILE_MAX_CLASSES
      * analyzer.HEAP_PROFILE_PHASE_FIELDS * 8
)

summary = analyzer.parse_snapshot(data, expected_phase=1)
assert summary["status"] == "PASS"
assert summary["event_trace"]["events"] == 0
assert summary["event_trace_records"] == []
assert [item["cutoff"] for item in summary["slab_candidates"]] == list(analyzer.CUTOFFS)
assert summary["hybrid_runtime"]["status"] == "PASS"
assert summary["hybrid_runtime"]["online_cpus"] == 1
assert summary["hybrid_runtime"]["route_counters"][
    "reclaim_cached_objects"] == 2
assert summary["hybrid_runtime"]["route_counters"][
    "slab_backing_allocation_failures"] == 2

# The first diagnostic captures used extension version 1 (19 route words).
# Keep those bounded artifacts parseable after adding the backing-failure
# counter in version 2.
legacy_runtime = bytearray(data)
legacy_extension_at = analyzer.expected_base_bytes() + (
    analyzer.HEAP_PROFILE_EVENT_CAPACITY * analyzer.HEAP_PROFILE_EVENT_BYTES)
legacy_route_end = legacy_extension_at + (
    analyzer.HEAP_PROFILE_HYBRID_HEADER_FIELDS +
    analyzer.HEAP_PROFILE_HYBRID_ROUTE_FIELDS) * 8
legacy_runtime = (legacy_runtime[:legacy_route_end - 8] +
                  legacy_runtime[legacy_route_end:])
legacy_extension_bytes = analyzer.expected_hybrid_bytes(
    analyzer.HEAP_PROFILE_HYBRID_ROUTE_FIELDS_V1)
struct.pack_into("<Q", legacy_runtime, 16, len(legacy_runtime))
struct.pack_into("<Q", legacy_runtime, legacy_extension_at + 8, 1)
struct.pack_into("<Q", legacy_runtime, legacy_extension_at + 16,
                 legacy_extension_bytes)
struct.pack_into("<Q", legacy_runtime, legacy_extension_at + 15 * 8,
                 legacy_extension_bytes)
event_bytes = analyzer.HEAP_PROFILE_EVENT_CAPACITY * analyzer.HEAP_PROFILE_EVENT_BYTES
struct.pack_into("<Q", legacy_runtime, legacy_extension_at + 17 * 8,
                 ((event_bytes + 15) & ~15) +
                 ((legacy_extension_bytes + 15) & ~15))
legacy_summary = analyzer.parse_snapshot(legacy_runtime, expected_phase=1)
assert legacy_summary["hybrid_runtime"]["extension_version"] == 1
assert "slab_backing_allocation_failures" not in legacy_summary[
    "hybrid_runtime"]["route_counters"]

legacy = make_snapshot(5)
legacy_summary = analyzer.parse_snapshot(legacy, expected_phase=1)
assert legacy_summary["format"] == "PITH-HEAP-PROFILE-5"
assert "hybrid_runtime" not in legacy_summary

def make_alloc_live_mismatch(copy):
    struct.pack_into("<Q", copy, 6 * 8, 1)  # one committed allocation
    struct.pack_into("<Q", copy, 7 * 8, 1)  # one attempt
    struct.pack_into("<Q", copy, 8 * 8, 1)  # one success
    size_row = analyzer.HEAP_PROFILE_FIELD_COUNT * 8 + 16 * analyzer.HEAP_PROFILE_SIZE_FIELDS * 8
    struct.pack_into("<Q", copy, size_row, 1)
    struct.pack_into("<Q", copy, size_row + 8, 1)
    struct.pack_into("<Q", copy, (analyzer.HEAP_PROFILE_FIELD_COUNT - 11) * 8, 1)
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
     "event trace count"),
    (lambda copy: struct.pack_into("<Q", copy, 21 * 8, 1),
     "event trace allocated contexts"),
    (lambda copy: struct.pack_into("<Q", copy, summary_offset + 2 * 8, 1),
     "candidate routed count"),
    (lambda copy: struct.pack_into("<Q", copy, 31 * 8, 1),
     "event trace bounds"),
    (lambda copy: struct.pack_into("<Q", copy, 30 * 8, 1),
     "event kind"),
):
    corrupt = bytearray(data)
    mutate(corrupt)
    try:
        analyzer.parse_snapshot(corrupt)
    except ValueError as error:
        assert expected in str(error).lower(), f"{expected}: {error}"
    else:
        raise AssertionError(f"negative control accepted: {expected}")

hybrid_extension_at = analyzer.expected_base_bytes() + (
    analyzer.HEAP_PROFILE_EVENT_CAPACITY * analyzer.HEAP_PROFILE_EVENT_BYTES)
hybrid_negative_controls = (
    (lambda copy: copy.__delitem__(slice(-1, None)), "length"),
    (lambda copy: struct.pack_into("<Q", copy, hybrid_extension_at + 3 * 8, 1),
     "incomplete"),
    (lambda copy: struct.pack_into("<Q", copy, hybrid_extension_at + 4 * 8, 2),
     "header"),
    (lambda copy: struct.pack_into("<Q", copy, hybrid_extension_at + 18 * 8, 2),
     "header"),
    (lambda copy: struct.pack_into("<Q", copy, hybrid_extension_at + 8, 9),
     "length"),
    (lambda copy: struct.pack_into("<Q", copy, hybrid_extension_at +
                                   analyzer.HEAP_PROFILE_HYBRID_HEADER_FIELDS * 8, 1),
     "routing counters"),
    (lambda copy: struct.pack_into("<Q", copy, hybrid_extension_at +
                                   (analyzer.HEAP_PROFILE_HYBRID_HEADER_FIELDS + 14) * 8, 2),
     "routing counters"),
)
for mutate, expected in hybrid_negative_controls:
    corrupt = bytearray(data)
    mutate(corrupt)
    try:
        analyzer.parse_snapshot(corrupt)
    except ValueError as error:
        assert expected in str(error).lower(), f"{expected}: {error}"
    else:
        raise AssertionError(f"hybrid negative control accepted: {expected}")

print("Heap-profile format completeness and corruption controls: PASS")
