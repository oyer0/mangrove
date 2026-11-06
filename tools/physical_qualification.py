#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Create and validate bounded records for physical Mangrove qualification."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys


SCHEMA = "mangrove-physical-qualification-v1"
STATUSES = ("NOT TESTED", "INCOMPLETE", "PASS", "FAIL")
STAGES = (
    "uefi_loader",
    "kernel_handoff",
    "kernel_initialization",
    "device_discovery",
    "root_mount",
    "shoot_input",
    "test_workload",
    "shutdown_recovery",
)
EVIDENCE_KINDS = ("photo", "log", "transcript", "host_report", "other")
EVIDENCE_SOURCES = ("OPERATOR", "AUTOMATED")
MAX_EVIDENCE_FILES = 64
MAX_EVIDENCE_FILE_BYTES = 32 * 1024 * 1024
MAX_EVIDENCE_TOTAL_BYTES = 128 * 1024 * 1024
SENSITIVE_KEY = re.compile(
    r"(?:^|[_-])(serial(?:_number)?|mac(?:_address)?|bssid|ssid|password|"
    r"passwd|credential|secret|token|service_tag|asset_tag)(?:$|[_-])", re.I
)
SENSITIVE_TEXT = re.compile(
    r"(?:\b(?:serial(?:\s+number)?|mac(?:\s+address)?|bssid|ssid|password|"
    r"credential|secret)\s*[:=]\s*\S+|"
    r"\b(?:[0-9a-f]{2}:){5}[0-9a-f]{2}\b)", re.I
)
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class ReportError(ValueError):
    """Invalid report, evidence reference, or requested operation."""


def _now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def _is_utc_timestamp(value: object) -> bool:
    if not isinstance(value, str):
        return False
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return False
    return parsed.tzinfo is not None and parsed.utcoffset() == timezone.utc.utcoffset(parsed)


