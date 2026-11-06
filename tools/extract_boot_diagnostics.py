#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read and validate opt-in Pith boot diagnostics from a Mangrove disk image."""

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import flash_usb  # noqa: E402
import update_mgfs  # noqa: E402

SECTOR = flash_usb.SECTOR
MGFS_BLOCK = 4096
MGFS_MAGIC = b"MGFSv1\0\0"
TRACE_HEADER = b"\n--- MANGROVE DIAGNOSTIC TRACE v1 ---\n"
FOOTER_MAGIC = b"MGBLOG01"
FOOTER_VERSION_V1 = 1
FOOTER_BYTES_V1 = 272
FOOTER_VERSION = 2
FOOTER_BYTES = 608
FLAG_TRACE_INCOMPLETE = 1
BLKGETSIZE64 = 0x80081272
COUNTER_NAMES = (
    "block_read_requests", "block_read_sectors", "block_read_min_sectors",
    "block_read_max_sectors", "block_write_requests", "block_write_sectors",
    "block_write_min_sectors", "block_write_max_sectors",
    "block_read_failures", "block_write_failures", "block_elapsed_ms",
    "block_max_ms", "bot_commands", "bot_read10", "bot_write10",
    "bot_other", "bot_failed", "bot_data_bytes", "bot_elapsed_ms",
    "bot_min_ms", "bot_max_ms", "bot_cbw_ms", "bot_data_ms", "bot_csw_ms",
)
WAIT_PHASES = ("preparation", "payload")
MEASUREMENT_KINDS = (
    "block_io_gate_acquire", "vfs_operation_guard_acquire",
    "vfs_operation_lock_acquire", "recovery_pin_handoff",
    "mgfs_ordinary_begin", "mgfs_mutation", "mgfs_ordinary_complete",
)


class ExtractionError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise ExtractionError(message)


def source_size(fd, info):
    if stat.S_ISREG(info.st_mode):
        return info.st_size
    require(stat.S_ISBLK(info.st_mode),
            "source must be a regular image or a block device")
    return struct.unpack("<Q", fcntl.ioctl(fd, BLKGETSIZE64, b"\0" * 8))[0]


def select_root_partition(gpt):
    roots = [partition for partition in gpt.partitions
             if partition[0] == "MANGROVE_ROOT"]
    require(len(roots) == 1,
            "GPT must contain exactly one MANGROVE_ROOT partition")
    return roots[0]


def read_exact(fd, offset, length):
    chunks = bytearray()
    while len(chunks) < length:
        part = os.pread(fd, length - len(chunks), offset + len(chunks))
        require(part, f"short read at byte {offset + len(chunks)}")
        chunks.extend(part)
    return bytes(chunks)


def open_source(source):
    # Resolve stable host aliases such as /dev/disk/by-id before opening with
    # O_NOFOLLOW. The opened path itself is then required to be a direct node.
    resolved = Path(source).resolve(strict=True)
    flags = (os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) |
             getattr(os, "O_NOFOLLOW", 0))
    return resolved, os.open(resolved, flags)


def mgfs_layout(image):
    require(len(image) >= MGFS_BLOCK and image[:8] == MGFS_MAGIC,
            "root partition does not contain MGFS")
    require(update_mgfs.u64(image, 8) == 1 and
            update_mgfs.u64(image, 16) == 2,
            "unsupported MGFS version")
    total_blocks = update_mgfs.u64(image, 40)
    require(total_blocks * MGFS_BLOCK == len(image),
            "MGFS geometry does not match the GPT root partition")
    return {
        "record_table_start": update_mgfs.u64(image, 136),
        "record_count": update_mgfs.u64(image, 152),
    }


def record_bytes(image, layout, record_id):
    offset = update_mgfs.record_offset(
        image, layout["record_table_start"], record_id,
        layout["record_count"])
    require(offset + update_mgfs.RECORD_BYTES <= len(image),
            "MGFS Record lies outside the root partition")
    return bytes(image[offset:offset + update_mgfs.RECORD_BYTES])


