#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate and summarize a fixed-format Pith heap-profile snapshot."""
import json
import hashlib
import math
import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True
HEAP_PROFILE_CANDIDATES = 5
HEAP_PROFILE_CANDIDATE_FIELDS = 10
HEAP_PROFILE_CANDIDATE_SUMMARY_FIELDS = 14
HEAP_PROFILE_CANDIDATE_PHASE_FIELDS = 5
HEAP_PROFILE_RANGE_COUNT = 7
HEAP_PROFILE_RANGE_FIELDS = 4
HEAP_PROFILE_CPU_SLOTS = 257
HEAP_PROFILE_EXACT_MAX = 1024
HEAP_PROFILE_FIELD_COUNT = 32
HEAP_PROFILE_LIFETIME_BUCKETS = 32
HEAP_PROFILE_LOG_BUCKETS = 64
HEAP_PROFILE_MAX_CLASSES = 32
HEAP_PROFILE_PHASES = 4
HEAP_PROFILE_PHASE_FIELDS = 8
HEAP_PROFILE_SIZE_FIELDS = 13
HEAP_PROFILE_SIZE_SLOTS = HEAP_PROFILE_EXACT_MAX + 1 + HEAP_PROFILE_LOG_BUCKETS
HEAP_PROFILE_EVENT_CAPACITY = 65536
HEAP_PROFILE_EVENT_BYTES = 24
SLAB_PAGE_BYTES = 4096
SLAB_OBJECT_AREA_OFFSET = 112
SLAB_SLOT_METADATA_BYTES = 2
MAGIC = 0x4850524F46303131
VERSION = 4

HEADER = (
    "magic version total_bytes flags cpu_count final_phase operations attempts "
    "successes failures frees successful_requested_bytes failed_requested_bytes "
    "cross_cpu_frees tracked_live tracked_peak tracking_misses max_probe "
    "inflight_at_freeze lifetime_samples lifetime_operation_sum "
    "alloc_context_normal alloc_context_irq_disabled "
    "alloc_context_spinlock_held fail_context_normal "
    "fail_context_irq_disabled fail_context_spinlock_held "
    "free_context_normal free_context_irq_disabled "
    "free_context_spinlock_held trace_event_count trace_event_capacity"
).split()
CONTEXT_NAMES = ("normal", "irq_disabled", "spinlock_held")
SIZE_FIELD_NAMES = (
    "attempts successes failures frees successful_requested_bytes "
    "failed_requested_bytes current_live current_requested_bytes peak_live "
    "peak_requested_bytes lifetime_samples lifetime_operation_sum cross_cpu_frees"
).split()
RANGE_NAMES = ("1-16", "17-64", "65-256", "257-512", "513-1024",
               ">1024", "zero-size-failures")
RANGE_FIELD_NAMES = ("current_live", "current_requested_bytes",
                     "peak_live", "peak_requested_bytes")
PHASE_FIELD_NAMES = (
    "allocations frees peak_live peak_requested_bytes live_at_end "
    "requested_bytes_at_end survivors_into_next_phase survivor_bytes"
).split()
CLASS_FIELD_NAMES = (
    "class_size allocations frees requested_bytes rounding_waste current_live "
    "current_requested_bytes peak_live peak_requested_bytes"
).split()
PHASE_NAMES = ("boot_and_service_start", "native_workload", "phase_2", "phase_3")
CUTOFFS = (256, 384, 512, 768, 1024)
CLASS_SUMMARY_NAMES = (
    "cutoff", "class_count", "slab_routed_successes",
    "heap_bypassed_successes", "heap_bypassed_requested_bytes",
    "current_live_objects", "current_live_requested_bytes",
    "peak_live_objects", "peak_live_requested_bytes", "current_slab_pages",
    "peak_slab_pages", "current_slot_capacity", "peak_slot_capacity",
    "peak_unused_slots",
)
CLASS_PHASE_NAMES = ("peak_live_objects", "peak_live_requested_bytes",
                     "peak_slab_pages", "peak_slot_capacity",
                     "peak_unused_slots")


