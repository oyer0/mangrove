#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the host verifier for persistent diagnostic log records."""

import importlib.util
import os
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import zlib

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "extract_boot_diagnostics", ROOT / "tools/extract_boot_diagnostics.py")
EXTRACTOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXTRACTOR)


def make_log(*, state=1, flags=0, trace_lines=2, corrupt_padding=False,
             version=EXTRACTOR.FOOTER_VERSION):
    snapshot = b"boot snapshot\n"
    trace = b"47 [INFO] xHCI READY\n47 [INFO] BOT=3\n"
    prefix = snapshot + EXTRACTOR.TRACE_HEADER + trace
    footer_bytes = (EXTRACTOR.FOOTER_BYTES_V1 if version == 1 else
                    EXTRACTOR.FOOTER_BYTES)
    total = ((len(prefix) + footer_bytes + 4095) // 4096) * 4096
    footer_offset = total - footer_bytes
    payload = bytearray(footer_offset)
    payload[:len(prefix)] = prefix
    if corrupt_padding:
        payload[len(prefix)] = 1
    footer = bytearray(footer_bytes)
    footer[:8] = EXTRACTOR.FOOTER_MAGIC
    struct.pack_into("<IIII", footer, 8, version, footer_bytes, state, flags)
    struct.pack_into("<QQQQQ", footer, 24, len(snapshot), len(trace),
                     123, 456, 600)
    struct.pack_into("<I", footer, 64, trace_lines)
    struct.pack_into("<I", footer, 68, zlib.crc32(payload))
    for index, value in enumerate(range(1, 25)):
        struct.pack_into("<Q", footer, 80 + index * 8, value)
    if version == 2:
        wait_offset = EXTRACTOR.FOOTER_BYTES_V1
        value = 1
        for _phase in EXTRACTOR.WAIT_PHASES:
            for _kind in EXTRACTOR.MEASUREMENT_KINDS:
                struct.pack_into("<QQQ", footer, wait_offset,
                                 value, value + 1, value + 2)
                wait_offset += 24
                value += 3
    struct.pack_into("<I", footer, 72, zlib.crc32(footer))
    return bytearray(payload + footer)


def expect_incomplete(log, reason):
    result = EXTRACTOR.decode_footer(log)
    assert result["status"] == "INCOMPLETE", result
    assert reason in result["reason"], result


def main():
    with tempfile.TemporaryDirectory(prefix="boot-diagnostics-source-") as td:
        target = Path(td) / "disk.img"
        alias = Path(td) / "selected-device"
        target.write_bytes(b"read-only fixture")
        alias.symlink_to(target)
        resolved, descriptor = EXTRACTOR.open_source(alias)
        try:
            assert resolved == target.resolve()
            assert os.read(descriptor, 64) == b"read-only fixture"
            try:
                os.write(descriptor, b"must not write")
            except OSError:
                pass
            else:
                raise AssertionError("source descriptor permits writes")
        finally:
            os.close(descriptor)
        assert target.read_bytes() == b"read-only fixture"

    valid = make_log()
    result = EXTRACTOR.decode_footer(valid)
    assert result["status"] == "COMPLETE", result
    assert result["completion_record_valid"]
    assert result["snapshot"] == b"boot snapshot\n"
    assert result["trace"] == b"47 [INFO] xHCI READY\n47 [INFO] BOT=3\n"
    assert result["preparation_ms"] == 123
    assert result["payload_write_ms"] == 456
    assert result["elapsed_before_finalize_ms"] == 600
    assert result["storage"]["block_read_requests"] == 1
    assert result["storage"]["bot_csw_ms"] == 24
    assert result["footer_version"] == 2
    assert result["measurements"]["preparation"]["block_io_gate_acquire"] == {
        "count": 1, "elapsed_ms": 2, "maximum_ms": 3}
    assert result["measurements"]["payload"]["mgfs_ordinary_complete"] == {
        "count": 40, "elapsed_ms": 41, "maximum_ms": 42}
    summary = EXTRACTOR.diagnostic_trace_summary(result, len(valid)).decode()
    assert "preparation-ms=123" in summary
    assert "payload-write-ms=456" in summary
    assert "elapsed-before-finalize-ms=600" in summary
    assert "completion-record=valid" in summary
    assert "boot-log-save=COMPLETED" in summary
    assert "BOT commands=13" in summary
    assert "preparation measurements: block_io_gate_acquire=1/2/3ms" in summary

    legacy = EXTRACTOR.decode_footer(make_log(version=1))
    assert legacy["status"] == "COMPLETE" and legacy["measurements"] is None

    root = ("MANGROVE_ROOT", 100, 200, b"root")
    assert EXTRACTOR.select_root_partition(
        SimpleNamespace(partitions=(root,))) == root
    for partitions in ((), (root, root)):
        try:
            EXTRACTOR.select_root_partition(SimpleNamespace(
                partitions=partitions))
        except EXTRACTOR.ExtractionError as error:
            assert "exactly one MANGROVE_ROOT" in str(error)
        else:
            raise AssertionError("missing or duplicate root GPT entry accepted")

    expect_incomplete(b"old text without completion record", "footer absent")
    expect_incomplete(make_log(version=3), "unsupported completion footer")
    damaged_payload = bytearray(valid)
    damaged_payload[0] ^= 1
    expect_incomplete(damaged_payload, "payload CRC mismatch")
    damaged_footer = bytearray(valid)
    damaged_footer[-1] ^= 1
    expect_incomplete(damaged_footer, "footer CRC mismatch")
    expect_incomplete(make_log(state=0), "completion state")
    truncated_trace = EXTRACTOR.decode_footer(
        make_log(flags=EXTRACTOR.FLAG_TRACE_INCOMPLETE))
    assert truncated_trace["status"] == "INCOMPLETE"
    assert truncated_trace["completion_record_valid"]
    assert "trace was truncated" in truncated_trace["reason"]
    truncated_summary = EXTRACTOR.diagnostic_trace_summary(
        truncated_trace, 4096).decode()
    assert "evidence=INCOMPLETE" in truncated_summary
    assert "boot-log-save=COMPLETED" in truncated_summary
    assert "payload-write=COMPLETE" in truncated_summary
    expect_incomplete(make_log(trace_lines=3), "trace line count")
    expect_incomplete(make_log(corrupt_padding=True), "padding")

    # Footer CRC is valid here; the host must still check payload CRC.
    bad_payload_crc = bytearray(valid)
    footer = len(bad_payload_crc) - EXTRACTOR.FOOTER_BYTES
    struct.pack_into("<I", bad_payload_crc, footer + 68, 0x12345678)
    struct.pack_into("<I", bad_payload_crc, footer + 72, 0)
    struct.pack_into("<I", bad_payload_crc, footer + 72,
                     zlib.crc32(bad_payload_crc[footer:]))
    expect_incomplete(bad_payload_crc, "payload CRC")

    print("boot diagnostic extraction: complete, interrupted, truncated, corrupt CRC/length and padding cases passed")


if __name__ == "__main__":
    main()