def record_payload_checked(image, record):
    size = update_mgfs.u64(record, 32)
    flags = update_mgfs.u64(record, 8)
    if flags & update_mgfs.RECORD_INLINE_DATA:
        require(size <= 56, "invalid inline MGFS payload")
        return record[128:128 + size]
    return update_mgfs.record_payload(image, record)


def mgfs_path(image, layout, components):
    record = record_bytes(image, layout, 1)
    for component in components:
        require(update_mgfs.u64(record, 0) == 2,
                "MGFS path component parent is not a directory")
        entries = record_payload_checked(image, record)
        record_id = update_mgfs.directory_find(entries, component)
        if record_id is None:
            return None
        record = record_bytes(image, layout, record_id)
    require(update_mgfs.u64(record, 0) == 1,
            "diagnostic path does not name a regular file")
    return record_payload_checked(image, record)


def decode_footer(log):
    if len(log) < FOOTER_BYTES_V1:
        return {"status": "INCOMPLETE", "reason": "footer absent"}
    footer_bytes = None
    offset = None
    version = None
    for candidate_version, candidate_bytes in (
            (FOOTER_VERSION, FOOTER_BYTES),
            (FOOTER_VERSION_V1, FOOTER_BYTES_V1)):
        if len(log) < candidate_bytes:
            continue
        candidate_offset = len(log) - candidate_bytes
        candidate = log[candidate_offset:]
        if candidate[:8] != FOOTER_MAGIC:
            continue
        found_version, found_bytes = struct.unpack_from("<II", candidate, 8)
        if found_version == candidate_version and found_bytes == candidate_bytes:
            footer_bytes = bytearray(candidate)
            offset = candidate_offset
            version = candidate_version
            break
    if footer_bytes is None:
        tail_start = max(0, len(log) - MGFS_BLOCK)
        unknown_offset = log.rfind(FOOTER_MAGIC, tail_start)
        if unknown_offset >= 0:
            if unknown_offset + 16 <= len(log):
                unknown_version, unknown_bytes = struct.unpack_from(
                    "<II", log, unknown_offset + 8)
                detail = f"version {unknown_version}, size {unknown_bytes}"
            else:
                detail = "truncated header"
            return {
                "status": "INCOMPLETE",
                "reason": f"unsupported completion footer ({detail})",
            }
        return {"status": "INCOMPLETE", "reason": "completion footer absent"}
    footer = footer_bytes
    _, _, state, flags = struct.unpack_from("<IIII", footer, 8)
    if struct.unpack_from("<I", footer, 76)[0] != 0:
        return {"status": "INCOMPLETE", "reason": "reserved footer field is nonzero"}
    expected_footer_crc = struct.unpack_from("<I", footer, 72)[0]
    struct.pack_into("<I", footer, 72, 0)
    if zlib.crc32(footer) != expected_footer_crc:
        return {"status": "INCOMPLETE", "reason": "footer CRC mismatch"}

    snapshot_bytes, trace_bytes, preparation_ms, write_ms, elapsed_before_finalize_ms = \
        struct.unpack_from("<QQQQQ", footer, 24)
    trace_lines = struct.unpack_from("<I", footer, 64)[0]
    payload_crc = struct.unpack_from("<I", footer, 68)[0]
    payload = log[:offset]
    trace_start = snapshot_bytes
    trace_end = trace_start + len(TRACE_HEADER) + trace_bytes
    if trace_start > len(payload) or trace_end > len(payload):
        return {"status": "INCOMPLETE", "reason": "payload lengths exceed file"}
    if payload[trace_start:trace_end - trace_bytes] != TRACE_HEADER:
        return {"status": "INCOMPLETE", "reason": "trace header mismatch"}
    if any(payload[trace_end:]):
        return {"status": "INCOMPLETE", "reason": "nonzero alignment padding"}
    if zlib.crc32(payload) != payload_crc:
        return {"status": "INCOMPLETE", "reason": "payload CRC mismatch"}
    trace = payload[trace_start + len(TRACE_HEADER):trace_end]
    if trace and (not trace.endswith(b"\n") or trace.count(b"\n") != trace_lines):
        return {"status": "INCOMPLETE", "reason": "trace line count mismatch"}
    counters = {
        name: struct.unpack_from("<Q", footer, 80 + 8 * index)[0]
        for index, name in enumerate(COUNTER_NAMES)
    }
    measurements = None
    if version == FOOTER_VERSION:
        measurements = {}
        wait_offset = FOOTER_BYTES_V1
        for phase in WAIT_PHASES:
            measurements[phase] = {}
            for kind in MEASUREMENT_KINDS:
                count, measurement_elapsed_ms, maximum_ms = struct.unpack_from(
                    "<QQQ", footer, wait_offset)
                measurements[phase][kind] = {
                    "count": count,
                    "elapsed_ms": measurement_elapsed_ms,
                    "maximum_ms": maximum_ms,
                }
                wait_offset += 24
    status = "COMPLETE" if state == 1 and not (flags & FLAG_TRACE_INCOMPLETE) \
        else "INCOMPLETE"
    reason = None
    if state != 1:
        reason = "completion state is not complete"
    elif flags & FLAG_TRACE_INCOMPLETE:
        reason = "diagnostic trace was truncated or unavailable"
    return {
        "status": status,
        "reason": reason,
        "footer_version": version,
        "completion_record_valid": state == 1,
        "snapshot_bytes": snapshot_bytes,
        "trace_bytes": trace_bytes,
        "trace_lines": trace_lines,
        "preparation_ms": preparation_ms,
        "payload_write_ms": write_ms,
        "elapsed_before_finalize_ms": elapsed_before_finalize_ms,
        "payload_crc32": f"{payload_crc:08x}",
        "storage": counters,
        "measurements": measurements,
        "snapshot": payload[:snapshot_bytes],
        "trace": trace,
    }