def expected_base_bytes():
    words = (
        HEAP_PROFILE_FIELD_COUNT
        + HEAP_PROFILE_SIZE_SLOTS * HEAP_PROFILE_SIZE_FIELDS
        + HEAP_PROFILE_RANGE_COUNT * HEAP_PROFILE_RANGE_FIELDS
        + HEAP_PROFILE_PHASES * HEAP_PROFILE_SIZE_SLOTS * HEAP_PROFILE_PHASE_FIELDS
        + HEAP_PROFILE_LOG_BUCKETS * HEAP_PROFILE_LIFETIME_BUCKETS
        + HEAP_PROFILE_CPU_SLOTS * 3
        + HEAP_PROFILE_CANDIDATES * HEAP_PROFILE_MAX_CLASSES * HEAP_PROFILE_CANDIDATE_FIELDS
        + HEAP_PROFILE_PHASES * HEAP_PROFILE_CANDIDATES * HEAP_PROFILE_MAX_CLASSES * HEAP_PROFILE_PHASE_FIELDS
        + HEAP_PROFILE_CANDIDATES * HEAP_PROFILE_CANDIDATE_SUMMARY_FIELDS
        + HEAP_PROFILE_PHASES * HEAP_PROFILE_CANDIDATES * HEAP_PROFILE_CANDIDATE_PHASE_FIELDS
        + HEAP_PROFILE_CANDIDATES * HEAP_PROFILE_MAX_CLASSES * 4
    )
    return words * 8


def expected_bytes(event_capacity=HEAP_PROFILE_EVENT_CAPACITY):
    return expected_base_bytes() + event_capacity * HEAP_PROFILE_EVENT_BYTES


def words(data, offset, count):
    return struct.unpack_from("<" + "Q" * count, data, offset)


def rows(data, offset, outer, inner, fields):
    out = []
    for i in range(outer):
        line = []
        for j in range(inner):
            line.append(words(data, offset, fields))
            offset += fields * 8
        out.append(line)
    return out, offset


def exact_slot(size):
    return size if size <= HEAP_PROFILE_EXACT_MAX else None


def log_slot(size):
    return HEAP_PROFILE_EXACT_MAX + 1 + (size.bit_length() - 1)


def candidate_classes(index):
    count = (16, 20, 24, 28, 32)[index]
    result = [16 * (i + 1) for i in range(16)]
    result += [288 + 32 * i for i in range(8)]
    result += [576 + 64 * i for i in range(4)]
    result += [832 + 64 * i for i in range(4)]
    return result[:count]


def candidate_class_index(requested, cutoff):
    normalized = (requested + 15) & ~15
    if normalized > cutoff:
        return None
    classes = candidate_classes(CUTOFFS.index(cutoff))
    for index, size in enumerate(classes):
        if normalized <= size:
            return index
    return None


def slab_slots_per_page(class_size):
    """Match the current production slab object and per-slot layout."""
    if class_size <= 0:
        raise ValueError("slab class size must be positive")
    return ((SLAB_PAGE_BYTES - SLAB_OBJECT_AREA_OFFSET) //
            (class_size + SLAB_SLOT_METADATA_BYTES))