def _canonical_json(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def _safe_text(value: object, label: str) -> str:
    if not isinstance(value, str) or not value.strip() or "\x00" in value:
        raise ReportError(f"{label} must be non-empty text")
    if SENSITIVE_TEXT.search(value):
        raise ReportError(f"{label} appears to contain a personal or secret identifier")
    return value.strip()


def _check_sensitive_keys(value: object, where: str = "report") -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            if not isinstance(key, str) or SENSITIVE_KEY.search(key):
                raise ReportError(f"{where} contains a disallowed personal or secret field")
            _check_sensitive_keys(child, f"{where}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _check_sensitive_keys(child, f"{where}[{index}]")
    elif isinstance(value, str) and SENSITIVE_TEXT.search(value):
        raise ReportError(f"{where} appears to contain a personal or secret identifier")


def _run_git(repo: Path, *args: str) -> bytes:
    try:
        return subprocess.check_output(["git", *args], cwd=repo, stderr=subprocess.PIPE)
    except (OSError, subprocess.CalledProcessError) as error:
        detail = getattr(error, "stderr", b"")
        if isinstance(detail, bytes):
            detail = detail.decode("utf-8", errors="replace").strip()
        raise ReportError(f"cannot read Git identity ({' '.join(args)}): {detail or error}") from error


def _source_identity(repo: Path) -> dict:
    repo = repo.resolve(strict=True)
    revision = _run_git(repo, "rev-parse", "--verify", "HEAD^{commit}").decode().strip()
    if not re.fullmatch(r"[0-9a-f]{40,64}", revision):
        raise ReportError("Git returned an invalid source revision")
    status = _run_git(repo, "status", "--porcelain=v1", "--untracked-files=all", "-z")
    diff = _run_git(repo, "diff", "--binary", "HEAD", "--")
    untracked = _run_git(repo, "ls-files", "--others", "--exclude-standard", "-z")
    untracked_hash = hashlib.sha256()
    untracked_paths = [item for item in untracked.split(b"\0") if item]
    for raw_path in sorted(untracked_paths):
        rel = raw_path.decode("utf-8", errors="strict")
        path = repo / rel
        mode = path.lstat().st_mode & 0o7777
        if path.is_symlink():
            content_hash = hashlib.sha256(
                os.readlink(path).encode("utf-8", errors="surrogateescape")).digest()
        elif path.is_file():
            content_hash = bytes.fromhex(_sha256_file(path)[0])
        else:
            raise ReportError(f"untracked source path is not a regular file: {rel}")
        untracked_hash.update(raw_path + b"\0" + mode.to_bytes(2, "big") + content_hash)
    return {
        "revision": revision,
        "worktree_dirty": bool(status),
        "tracked_diff_sha256": _sha256_bytes(diff),
        "untracked_source_sha256": untracked_hash.hexdigest(),
        "untracked_file_count": len(untracked_paths),
    }


def _repo_file(repo: Path, value: str, label: str) -> tuple[Path, str]:
    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = repo / candidate
    if candidate.is_symlink():
        raise ReportError(f"{label} must not be a symlink")
    try:
        resolved = candidate.resolve(strict=True)
        rel = resolved.relative_to(repo.resolve(strict=True)).as_posix()
    except (OSError, ValueError) as error:
        raise ReportError(f"{label} must be an existing file inside the repository") from error
    if not resolved.is_file():
        raise ReportError(f"{label} must be a regular file")
    return resolved, rel


def _artifact(repo: Path, value: str, label: str) -> dict:
    path, rel = _repo_file(repo, value, label)
    digest, size = _sha256_file(path)
    return {"path": rel, "size_bytes": size, "sha256": digest}


def _parse_pairs(items: list[str], label: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for item in items:
        if "=" not in item:
            raise ReportError(f"{label} entries must use NAME=VALUE")
        key, value = item.split("=", 1)
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_.-]*", key) or not value.strip():
            raise ReportError(f"invalid {label} entry: {item}")
        if SENSITIVE_KEY.search(key):
            raise ReportError(f"{label} contains a disallowed personal or secret field")
        if key in result:
            raise ReportError(f"duplicate {label} key: {key}")
        result[key] = _safe_text(value, label)
    return result


def _load_firmware(path: Path) -> dict:
    try:
        snapshot = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ReportError(f"cannot read firmware snapshot JSON: {error}") from error
    if not isinstance(snapshot, dict) or not isinstance(snapshot.get("values"), dict) or not snapshot["values"]:
        raise ReportError("firmware snapshot must be a JSON object with a 'values' object")
    _check_sensitive_keys(snapshot["values"], "firmware snapshot")
    return {
        "source": _safe_text(snapshot.get("source", "OPERATOR"), "firmware snapshot source"),
        "values": snapshot["values"],
        "values_sha256": _sha256_bytes(_canonical_json(snapshot["values"])),
    }


def _new_report(args: argparse.Namespace) -> dict:
    repo = Path(args.repo).resolve(strict=True)
    profile, profile_rel = _repo_file(repo, args.profile, "hardware profile")
    firmware = _load_firmware(Path(args.firmware_snapshot).resolve(strict=True))
    medium_model = _safe_text(args.medium_model, "boot-medium model")
    if args.medium_capacity is not None and args.medium_capacity <= 0:
        raise ReportError("boot-medium capacity must be positive")
    if args.medium_kind not in ("USB", "SATA", "NETWORK", "OTHER"):
        raise ReportError("unsupported boot-medium kind")
    if args.medium_verification not in ("NOT TESTED", "VERIFIED", "FAILED"):
        raise ReportError("invalid boot-medium verification state")
    expected = [_safe_text(item, "expected action") for item in args.expected_action]
    if not expected:
        raise ReportError("at least one expected physical action is required")
    required = list(dict.fromkeys(args.required_stage))
    if not required or any(stage not in STAGES for stage in required):
        raise ReportError("select at least one supported required stage")
    deferred = [_safe_text(item, "deferred-device note") for item in args.deferred_device]
    return {
        "schema": SCHEMA,
        "created_utc": _now(),
        "hardware_profile": {
            "id": _safe_text(args.profile_id, "hardware-profile identifier"),
            "reference": profile_rel,
            "sha256": _sha256_file(profile)[0],
        },
        "firmware_snapshot": firmware,
        "source_identity": _source_identity(repo),
        "build_artifacts": {
            "loader": _artifact(repo, args.loader, "UEFI loader"),
            "kernel": _artifact(repo, args.kernel, "Pith kernel"),
            "image": _artifact(repo, args.image, "boot image"),
            "embedded_loader_kernel_match": "NOT TESTED",
        },
        "boot_medium": {
            "kind": args.medium_kind,
            "model": medium_model,
            "capacity_bytes": args.medium_capacity,
            "vendor_product_id": args.medium_vid_pid,
            "write_verification": args.medium_verification,
        },
        "configuration": {
            "build_options": _parse_pairs(args.build_option, "build option"),
            "diagnostic_options": _parse_pairs(args.diagnostic_option, "diagnostic option"),
            "boot_classification": args.boot_classification,
        },
        "expected_actions": expected,
        "required_stages": required,
        "deferred_devices": deferred,
        "stage_results": {
            stage: {"status": "NOT TESTED", "observations": []} for stage in STAGES
        },
        "test_result": {
            "status": "NOT TESTED",
            "history": ["NOT TESTED"],
            "failure_stage": None,
            "physical_confirmation": {"confirmed": False, "confirmed_utc": None},
        },
        "evidence": [],
    }


def _valid_transition(history: object, status: object, label: str) -> None:
    if not isinstance(history, list) or not history or history[0] != "NOT TESTED":
        raise ReportError(f"{label} history must begin with NOT TESTED")
    if any(item not in STATUSES for item in history):
        raise ReportError(f"{label} history contains an unknown result state")
    if status != history[-1]:
        raise ReportError(f"{label} status does not match its final history entry")
    seen_terminal = False
    for previous, current in zip(history, history[1:]):
        if seen_terminal:
            raise ReportError(f"{label} has a transition after a terminal result")
        if previous == "NOT TESTED" and current != "INCOMPLETE":
            raise ReportError(f"{label} must enter INCOMPLETE before PASS or FAIL")
        if previous == "INCOMPLETE" and current not in ("INCOMPLETE", "PASS", "FAIL"):
            raise ReportError(f"invalid {label} transition {previous} -> {current}")
        if previous in ("PASS", "FAIL"):
            raise ReportError(f"{label} terminal result cannot transition")
        seen_terminal = current in ("PASS", "FAIL")


def _relative_evidence_file(root: Path, relative: str) -> Path:
    rel = PurePosixPath(relative)
    if rel.is_absolute() or not rel.parts or any(part in ("", ".", "..") for part in rel.parts):
        raise ReportError("evidence paths must be normalized and relative")
    root_resolved = root.resolve(strict=True)
    candidate = root_resolved.joinpath(*rel.parts)
    try:
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(root_resolved)
    except (OSError, ValueError) as error:
        raise ReportError(f"evidence file is missing or escapes evidence root: {relative}") from error
    if candidate.is_symlink() or not resolved.is_file():
        raise ReportError(f"evidence reference must be a regular, non-symlink file: {relative}")
    return resolved


def validate_report(report: object, repo: Path | None = None,
                    evidence_root: Path | None = None,
                    verify_files: bool = True) -> None:
    if not isinstance(report, dict) or report.get("schema") != SCHEMA:
        raise ReportError("unsupported physical qualification report schema")
    _check_sensitive_keys(report)
    if not _is_utc_timestamp(report.get("created_utc")):
        raise ReportError("report creation time must be an ISO-8601 UTC timestamp")
    evidence = report.get("evidence")
    if not isinstance(evidence, list) or len(evidence) > MAX_EVIDENCE_FILES:
        raise ReportError("evidence list is missing or exceeds its bounded entry count")
    profile = report.get("hardware_profile")
    if not isinstance(profile, dict) or not profile.get("id") or not profile.get("reference"):
        raise ReportError("hardware profile identity/reference is missing")
    _check_hash(profile.get("sha256"), "hardware profile")
    firmware = report.get("firmware_snapshot")
    if not isinstance(firmware, dict) or not isinstance(firmware.get("values"), dict):
        raise ReportError("firmware snapshot values are missing")
    if not any(key in firmware["values"] for key in ("bios_version", "firmware_version")):
        raise ReportError("firmware snapshot must record bios_version or firmware_version (use unknown if needed)")
    _check_hash(firmware.get("values_sha256"), "firmware snapshot")
    if firmware["values_sha256"] != _sha256_bytes(_canonical_json(firmware["values"])):
        raise ReportError("firmware snapshot hash does not match its values")

    source = report.get("source_identity")
    if not isinstance(source, dict) or not re.fullmatch(r"[0-9a-f]{40,64}", str(source.get("revision", ""))):
        raise ReportError("source revision is missing or malformed")
    for field in ("tracked_diff_sha256", "untracked_source_sha256"):
        _check_hash(source.get(field), f"source identity {field}")
    if not isinstance(source.get("worktree_dirty"), bool) or not isinstance(source.get("untracked_file_count"), int):
        raise ReportError("source worktree state is malformed")
    if verify_files:
        if repo is None:
            raise ReportError("repository root is required to verify source identity")
        if _source_identity(repo) != source:
            raise ReportError("source revision or working-tree identity has changed")
        profile_path = _relative_evidence_file(repo, profile["reference"])
        if _sha256_file(profile_path)[0] != profile["sha256"]:
            raise ReportError("hardware profile hash has changed")

    artifacts = report.get("build_artifacts")
    if not isinstance(artifacts, dict) or artifacts.get("embedded_loader_kernel_match") not in (
            "NOT TESTED", "VERIFIED", "FAILED"):
        raise ReportError("build artifact identity is malformed")
    for name in ("loader", "kernel", "image"):
        item = artifacts.get(name)
        if not isinstance(item, dict) or not isinstance(item.get("path"), str):
            raise ReportError(f"{name} build artifact identity is missing")
        _check_hash(item.get("sha256"), f"{name} artifact")
        if not isinstance(item.get("size_bytes"), int) or item["size_bytes"] <= 0:
            raise ReportError(f"{name} artifact size is invalid")
        if verify_files:
            if repo is None:
                raise ReportError("repository root is required to verify build artifacts")
            path = _relative_evidence_file(repo, item["path"])
            digest, size = _sha256_file(path)
            if digest != item["sha256"] or size != item["size_bytes"]:
                raise ReportError(f"{name} artifact hash or size has changed")
    if artifacts["embedded_loader_kernel_match"] in ("VERIFIED", "FAILED"):
        if not any(item.get("source") == "AUTOMATED" and item.get("kind") == "host_report"
                   and any(term in item.get("description", "").lower()
                           for term in ("embedded", "image identity"))
                   for item in report.get("evidence", []) if isinstance(item, dict)):
            raise ReportError("embedded component match requires an automated host-report reference")

    medium = report.get("boot_medium")
    if not isinstance(medium, dict) or medium.get("kind") not in ("USB", "SATA", "NETWORK", "OTHER"):
        raise ReportError("boot-medium identity is malformed")
    _safe_text(medium.get("model"), "boot-medium model")
    if medium.get("capacity_bytes") is not None and (
            not isinstance(medium.get("capacity_bytes"), int) or medium["capacity_bytes"] <= 0):
        raise ReportError("boot-medium capacity is invalid")
    if medium.get("vendor_product_id") is not None and not re.fullmatch(
            r"[0-9a-fA-F]{4}:[0-9a-fA-F]{4}", str(medium["vendor_product_id"])):
        raise ReportError("boot-medium vendor/product ID must use vvvv:pppp format")
    if medium.get("write_verification") not in ("NOT TESTED", "VERIFIED", "FAILED"):
        raise ReportError("boot-medium write verification state is invalid")
    if medium["write_verification"] in ("VERIFIED", "FAILED") and not any(
            item.get("source") == "AUTOMATED" and item.get("kind") in ("transcript", "host_report")
            and any(term in item.get("description", "").lower() for term in ("flash", "write verification"))
            for item in report.get("evidence", []) if isinstance(item, dict)):
        raise ReportError("medium write result requires automated flash-verification evidence")

    config = report.get("configuration")
    if not isinstance(config, dict) or config.get("boot_classification") not in (
            "COLD", "WARM", "UNKNOWN"):
        raise ReportError("boot classification must be COLD, WARM, or UNKNOWN")
    for field in ("build_options", "diagnostic_options"):
        if not isinstance(config.get(field), dict):
            raise ReportError(f"{field} must be a JSON object")

    actions, required = report.get("expected_actions"), report.get("required_stages")
    if not isinstance(actions, list) or not actions or any(not isinstance(item, str) or not item.strip() for item in actions):
        raise ReportError("expected physical actions are missing")
    if not isinstance(required, list) or not required or len(set(required)) != len(required) or any(s not in STAGES for s in required):
        raise ReportError("required physical stages are missing or invalid")
    if not isinstance(report.get("deferred_devices"), list):
        raise ReportError("deferred-device list is malformed")

    stages = report.get("stage_results")
    if not isinstance(stages, dict) or set(stages) != set(STAGES):
        raise ReportError("stage result table is incomplete")
    for name, result in stages.items():
        if not isinstance(result, dict) or result.get("status") not in STATUSES:
            raise ReportError(f"stage result is malformed: {name}")
        observations = result.get("observations")
        if not isinstance(observations, list):
            raise ReportError(f"stage observations are malformed: {name}")
        for observation in observations:
            if (not isinstance(observation, dict) or observation.get("source") not in EVIDENCE_SOURCES
                    or not isinstance(observation.get("text"), str) or not observation["text"].strip()):
                raise ReportError(f"stage observation is malformed: {name}")

    outcome = report.get("test_result")
    if not isinstance(outcome, dict):
        raise ReportError("physical test result is missing")
    status = outcome.get("status")
    if status not in STATUSES:
        raise ReportError("physical result state is invalid")
    _valid_transition(outcome.get("history"), status, "physical result")
    confirmation = outcome.get("physical_confirmation")
    if not isinstance(confirmation, dict) or not isinstance(confirmation.get("confirmed"), bool):
        raise ReportError("physical confirmation record is malformed")
    if confirmation["confirmed"]:
        if not _is_utc_timestamp(confirmation.get("confirmed_utc")):
            raise ReportError("physical confirmation timestamp must be ISO-8601 UTC")
    elif confirmation.get("confirmed_utc") is not None:
        raise ReportError("unconfirmed physical result must not have a confirmation timestamp")
    failure_stage = outcome.get("failure_stage")
    if status == "FAIL":
        if failure_stage not in STAGES or stages[failure_stage]["status"] != "FAIL":
            raise ReportError("failed physical result requires a matching failed stage")
        if not any(item.get("source") == "OPERATOR" and item.get("text", "").strip()
                   for item in stages[failure_stage]["observations"]):
            raise ReportError("failed physical stage requires an operator observation")
    elif failure_stage is not None:
        raise ReportError("failure stage is set for a non-failed result")
    if status in ("PASS", "FAIL") and not confirmation["confirmed"]:
        raise ReportError("terminal physical result requires explicit operator confirmation")
    if status in ("PASS", "FAIL") and not any(
            item.get("source") == "OPERATOR" for item in report.get("evidence", [])
            if isinstance(item, dict)):
        raise ReportError("terminal physical result requires operator-supplied evidence")
    if status == "NOT TESTED":
        if (any(stages[name]["status"] != "NOT TESTED" or stages[name]["observations"] for name in STAGES)
                or confirmation["confirmed"] or failure_stage is not None):
            raise ReportError("NOT TESTED report contains physical boot results")
    if status == "PASS":
        if any(stages[stage]["status"] != "PASS" for stage in required):
            raise ReportError("physical PASS requires PASS for every required stage")
        if any(not any(item.get("source") == "OPERATOR" and item.get("text", "").strip()
                        for item in stages[stage]["observations"]) for stage in required):
            raise ReportError("physical PASS requires an operator observation for every required stage")
        if medium["kind"] == "USB" and medium["write_verification"] != "VERIFIED":
            raise ReportError("USB boot PASS requires verified media write/readback evidence")

    total_size = 0
    seen_paths: set[str] = set()
    for item in evidence:
        if not isinstance(item, dict) or item.get("kind") not in EVIDENCE_KINDS:
            raise ReportError("evidence reference is malformed")
        if item.get("source") not in EVIDENCE_SOURCES or not isinstance(item.get("description"), str):
            raise ReportError("evidence provenance or description is malformed")
        path = item.get("path")
        _check_hash(item.get("sha256"), "evidence")
        size = item.get("size_bytes")
        if not isinstance(path, str) or not isinstance(size, int) or size < 0 or size > MAX_EVIDENCE_FILE_BYTES:
            raise ReportError("evidence path or size is invalid")
        if path in seen_paths:
            raise ReportError("duplicate evidence path")
        seen_paths.add(path)
        total_size += size
        if total_size > MAX_EVIDENCE_TOTAL_BYTES:
            raise ReportError("evidence references exceed the total size limit")
        if verify_files:
            if evidence_root is None:
                raise ReportError("evidence root is required to verify evidence references")
            evidence_path = _relative_evidence_file(evidence_root, path)
            digest, actual_size = _sha256_file(evidence_path)
            if digest != item["sha256"] or actual_size != size:
                raise ReportError(f"evidence hash or size does not match: {path}")


def _check_hash(value: object, label: str) -> None:
    if not isinstance(value, str) or not SHA256_RE.fullmatch(value):
        raise ReportError(f"{label} SHA-256 is missing or malformed")


def _check_report_output(repo: Path, output: Path) -> None:
    repo = repo.resolve(strict=True)
    output = output.absolute()
    resolved = output.parent.resolve() / output.name
    try:
        relative = resolved.relative_to(repo)
    except ValueError:
        return
    if relative.parts and relative.parts[0] == ".git":
        raise ReportError("report output must not be written inside .git")
    checked = subprocess.run(["git", "check-ignore", "--quiet", "--", relative.as_posix()],
                             cwd=repo, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if checked.returncode != 0:
        raise ReportError("report output inside the repository must be ignored (use build/) or outside it")


def _write_new(path: Path, report: dict) -> None:
    path = path.absolute()
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() or path.is_symlink():
        raise ReportError(f"output already exists: {path}")
    data = _canonical_json(report)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    fd = os.open(path, flags, 0o600)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
    except BaseException:
        try:
            path.unlink()
        except OSError:
            pass
        raise


def _add_evidence(report: dict, args: argparse.Namespace) -> None:
    if not args.evidence:
        return
    if not args.evidence_root:
        raise ReportError("--evidence-root is required with --evidence")
    if len(report["evidence"]) + len(args.evidence) > MAX_EVIDENCE_FILES:
        raise ReportError("evidence entry limit exceeded")
    root = Path(args.evidence_root).resolve(strict=True)
    total = sum(item["size_bytes"] for item in report["evidence"])
    for value in args.evidence:
        if "=" not in value:
            raise ReportError("--evidence uses KIND=PATH")
        kind, source_value = value.split("=", 1)
        if kind not in EVIDENCE_KINDS:
            raise ReportError(f"unsupported evidence kind: {kind}")
        source = Path(source_value)
        if not source.is_absolute():
            source = root / source
        try:
            resolved = source.resolve(strict=True)
            relative = resolved.relative_to(root).as_posix()
        except (OSError, ValueError) as error:
            raise ReportError("evidence input must be inside --evidence-root") from error
        if source.is_symlink() or not resolved.is_file():
            raise ReportError("evidence input must be a regular non-symlink file")
        digest, size = _sha256_file(resolved)
        if size > MAX_EVIDENCE_FILE_BYTES:
            raise ReportError(f"evidence file exceeds {MAX_EVIDENCE_FILE_BYTES} bytes")
        total += size
        if total > MAX_EVIDENCE_TOTAL_BYTES:
            raise ReportError(f"evidence exceeds {MAX_EVIDENCE_TOTAL_BYTES} total bytes")
        if any(item["path"] == relative for item in report["evidence"]):
            raise ReportError(f"evidence already referenced: {relative}")
        report["evidence"].append({
            "kind": kind,
            "path": relative,
            "size_bytes": size,
            "sha256": digest,
            "source": args.evidence_source,
            "description": _safe_text(args.evidence_description, "evidence description"),
        })


def _cmd_create(args: argparse.Namespace) -> None:
    report = _new_report(args)
    validate_report(report, Path(args.repo), verify_files=True)
    _check_report_output(Path(args.repo), Path(args.output))
    _write_new(Path(args.output), report)
    print(f"Created {args.output}: physical result NOT TESTED; no physical outcome was inferred")


def _cmd_update(args: argparse.Namespace) -> None:
    input_path = Path(args.report)
    try:
        report = json.loads(input_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ReportError(f"cannot read report: {error}") from error
    validate_report(report, Path(args.repo), Path(args.evidence_root) if args.evidence_root else None,
                    verify_files=True)
    changed = False
    outcome = report["test_result"]
    if args.status:
        history = outcome["history"]
        previous = outcome["status"]
        if previous in ("PASS", "FAIL"):
            raise ReportError("terminal result cannot be updated; create a new report for a new run")
        if previous == "NOT TESTED" and args.status != "INCOMPLETE":
            raise ReportError("physical result must enter INCOMPLETE before PASS or FAIL")
        if previous == "INCOMPLETE" and args.status not in ("INCOMPLETE", "PASS", "FAIL"):
            raise ReportError("invalid physical result transition")
        if previous != args.status:
            history.append(args.status)
            outcome["status"] = args.status
            changed = True
    if args.stage:
        stage = report["stage_results"][args.stage]
        if args.stage_status:
            if stage["status"] in ("PASS", "FAIL") and stage["status"] != args.stage_status:
                raise ReportError("terminal stage result cannot be changed")
            if stage["status"] == "INCOMPLETE" and args.stage_status == "NOT TESTED":
                raise ReportError("stage cannot return to NOT TESTED after an attempt")
            stage["status"] = args.stage_status
            changed = True
        if args.observation:
            stage["observations"].append({"source": "OPERATOR", "text": _safe_text(args.observation, "observation")})
            changed = True
    elif args.stage_status or args.observation:
        raise ReportError("--stage is required with --stage-status or --observation")
    if args.failure_stage:
        outcome["failure_stage"] = args.failure_stage
        changed = True
    if args.confirm_physical:
        outcome["physical_confirmation"] = {"confirmed": True, "confirmed_utc": _now()}
        changed = True
    if args.medium_verification:
        report["boot_medium"]["write_verification"] = args.medium_verification
        changed = True
    if args.embedded_match:
        report["build_artifacts"]["embedded_loader_kernel_match"] = args.embedded_match
        changed = True
    if args.boot_classification:
        report["configuration"]["boot_classification"] = args.boot_classification
        changed = True
    before_evidence = len(report["evidence"])
    _add_evidence(report, args)
    changed |= len(report["evidence"]) != before_evidence
    if not changed:
        raise ReportError("update contains no changes")
    validate_report(report, Path(args.repo), Path(args.evidence_root) if args.evidence_root else None,
                    verify_files=True)
    _check_report_output(Path(args.repo), Path(args.output))
    _write_new(Path(args.output), report)
    print(f"Updated {args.output}: physical result {report['test_result']['status']}")


def _cmd_validate(args: argparse.Namespace) -> None:
    try:
        report = json.loads(Path(args.report).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ReportError(f"cannot read report: {error}") from error
    validate_report(report, Path(args.repo), Path(args.evidence_root) if args.evidence_root else None,
                    verify_files=not args.metadata_only)
    result = report["test_result"]["status"]
    if args.require_pass and result != "PASS":
        raise ReportError(f"report is valid but physical result is {result}, not PASS")
    print(f"VALID: {SCHEMA}; physical result={result}; evidence={len(report['evidence'])}")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    create = sub.add_parser("create", help="write a new NOT TESTED physical-run report")
    create.add_argument("--repo", default=".")
    create.add_argument("--profile-id", required=True)
    create.add_argument("--profile", required=True, help="profile document inside the repository")
    create.add_argument("--firmware-snapshot", required=True, help="JSON object with a values object")
    create.add_argument("--loader", required=True)
    create.add_argument("--kernel", required=True)
    create.add_argument("--image", required=True)
    create.add_argument("--medium-kind", required=True, choices=("USB", "SATA", "NETWORK", "OTHER"))
    create.add_argument("--medium-model", required=True, help="non-unique make/model only")
    create.add_argument("--medium-capacity", type=int)
    create.add_argument("--medium-vid-pid", help="optional vendor:product ID; do not include a serial")
    create.add_argument("--medium-verification", default="NOT TESTED", choices=("NOT TESTED", "VERIFIED", "FAILED"))
    create.add_argument("--boot-classification", required=True,
                        choices=("COLD", "WARM", "UNKNOWN"))
    create.add_argument("--build-option", action="append", default=[], metavar="NAME=VALUE")
    create.add_argument("--diagnostic-option", action="append", default=[], metavar="NAME=VALUE")
    create.add_argument("--expected-action", action="append", default=[])
    create.add_argument("--required-stage", action="append", default=[] , choices=STAGES)
    create.add_argument("--deferred-device", action="append", default=[])
    create.add_argument("--output", required=True)
    create.set_defaults(func=_cmd_create)

    update = sub.add_parser("update", help="write a new report revision with physical observations")
    update.add_argument("--report", required=True)
    update.add_argument("--repo", default=".")
    update.add_argument("--output", required=True)
    update.add_argument("--status", choices=("INCOMPLETE", "PASS", "FAIL"))
    update.add_argument("--stage", choices=STAGES)
    update.add_argument("--stage-status", choices=STATUSES)
    update.add_argument("--observation")
    update.add_argument("--failure-stage", choices=STAGES)
    update.add_argument("--confirm-physical", action="store_true")
    update.add_argument("--medium-verification", choices=("NOT TESTED", "VERIFIED", "FAILED"))
    update.add_argument("--embedded-match", choices=("NOT TESTED", "VERIFIED", "FAILED"))
    update.add_argument("--boot-classification", choices=("COLD", "WARM", "UNKNOWN"))
    update.add_argument("--evidence-root")
    update.add_argument("--evidence", action="append", default=[], metavar="KIND=PATH")
    update.add_argument("--evidence-source", choices=EVIDENCE_SOURCES, default="OPERATOR")
    update.add_argument("--evidence-description", default="Operator-supplied physical test evidence")
    update.set_defaults(func=_cmd_update)

    validate = sub.add_parser("validate", help="validate report schema, hashes, states, and evidence")
    validate.add_argument("--report", required=True)
    validate.add_argument("--repo", default=".")
    validate.add_argument("--evidence-root")
    validate.add_argument("--metadata-only", action="store_true", help="skip local file rehashing")
    validate.add_argument("--require-pass", action="store_true")
    validate.set_defaults(func=_cmd_validate)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        args.func(args)
    except (ReportError, OSError) as error:
        print(f"physical-qualification: FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