def diagnostic_trace_summary(result, file_bytes):
    storage = result["storage"]
    save_completed = result.get("completion_record_valid", False)
    rows = [
        "--- persisted boot-log completion summary ---",
        f"evidence={result['status']}",
        "boot-log-save=" + ("COMPLETED" if save_completed else "NOT PROVEN"),
    ]
    if result.get("reason"):
        rows.append(f"reason={result['reason']}")
    rows.extend((
        f"snapshot-bytes={result['snapshot_bytes']}",
        f"trace-bytes={result['trace_bytes']} lines={result['trace_lines']}",
        f"file-bytes={file_bytes}",
        f"preparation-ms={result['preparation_ms']}",
        f"payload-write-ms={result['payload_write_ms']}",
        f"elapsed-before-finalize-ms={result['elapsed_before_finalize_ms']}",
        "completion-record=" + ("valid" if save_completed
                                else "incomplete-state") +
        "; final footer self-duration is not persisted",
        "block read={block_read_requests}/{block_read_sectors} sectors "
        "write={block_write_requests}/{block_write_sectors} sectors "
        "failures={block_read_failures}/{block_write_failures} "
        "elapsed/max-ms={block_elapsed_ms}/{block_max_ms}".format(**storage),
        "BOT commands={bot_commands} read10={bot_read10} write10={bot_write10} "
        "failed={bot_failed} bytes={bot_data_bytes} elapsed-ms={bot_elapsed_ms} "
        "CBW/data/CSW-ms={bot_cbw_ms}/{bot_data_ms}/{bot_csw_ms}".format(
            **storage),
    ))
    measurements = result.get("measurements")
    if measurements:
        for phase in WAIT_PHASES:
            values = measurements[phase]
            rows.append(
                f"{phase} measurements: " + " ".join(
                    f"{kind}={values[kind]['count']}/"
                    f"{values[kind]['elapsed_ms']}/"
                    f"{values[kind]['maximum_ms']}ms"
                    for kind in MEASUREMENT_KINDS))
    if save_completed:
        rows.insert(6,
            "payload-write=COMPLETE content-bytes={} duration-ms={}".format(
                result["snapshot_bytes"] + result["trace_bytes"],
                result["payload_write_ms"]))
    return ("\n".join(rows) + "\n").encode("ascii")