def parse_snapshot(data, expected_phase=None):
    if len(data) < expected_base_bytes():
        raise ValueError("profile is shorter than its fixed accounting section")
    offset = 0
    header_words = words(data, offset, HEAP_PROFILE_FIELD_COUNT)
    header = dict(zip(HEADER, header_words))
    offset += HEAP_PROFILE_FIELD_COUNT * 8
    if any(header_words[len(HEADER):]):
        raise ValueError("reserved header fields are nonzero")
    if header["magic"] != MAGIC or header["version"] != VERSION:
        raise ValueError("profile magic or version mismatch")
    event_capacity = header["trace_event_capacity"]
    if event_capacity > HEAP_PROFILE_EVENT_CAPACITY or \
       len(data) != expected_bytes(event_capacity):
        raise ValueError("event trace bounds or profile length is invalid")
    if header["total_bytes"] != len(data):
        raise ValueError("profile self-reported length mismatch")
    if header["final_phase"] >= HEAP_PROFILE_PHASES:
        raise ValueError("profile phase index is out of range")
    if header["cpu_count"] > HEAP_PROFILE_CPU_SLOTS - 1:
        raise ValueError("profile CPU count exceeds fixed table")
    if header["flags"]:
        raise ValueError(f"profile marked incomplete (flags=0x{header['flags']:x})")
    if header["inflight_at_freeze"]:
        raise ValueError("profile stopped while kmalloc operations were in flight")
    if expected_phase is not None and header["final_phase"] != expected_phase:
        raise ValueError("unexpected final profile phase")

    size_rows = []
    for _ in range(HEAP_PROFILE_SIZE_SLOTS):
        size_rows.append(words(data, offset, HEAP_PROFILE_SIZE_FIELDS))
        offset += HEAP_PROFILE_SIZE_FIELDS * 8
    range_rows = []
    for _ in range(HEAP_PROFILE_RANGE_COUNT):
        range_rows.append(words(data, offset, HEAP_PROFILE_RANGE_FIELDS))
        offset += HEAP_PROFILE_RANGE_FIELDS * 8
    phase_rows = []
    for _ in range(HEAP_PROFILE_PHASES):
        phase = []
        for _ in range(HEAP_PROFILE_SIZE_SLOTS):
            phase.append(words(data, offset, HEAP_PROFILE_PHASE_FIELDS))
            offset += HEAP_PROFILE_PHASE_FIELDS * 8
        phase_rows.append(phase)
    lifetime = []
    for _ in range(HEAP_PROFILE_LOG_BUCKETS):
        lifetime.append(words(data, offset, HEAP_PROFILE_LIFETIME_BUCKETS))
        offset += HEAP_PROFILE_LIFETIME_BUCKETS * 8
    cpu = []
    for _ in range(HEAP_PROFILE_CPU_SLOTS):
        cpu.append(words(data, offset, 3))
        offset += 3 * 8

    candidates = []
    for _ in range(HEAP_PROFILE_CANDIDATES):
        candidate = []
        for _ in range(HEAP_PROFILE_MAX_CLASSES):
            candidate.append(words(data, offset, HEAP_PROFILE_CANDIDATE_FIELDS))
            offset += HEAP_PROFILE_CANDIDATE_FIELDS * 8
        candidates.append(candidate)
    candidate_phases = []
    for _ in range(HEAP_PROFILE_PHASES):
        phase = []
        for _ in range(HEAP_PROFILE_CANDIDATES):
            candidate = []
            for _ in range(HEAP_PROFILE_MAX_CLASSES):
                candidate.append(words(data, offset, HEAP_PROFILE_PHASE_FIELDS))
                offset += HEAP_PROFILE_PHASE_FIELDS * 8
            phase.append(candidate)
        candidate_phases.append(phase)
    candidate_summary = []
    for _ in range(HEAP_PROFILE_CANDIDATES):
        candidate_summary.append(words(data, offset,
                                       HEAP_PROFILE_CANDIDATE_SUMMARY_FIELDS))
        offset += HEAP_PROFILE_CANDIDATE_SUMMARY_FIELDS * 8
    candidate_phase_summary = []
    for _ in range(HEAP_PROFILE_PHASES):
        candidate_phase_summary.append(words(
            data, offset,
            HEAP_PROFILE_CANDIDATES * HEAP_PROFILE_CANDIDATE_PHASE_FIELDS))
        offset += (HEAP_PROFILE_CANDIDATES *
                   HEAP_PROFILE_CANDIDATE_PHASE_FIELDS * 8)
    candidate_cpu_masks = []
    for _ in range(HEAP_PROFILE_CANDIDATES):
        candidate = []
        for _ in range(HEAP_PROFILE_MAX_CLASSES):
            candidate.append(words(data, offset, 4))
            offset += 4 * 8
        candidate_cpu_masks.append(candidate)
    event_count = header["trace_event_count"]
    if event_count > event_capacity:
        raise ValueError("profile event trace bounds are invalid")
    event_payload = data[offset:offset + event_count * HEAP_PROFILE_EVENT_BYTES]
    event_trace = []
    event_counts = {"allocated": 0, "freed": 0, "failed": 0}
    event_contexts = {name: [0] * len(CONTEXT_NAMES)
                      for name in event_counts}
    event_sizes = {}
    live_ids = {}
    cross_cpu = 0
    for index in range(event_count):
        object_id, reserved0, requested, cpu_id, kind, context, phase, \
            reserved1, reserved2 = struct.unpack_from(
                "<IIQHBBBBH", data,
                offset + index * HEAP_PROFILE_EVENT_BYTES)
        if reserved0 or reserved1 or reserved2 or \
           context >= len(CONTEXT_NAMES) or phase > header["final_phase"] or \
           cpu_id >= HEAP_PROFILE_CPU_SLOTS:
            raise ValueError("profile event contains invalid metadata")
        if kind == 1:
            name = "allocated"
            if not requested or object_id != index + 1 or object_id in live_ids:
                raise ValueError("profile allocation event identity is invalid")
            live_ids[object_id] = (requested, cpu_id)
        elif kind == 2:
            name = "freed"
            owner = live_ids.pop(object_id, None)
            if not object_id or owner is None or owner[0] != requested:
                raise ValueError("profile free event has no matching allocation")
            if owner[1] != cpu_id:
                cross_cpu += 1
        elif kind == 3:
            name = "failed"
            if object_id:
                raise ValueError("failed allocation has an object identity")
        else:
            raise ValueError("profile event kind is invalid")
        event_counts[name] += 1
        event_contexts[name][context] += 1
        event_sizes.setdefault(requested, [0, 0, 0])[
            {"allocated": 0, "freed": 1, "failed": 2}[name]] += 1
        event_trace.append({
            "sequence": index + 1, "event": name, "object_id": object_id,
            "requested_bytes": requested, "cpu": cpu_id,
            "context": CONTEXT_NAMES[context], "phase": phase,
        })
    tail_offset = offset + event_count * HEAP_PROFILE_EVENT_BYTES
    if any(data[tail_offset:]):
        raise ValueError("unused profile event slots are not zero")
    offset = len(data)
    if offset != len(data):
        raise ValueError("profile parser did not consume exact snapshot")

    if event_count != header["successes"] + header["frees"] + \
       header["failures"]:
        raise ValueError("event trace count does not match allocator outcomes")
    if event_counts != {"allocated": header["successes"],
                        "freed": header["frees"],
                        "failed": header["failures"]}:
        raise ValueError("event trace outcomes disagree with profile totals")
    if event_counts["allocated"] - event_counts["freed"] != \
       len(live_ids) or len(live_ids) != header["tracked_live"]:
        raise ValueError("event trace live objects disagree with profile")
    if cross_cpu != header["cross_cpu_frees"]:
        raise ValueError("event trace cross-CPU totals disagree with profile")
    for name, field_prefix in (("allocated", "alloc_context_"),
                               ("freed", "free_context_"),
                               ("failed", "fail_context_")):
        if event_contexts[name] != [header[field_prefix + context]
                                    for context in CONTEXT_NAMES]:
            raise ValueError(f"event trace {name} contexts disagree with profile")
    event_size_buckets = {}
    for size, counts in event_sizes.items():
        slot = size if size <= HEAP_PROFILE_EXACT_MAX else \
            HEAP_PROFILE_EXACT_MAX + 1 + (size.bit_length() - 1)
        bucket = event_size_buckets.setdefault(slot, [0, 0, 0])
        for index, count in enumerate(counts):
            bucket[index] += count
    for slot, row in enumerate(size_rows):
        counts = event_size_buckets.get(slot, [0, 0, 0])
        if (row[1], row[3], row[2]) != tuple(counts):
            raise ValueError(f"event trace size bucket {slot} totals "
                             f"{counts} disagree with profile "
                             f"{row[1], row[3], row[2]}")

    if header["attempts"] != header["successes"] + header["failures"]:
        raise ValueError("request outcomes do not account for every attempt")
    if sum(row[0] for row in size_rows) != header["attempts"]:
        raise ValueError("size histogram attempt total mismatch")
    if header["successes"] != sum(row[1] for row in size_rows):
        raise ValueError("size histogram success total mismatch")
    if header["failures"] != sum(row[2] for row in size_rows):
        raise ValueError("size histogram failure total mismatch")
    if sum(header[f"alloc_context_{name}"] for name in CONTEXT_NAMES) != \
       header["successes"]:
        raise ValueError("allocation context totals do not match successes")
    if sum(header[f"fail_context_{name}"] for name in CONTEXT_NAMES) != \
       header["failures"]:
        raise ValueError("failure context totals do not match failures")
    if sum(header[f"free_context_{name}"] for name in CONTEXT_NAMES) != \
       header["frees"]:
        raise ValueError("free context totals do not match frees")
    if header["frees"] != sum(row[3] for row in size_rows):
        raise ValueError("size histogram free total mismatch")
    if header["cross_cpu_frees"] != sum(row[12] for row in size_rows):
        raise ValueError("cross-CPU free total mismatch")
    if header["successful_requested_bytes"] != sum(row[4] for row in size_rows):
        raise ValueError("successful requested-byte total mismatch")
    if header["failed_requested_bytes"] != sum(row[5] for row in size_rows):
        raise ValueError("failed requested-byte total mismatch")
    if header["lifetime_samples"] != sum(row[10] for row in size_rows):
        raise ValueError("lifetime sample total mismatch")
    if header["lifetime_operation_sum"] != sum(row[11] for row in size_rows):
        raise ValueError("lifetime operation total mismatch")
    if header["successes"] < header["frees"] or \
       header["successes"] - header["frees"] != header["tracked_live"]:
        raise ValueError("successful allocations, frees, and live objects disagree")
    if header["lifetime_samples"] != header["frees"]:
        raise ValueError("completed lifetime count does not match frees")
    for size, row in enumerate(size_rows):
        if row[1] < row[3] or row[1] - row[3] != row[6]:
            raise ValueError(f"size {size} allocation/free/live totals disagree")
    if header["operations"] != header["successes"] + header["frees"]:
        raise ValueError("operation sequence does not match allocator commits")
    if sum(row[0] for row in cpu) != header["successes"] or \
       sum(row[1] for row in cpu) != header["frees"] or \
       sum(row[2] for row in cpu) != header["cross_cpu_frees"]:
        raise ValueError("per-CPU totals disagree with global counters")
    if header["tracked_live"] != sum(row[6] for row in size_rows):
        raise ValueError("live table and exact-size counters disagree")
    for size in range(HEAP_PROFILE_SIZE_SLOTS):
        expected_live = 0
        for phase in range(header["final_phase"] + 1):
            row = phase_rows[phase][size]
            if row[1] > expected_live + row[0] or \
               row[4] != expected_live + row[0] - row[1]:
                raise ValueError(
                    f"phase {phase} size {size} live-object accounting disagrees")
            expected_live = row[4]
        if expected_live != size_rows[size][6]:
            raise ValueError(f"phase and size {size} live totals disagree")
    if sum(phase_rows[p][size][0]
           for p in range(header["final_phase"] + 1)
           for size in range(HEAP_PROFILE_SIZE_SLOTS)) != header["successes"] or \
       sum(phase_rows[p][size][1]
           for p in range(header["final_phase"] + 1)
           for size in range(HEAP_PROFILE_SIZE_SLOTS)) != header["frees"]:
        raise ValueError("phase allocation/free totals disagree with global totals")

    range_bounds = ((1, 16), (17, 64), (65, 256), (257, 512),
                    (513, 1024), (1025, len(size_rows) - 1))
    for index, (lower, upper) in enumerate(range_bounds):
        current = sum(size_rows[size][6] for size in range(lower, upper + 1))
        current_bytes = sum(size_rows[size][7]
                            for size in range(lower, upper + 1))
        if range_rows[index][0] != current or range_rows[index][1] != current_bytes:
            raise ValueError("range live totals disagree with exact-size totals")
        if range_rows[index][2] < max(
                (size_rows[size][8] for size in range(lower, upper + 1)),
                default=0):
            raise ValueError("range peak is below an exact-size peak")
        if range_rows[index][3] < max(
                (size_rows[size][9] for size in range(lower, upper + 1)),
                default=0):
            raise ValueError("range byte peak is below an exact-size peak")
    if any(range_rows[6]):
        raise ValueError("zero-size allocation cannot be live")

    ranges = []
    for range_index, (lower, upper, name) in enumerate((
            (1, 16, "1-16"), (17, 64, "17-64"), (65, 256, "65-256"),
            (257, 512, "257-512"), (513, 1024, "513-1024"))):
        counts = sum(size_rows[size][1] for size in range(lower, upper + 1))
        bytes_ = sum(size_rows[size][4] for size in range(lower, upper + 1))
        range_peak = range_rows[range_index]
        ranges.append({"range": name, "successes": counts,
                       "requested_bytes": bytes_,
                       "current_live_objects": range_peak[0],
                       "current_live_requested_bytes": range_peak[1],
                       "observed_peak_live_objects": range_peak[2],
                       "observed_peak_live_requested_bytes": range_peak[3]})
    large_successes = sum(row[1] for row in size_rows[1025:])
    large_bytes = sum(row[4] for row in size_rows[1025:])
    ranges.append({"range": ">1024 (log2 buckets)",
                   "successes": large_successes,
                   "requested_bytes": large_bytes,
                   "current_live_objects": range_rows[5][0],
                   "current_live_requested_bytes": range_rows[5][1],
                   "observed_peak_live_objects": range_rows[5][2],
                   "observed_peak_live_requested_bytes": range_rows[5][3]})
    ranges.append({"range": "zero-size failures",
                   "failures": size_rows[0][2],
                   "failed_requested_bytes": size_rows[0][5],
                   "current_live_objects": range_rows[6][0],
                   "current_live_requested_bytes": range_rows[6][1],
                   "observed_peak_live_objects": range_rows[6][2],
                   "observed_peak_live_requested_bytes": range_rows[6][3]})

    phase_reports = []
    for phase_id, name in enumerate(PHASE_NAMES):
        if phase_id > header["final_phase"]:
            break
        phase_count = sum(row[0] for row in phase_rows[phase_id])
        phase_frees = sum(row[1] for row in phase_rows[phase_id])
        peak_objects = sum(row[2] for row in phase_rows[phase_id])
        peak_bytes = sum(row[3] for row in phase_rows[phase_id])
        survivors = sum(row[6] for row in phase_rows[phase_id])
        survivor_bytes = sum(row[7] for row in phase_rows[phase_id])
        phase_reports.append({
            "phase": name, "successful_allocations": phase_count,
            "frees": phase_frees,
            "per_size_peak_sum_objects_conservative": peak_objects,
            "per_size_peak_sum_requested_bytes_conservative": peak_bytes,
            "live_objects_at_end": sum(row[4] for row in phase_rows[phase_id]),
            "live_requested_bytes_at_end": sum(row[5] for row in phase_rows[phase_id]),
            "objects_born_here_surviving_boundary": survivors,
            "survivor_requested_bytes": survivor_bytes,
        })

    candidate_reports = []
    for candidate_index, cutoff in enumerate(CUTOFFS):
        summary = candidate_summary[candidate_index]
        classes = candidate_classes(candidate_index)
        class_reports = []
        total_slabs_upper = 0
        total_slab_bytes_upper = 0
        total_round_waste = 0
        routed = 0
        for class_index, class_size in enumerate(classes):
            row = candidates[candidate_index][class_index]
            if row[0] != class_size:
                raise ValueError("candidate size-class table mismatch")
            if row[7] == 0 and row[1] == 0 and row[3] == 0:
                continue
            slots = slab_slots_per_page(class_size)
            if not slots:
                raise ValueError("candidate class does not fit in a slab page")
            slabs = math.ceil(row[7] / slots) if row[7] else 0
            total_slabs_upper += slabs
            total_slab_bytes_upper += slabs * 4096
            total_round_waste += row[4]
            routed += row[1]
            class_reports.append({
                "class_size": class_size, "slots_per_4k_slab": slots,
                "successful_allocations": row[1], "frees": row[2],
                "requested_bytes": row[3], "class_rounding_waste_bytes": row[4],
                "peak_live_objects": row[7],
                "peak_live_requested_bytes": row[8],
                "cross_cpu_frees": row[9],
                "active_cpu_indices": [
                    cpu_id for cpu_id in range(256)
                    if candidate_cpu_masks[candidate_index][class_index]
                       [cpu_id // 64] & (1 << (cpu_id % 64))
                ],
                "slabs_for_class_peak": slabs,
                "slab_object_capacity": slabs * slots,
                "unused_slots_at_class_peak": slabs * slots - row[7],
                "descriptor_bitmap_bytes": slabs * 104,
                "object_area_alignment_padding_bytes": slabs *
                    (SLAB_OBJECT_AREA_OFFSET - 104),
                "slot_metadata_bytes_per_object":
                    SLAB_SLOT_METADATA_BYTES,
                "page_tail_bytes": slabs * (SLAB_PAGE_BYTES -
                    SLAB_OBJECT_AREA_OFFSET -
                    slots * (class_size + SLAB_SLOT_METADATA_BYTES)),
                "indexed_heap_headers_bytes": slabs * 48,
                "phase_peaks": [
                    candidate_phases[p][candidate_index][class_index][2]
                    for p in range(header["final_phase"] + 1)
                ],
                "phase_survivors": [
                    candidate_phases[p][candidate_index][class_index][6]
                    for p in range(header["final_phase"])
                ],
            })
        if routed != summary[2] or routed + summary[3] != header["successes"]:
            raise ValueError("candidate routed count mismatch")
        if summary[5] != sum(row[5] for row in candidates[candidate_index]) or \
           summary[6] != sum(row[6] for row in candidates[candidate_index]):
            raise ValueError("candidate current-live totals mismatch")
        current_pages = sum(
            math.ceil(row[5] / slab_slots_per_page(row[0]))
            for row in candidates[candidate_index][:len(classes)]
            if row[5])
        if summary[9] != current_pages:
            raise ValueError("candidate current slab-page total mismatch")
        current_capacity = sum(
            math.ceil(row[5] / slab_slots_per_page(row[0])) *
            slab_slots_per_page(row[0])
            for row in candidates[candidate_index][:len(classes)] if row[5])
        if summary[11] != current_capacity:
            raise ValueError("candidate current slot-capacity mismatch")
        if summary[10] > total_slabs_upper or summary[7] < summary[5] or \
           summary[8] < summary[6] or summary[12] < summary[11]:
            raise ValueError("candidate peak counters are inconsistent")
        if sum(item["requested_bytes"] for item in class_reports) > header["successful_requested_bytes"]:
            raise ValueError("candidate request-byte accounting exceeds total")
        candidate_reports.append({
            "cutoff": cutoff, "class_count": summary[1],
            "slab_routed_successes": summary[2],
            "heap_bypassed_successes": summary[3],
            "heap_bypassed_requested_bytes": summary[4],
            "allocation_count_coverage_percent":
                100.0 * summary[2] / header["successes"] if header["successes"] else 0.0,
            "slab_requested_byte_coverage_percent":
                100.0 * sum(item["requested_bytes"] for item in class_reports)
                / header["successful_requested_bytes"]
                if header["successful_requested_bytes"] else 0.0,
            "class_rounding_waste_bytes": total_round_waste,
            "slabs_from_individual_class_peaks_conservative": total_slabs_upper,
            "slab_page_bytes_from_individual_class_peaks_conservative": total_slab_bytes_upper,
            "observed_simultaneous_peak_live_objects": summary[7],
            "observed_simultaneous_peak_live_requested_bytes": summary[8],
            "observed_simultaneous_peak_slab_pages": summary[10],
            "observed_simultaneous_peak_slab_bytes": summary[10] * 4096,
            "peak_slot_capacity_at_an_observed_point": summary[12],
            "maximum_simultaneously_unused_slots": summary[13],
            "current_slab_pages_at_snapshot": summary[9],
            "phase_simultaneous_peak_slab_pages": [
                candidate_phase_summary[p][candidate_index *
                                           HEAP_PROFILE_CANDIDATE_PHASE_FIELDS +
                                           2]
                for p in range(header["final_phase"] + 1)
            ],
            "phase_peak_slot_capacity": [
                candidate_phase_summary[p][candidate_index *
                                           HEAP_PROFILE_CANDIDATE_PHASE_FIELDS +
                                           3]
                for p in range(header["final_phase"] + 1)
            ],
            "phase_peak_unused_slots": [
                candidate_phase_summary[p][candidate_index *
                                           HEAP_PROFILE_CANDIDATE_PHASE_FIELDS +
                                           4]
                for p in range(header["final_phase"] + 1)
            ],
            "additional_heap_header_bytes": total_slabs_upper * 48,
            "classes": class_reports,
            "note": "Observed slab-page peak is the maximum sum of per-class ceil(live slots / slots per page) at allocator commit points. The separate sum of each class's independent peak is a conservative upper bound. Page alignment gaps remain indexed-heap free blocks when representable.",
        })

    report = {
        "format": "PITH-HEAP-PROFILE-4", "status": "PASS",
        "phase_boundary": "phase 0 starts at heap initialization; phase 1 begins at the native fixture's explicit marker",
        "lifetime_unit": "successful kmalloc/kfree allocator operations, not elapsed time",
        "operation_order": "heap metadata lock commit order",
        "event_trace": {
            "status": "PASS", "events": event_count,
            "sha256": hashlib.sha256(event_payload).hexdigest(),
            "ordering_limit": "serialized heap commit order, not call-entry order or wall-clock overlap",
        },
        "cpu_online_count": header["cpu_count"],
        "totals": {key: header[key] for key in (
            "operations", "attempts", "successes", "failures", "frees",
            "successful_requested_bytes", "failed_requested_bytes",
            "cross_cpu_frees", "tracked_live", "tracked_peak",
            "tracking_misses", "max_probe", "lifetime_samples",
            "lifetime_operation_sum")},
        "allocation_contexts": {
            name: header[f"alloc_context_{name}"]
            for name in CONTEXT_NAMES
        },
        "failure_contexts": {
            name: header[f"fail_context_{name}"]
            for name in CONTEXT_NAMES
        },
        "free_contexts": {
            name: header[f"free_context_{name}"]
            for name in CONTEXT_NAMES
        },
        "size_ranges": ranges,
        "exact_size_histogram": [
            {"requested_bytes": size, "attempts": row[0],
             "successes": row[1], "failures": row[2], "frees": row[3],
             "successful_requested_bytes": row[4],
             "failed_requested_bytes": row[5], "current_live": row[6],
             "current_live_requested_bytes": row[7], "peak_live": row[8],
             "peak_live_requested_bytes": row[9], "lifetime_samples": row[10],
             "lifetime_operation_sum": row[11], "cross_cpu_frees": row[12]}
            for size, row in enumerate(size_rows[:1025]) if any(row)
        ],
        "log_size_histogram": [
            {"request_size_log2": index,
             "successes": size_rows[HEAP_PROFILE_EXACT_MAX + 1 + index][1],
             "failures": size_rows[HEAP_PROFILE_EXACT_MAX + 1 + index][2],
             "frees": size_rows[HEAP_PROFILE_EXACT_MAX + 1 + index][3],
             "successful_requested_bytes":
                 size_rows[HEAP_PROFILE_EXACT_MAX + 1 + index][4],
             "failed_requested_bytes":
                 size_rows[HEAP_PROFILE_EXACT_MAX + 1 + index][5],
             "current_live":
                 size_rows[HEAP_PROFILE_EXACT_MAX + 1 + index][6],
             "peak_live":
                 size_rows[HEAP_PROFILE_EXACT_MAX + 1 + index][8]}
            for index in range(HEAP_PROFILE_LOG_BUCKETS)
            if any(size_rows[HEAP_PROFILE_EXACT_MAX + 1 + index])
        ],
        "phases": phase_reports,
        "per_cpu": [{"cpu": i, "allocations": row[0], "frees": row[1],
                     "cross_cpu_frees": row[2]}
                    for i, row in enumerate(cpu) if any(row)],
        "lifetime_histogram": [
            {"request_size_log2": size_bucket,
             "lifetime_operations_log2_counts": list(bins)}
            for size_bucket, bins in enumerate(lifetime) if any(bins)
        ],
        "slab_candidates": candidate_reports,
        "event_trace_records": event_trace,
    }
    return report


if __name__ == "__main__":
    source = Path(sys.argv[1])
    destination = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    result = parse_snapshot(source.read_bytes())
    output = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if destination:
        destination.write_text(output)
    else:
        sys.stdout.write(output)