def extract(source, output, checker):
    source = Path(source)
    output = Path(output)
    checker = Path(checker)
    require(not output.exists(), "output path must be new")
    require(checker.is_file() and os.access(checker, os.X_OK),
            f"MGFS checker is missing or not executable: {checker}")
    output.mkdir(parents=True, exist_ok=False)
    partition_path = output / ".mgfs-root-check.img"
    fd = None
    try:
        source, fd = open_source(source)
        evidence = {
            "source_kind": "block-device" if stat.S_ISBLK(os.fstat(fd).st_mode)
                           else "image-file",
            "read_only": True,
        }
        info = os.fstat(fd)
        size = source_size(fd, info)
        gpt = flash_usb.load_gpt(
            lambda offset, length: read_exact(fd, offset, length), size)
        root_name, first_lba, last_lba, _ = select_root_partition(gpt)
        root_offset = first_lba * SECTOR
        root_bytes = (last_lba - first_lba + 1) * SECTOR
        digest = hashlib.sha256()
        with partition_path.open("xb") as part:
            copied = 0
            while copied < root_bytes:
                chunk = read_exact(fd, root_offset + copied,
                                   min(1024 * 1024, root_bytes - copied))
                part.write(chunk)
                digest.update(chunk)
                copied += len(chunk)
            part.flush()
            os.fsync(part.fileno())
        checked = subprocess.run([str(checker), str(partition_path)],
                                 text=True, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, check=False,
                                 timeout=120)
        (output / "mgfsck.txt").write_text(checked.stdout)
        require(checked.returncode == 0 and
                re.search(r"^Errors: 0$", checked.stdout, re.M),
                "independent MGFS check failed; see mgfsck.txt")
        root_image = partition_path.read_bytes()
        layout = mgfs_layout(root_image)
        logs = {}
        for filename in ("boot.log", "boot.previous.log"):
            try:
                content = mgfs_path(root_image, layout,
                                    ("sys", "logs", filename))
            except (IndexError, RuntimeError, struct.error) as error:
                raise ExtractionError(f"cannot read /sys/logs/{filename}: {error}")
            if content is None:
                logs[filename] = {"status": "MISSING"}
                continue
            destination = output / filename
            destination.write_bytes(content)
            result = decode_footer(content)
            result["sha256"] = hashlib.sha256(content).hexdigest()
            if "snapshot" in result:
                trace = result.pop("trace")
                (output / f"{filename}.trace.txt").write_bytes(
                    trace + diagnostic_trace_summary(result, len(content)))
                (output / f"{filename}.snapshot.txt").write_bytes(
                    result.pop("snapshot"))
            logs[filename] = result
        evidence.update({
            "capacity_bytes": size,
            "gpt_sectors": gpt.sectors,
            "root_partition": {"name": root_name, "first_lba": first_lba,
                               "last_lba": last_lba, "bytes": root_bytes,
                               "sha256": digest.hexdigest()},
            "mgfs_check": "PASS",
            "logs": logs,
        })
        (output / "extraction.json").write_text(
            json.dumps(evidence, indent=2, sort_keys=True) + "\n")
        os.unlink(partition_path)
        return evidence
    finally:
        if fd is not None:
            os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True,
                        help="read-only full Mangrove image or block device")
    parser.add_argument("--output", required=True,
                        help="new directory for extracted logs and checks")
    parser.add_argument("--mgfsck", type=Path, default=ROOT / "build/mgfsck")
    args = parser.parse_args()
    try:
        report = extract(args.source, args.output, args.mgfsck)
    except (ExtractionError, OSError, subprocess.SubprocessError,
            flash_usb.FlashError) as error:
        print(f"boot-diagnostics: FAILED: {error}", file=sys.stderr)
        return 1
    for name, result in report["logs"].items():
        reason = f" ({result['reason']})" if result.get("reason") else ""
        print(f"{name}: {result['status']}{reason}")
    print(f"MGFS integrity: PASS; evidence: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
