#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic report-generation and physical-evidence validator controls."""

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/physical_qualification.py"
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("physical_qualification", TOOL)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


def git(repo, *args):
    return subprocess.run(["git", *args], cwd=repo, check=True,
                          capture_output=True, text=True).stdout.strip()


def rejected(report, repo, evidence, phrase=None):
    try:
        tool.validate_report(report, repo, evidence, verify_files=True)
    except tool.ReportError as error:
        if phrase:
            assert phrase.lower() in str(error).lower(), str(error)
        return
    raise AssertionError("invalid physical qualification report was accepted")


with tempfile.TemporaryDirectory(prefix="mangrove-physical-report-") as directory:
    temp = Path(directory)
    repo = temp / "repo"
    repo.mkdir()
    (repo / "boot").mkdir()
    (repo / "kernel").mkdir()
    (repo / "docs").mkdir()
    (repo / "build").mkdir()
    (repo / "boot/BOOTX64.EFI").write_bytes(b"synthetic loader bytes\n")
    (repo / "kernel/pith.elf").write_bytes(b"synthetic Pith bytes\n")
    (repo / "build/MangroveUSB.img").write_bytes(b"synthetic image bytes\n")
    (repo / "docs/profile.md").write_text("Synthetic test profile; not hardware evidence.\n")
    git(repo, "init", "-q")
    (repo / ".git/info/exclude").write_text("/build/physical-tests/\n")
    git(repo, "-c", "user.name=Report Fixture", "-c", "user.email=fixture@example.invalid",
        "add", ".")
    git(repo, "-c", "user.name=Report Fixture", "-c", "user.email=fixture@example.invalid",
        "commit", "-qm", "fixture")

    firmware = temp / "firmware.json"
    firmware.write_text(json.dumps({"source": "synthetic fixture", "values": {
        "bios_version": "fixture", "secure_boot": "unknown"}}))
    report_path = temp / "reports/first/report.json"
    command = [sys.executable, str(TOOL), "create", "--repo", str(repo),
        "--profile-id", "synthetic-test-machine", "--profile", "docs/profile.md",
        "--firmware-snapshot", str(firmware), "--loader", "boot/BOOTX64.EFI",
        "--kernel", "kernel/pith.elf", "--image", "build/MangroveUSB.img",
        "--medium-kind", "USB", "--medium-model", "synthetic removable test device",
        "--medium-capacity", "1048576", "--boot-classification", "COLD",
        "--build-option", "SAMPLE_BUILD_OPTION=off", "--diagnostic-option", "overlay=disabled",
        "--expected-action", "Reach root-mounted Shoot prompt",
        "--expected-action", "Run a bounded shell workload",
        "--required-stage", "root_mount", "--required-stage", "shoot_input",
        "--required-stage", "test_workload", "--deferred-device", "optional class device marked deferred",
        "--output", str(report_path)]
    created = subprocess.run(command, capture_output=True, text=True)
    assert created.returncode == 0, created.stderr
    base = json.loads(report_path.read_text())
    assert base["test_result"]["status"] == "NOT TESTED"
    assert base["test_result"]["physical_confirmation"]["confirmed"] is False
    assert base["source_identity"]["revision"] == git(repo, "rev-parse", "HEAD")
    assert base["build_artifacts"]["image"]["sha256"] == hashlib.sha256(
        (repo / "build/MangroveUSB.img").read_bytes()).hexdigest()
    unknown_boot_classification = copy.deepcopy(base)
    unknown_boot_classification["configuration"]["boot_classification"] = "UNKNOWN"
    tool.validate_report(unknown_boot_classification, repo)
    invalid_boot_classification = copy.deepcopy(base)
    invalid_boot_classification["configuration"]["boot_classification"] = "UNSURE"
    rejected(invalid_boot_classification, repo, None, "COLD, WARM, or UNKNOWN")
    ignored_output = repo / "build/physical-tests/ignored-report.json"
    ignored_command = [*command[:-1], str(ignored_output)]
    ignored = subprocess.run(ignored_command, capture_output=True, text=True)
    assert ignored.returncode == 0, ignored.stderr
    ignored_report = json.loads(ignored_output.read_text())
    tool.validate_report(ignored_report, repo)

    evidence = temp / "evidence"
    evidence.mkdir()
    (evidence / "flash-transcript.txt").write_text(
        "Synthetic fixture only; not a real USB flash or verification.\n")
    (evidence / "screen-note.txt").write_text(
        "Synthetic fixture only; a physical operator would record the visible prompt.\n")
    evidence_digest = hashlib.sha256((evidence / "screen-note.txt").read_bytes()).hexdigest()
    def successful_report():
        report = copy.deepcopy(base)
        report["boot_medium"]["write_verification"] = "VERIFIED"
        report["test_result"].update(status="PASS", history=["NOT TESTED", "INCOMPLETE", "PASS"],
            physical_confirmation={"confirmed": True, "confirmed_utc": "2026-01-01T00:00:00Z"})
        for stage in report["required_stages"]:
            report["stage_results"][stage]["status"] = "PASS"
            report["stage_results"][stage]["observations"].append(
                {"source": "OPERATOR", "text": "Synthetic positive-control observation"})
        report["evidence"] = [{"kind": "transcript", "path": "flash-transcript.txt",
            "size_bytes": (evidence / "flash-transcript.txt").stat().st_size,
            "sha256": hashlib.sha256((evidence / "flash-transcript.txt").read_bytes()).hexdigest(),
            "source": "AUTOMATED", "description": "Synthetic flash-usb.sh write verification transcript"},
            {"kind": "other", "path": "screen-note.txt",
            "size_bytes": (evidence / "screen-note.txt").stat().st_size,
            "sha256": evidence_digest, "source": "OPERATOR",
            "description": "Synthetic fixture evidence; not a physical result"}]
        return report

    # Structurally valid positive control; deferred optional hardware does not block.
    passed = successful_report()
    tool.validate_report(passed, repo, evidence)
    assert passed["deferred_devices"] == ["optional class device marked deferred"]

    # Exercise the normal immutable report-revision flow through the CLI.
    revisions = [temp / f"reports/revision-{number}.json" for number in range(1, 5)]
    updates = [
        ["--status", "INCOMPLETE", "--stage", "root_mount", "--stage-status", "PASS",
         "--observation", "Synthetic root-mount checkpoint", "--medium-verification", "VERIFIED",
         "--boot-classification", "UNKNOWN",
         "--evidence", "transcript=flash-transcript.txt",
         "--evidence-source", "AUTOMATED", "--evidence-description",
         "Synthetic flash-usb.sh write verification transcript"],
        ["--stage", "shoot_input", "--stage-status", "PASS", "--observation",
         "Synthetic interactive-input checkpoint"],
        ["--stage", "test_workload", "--stage-status", "PASS", "--observation",
         "Synthetic bounded-command checkpoint"],
        ["--status", "PASS", "--confirm-physical", "--evidence-root", str(evidence),
         "--evidence", "other=screen-note.txt", "--evidence-description",
         "Synthetic operator observation file; not physical evidence"],
    ]
    previous = report_path
    for index, args in enumerate(updates):
        result = subprocess.run([sys.executable, str(TOOL), "update", "--repo", str(repo),
            "--report", str(previous), "--output", str(revisions[index]),
            "--evidence-root", str(evidence), *args],
            # Every revision verifies previously attached evidence before copying it forward.
            capture_output=True, text=True)
        assert result.returncode == 0, result.stderr + "\n" + json.dumps(
            json.loads(previous.read_text())["stage_results"], sort_keys=True)
        previous = revisions[index]
        if index == 0:
            assert json.loads(previous.read_text())["configuration"][
                "boot_classification"] == "UNKNOWN"
    result = subprocess.run([sys.executable, str(TOOL), "validate", "--repo", str(repo),
        "--report", str(previous), "--evidence-root", str(evidence), "--require-pass"],
        capture_output=True, text=True)
    assert result.returncode == 0 and "physical result=PASS" in result.stdout, result.stderr

    # A boot that fails at device discovery is reportable and remains a FAIL.
    failed = copy.deepcopy(base)
    failed["test_result"].update(status="FAIL", history=["NOT TESTED", "INCOMPLETE", "FAIL"],
        failure_stage="device_discovery",
        physical_confirmation={"confirmed": True, "confirmed_utc": "2026-01-01T00:00:00Z"})
    failed["stage_results"]["device_discovery"].update(status="FAIL", observations=[
        {"source": "OPERATOR", "text": "Synthetic failure-stage observation"}])
    failed["evidence"] = copy.deepcopy(passed["evidence"])
    tool.validate_report(failed, repo, evidence)
    wrong_failure = copy.deepcopy(failed)
    wrong_failure["test_result"]["failure_stage"] = "root_mount"
    rejected(wrong_failure, repo, evidence, "matching failed stage")

    # Direct PASS transition and post-terminal transitions are rejected.
    direct_pass = copy.deepcopy(passed)
    direct_pass["test_result"]["history"] = ["NOT TESTED", "PASS"]
    rejected(direct_pass, repo, evidence, "INCOMPLETE")
    after_terminal = copy.deepcopy(passed)
    after_terminal["test_result"]["history"].append("FAIL")
    after_terminal["test_result"]["status"] = "FAIL"
    rejected(after_terminal, repo, evidence, "terminal")

    # No physical confirmation and no operator evidence can be replaced by KVM evidence.
    no_confirmation = copy.deepcopy(passed)
    no_confirmation["test_result"]["physical_confirmation"] = {"confirmed": False, "confirmed_utc": None}
    rejected(no_confirmation, repo, evidence, "operator confirmation")
    missing_stage_observation = copy.deepcopy(passed)
    missing_stage_observation["stage_results"]["shoot_input"]["observations"] = []
    rejected(missing_stage_observation, repo, evidence, "operator observation for every required stage")
    automated_only = copy.deepcopy(passed)
    for item in automated_only["evidence"]:
        item["source"] = "AUTOMATED"
    rejected(automated_only, repo, evidence, "operator-supplied evidence")

    # Missing and altered artifact/evidence hashes, absent references, and bad snapshot hashes fail.
    missing_hash = copy.deepcopy(base)
    del missing_hash["build_artifacts"]["kernel"]["sha256"]
    rejected(missing_hash, repo, evidence, "SHA-256")
    changed_hash = copy.deepcopy(base)
    changed_hash["build_artifacts"]["image"]["sha256"] = "0" * 64
    rejected(changed_hash, repo, evidence, "changed")
    missing_evidence = copy.deepcopy(passed)
    missing_evidence["evidence"][0]["path"] = "missing.jpg"
    rejected(missing_evidence, repo, evidence, "missing")
    altered_evidence = copy.deepcopy(passed)
    altered_evidence["evidence"][0]["sha256"] = "0" * 64
    rejected(altered_evidence, repo, evidence, "does not match")
    oversized_evidence = copy.deepcopy(passed)
    oversized_evidence["evidence"][0]["size_bytes"] = tool.MAX_EVIDENCE_FILE_BYTES + 1
    rejected(oversized_evidence, repo, evidence, "size is invalid")
    outside = temp / "outside-evidence.txt"
    outside.write_text("must not be accepted from outside evidence root\n")
    (evidence / "escape.txt").symlink_to(outside)
    escaped_evidence = copy.deepcopy(passed)
    escaped_evidence["evidence"][0]["path"] = "escape.txt"
    rejected(escaped_evidence, repo, evidence, "escapes")
    bad_firmware = copy.deepcopy(base)
    bad_firmware["firmware_snapshot"]["values"]["secure_boot"] = "enabled"
    rejected(bad_firmware, repo, evidence, "firmware snapshot hash")
    missing_firmware_version = copy.deepcopy(base)
    missing_firmware_version["firmware_snapshot"]["values"].pop("bios_version")
    missing_firmware_version["firmware_snapshot"]["values_sha256"] = hashlib.sha256(
        tool._canonical_json(missing_firmware_version["firmware_snapshot"]["values"])).hexdigest()
    rejected(missing_firmware_version, repo, evidence, "bios_version or firmware_version")
    stale_source = copy.deepcopy(base)
    stale_source["source_identity"]["tracked_diff_sha256"] = "0" * 64
    rejected(stale_source, repo, evidence, "working-tree identity")
    stale_profile = copy.deepcopy(base)
    stale_profile["hardware_profile"]["sha256"] = "0" * 64
    rejected(stale_profile, repo, evidence, "hardware profile hash")

    private_snapshot = copy.deepcopy(base)
    private_snapshot["firmware_snapshot"]["values"]["serial_number"] = "synthetic"
    rejected(private_snapshot, repo, evidence, "disallowed")

    # A report with successful flash verification must cite an automated flash transcript.
    flash_verified = copy.deepcopy(base)
    flash_verified["boot_medium"]["write_verification"] = "VERIFIED"
    flash_verified["evidence"] = [{"kind": "transcript", "path": "flash-transcript.txt",
        "size_bytes": (evidence / "flash-transcript.txt").stat().st_size,
        "sha256": hashlib.sha256((evidence / "flash-transcript.txt").read_bytes()).hexdigest(),
        "source": "AUTOMATED",
        "description": "Synthetic flash utility write verification transcript"}]
    tool.validate_report(flash_verified, repo, evidence)
    flash_unverified = copy.deepcopy(flash_verified)
    flash_unverified["evidence"] = []
    rejected(flash_unverified, repo, evidence, "flash-verification")
    embedded_match = copy.deepcopy(base)
    embedded_match["build_artifacts"]["embedded_loader_kernel_match"] = "VERIFIED"
    embedded_match["evidence"] = [{"kind": "host_report", "path": "screen-note.txt",
        "size_bytes": (evidence / "screen-note.txt").stat().st_size,
        "sha256": evidence_digest, "source": "AUTOMATED",
        "description": "Synthetic embedded image identity report"}]
    tool.validate_report(embedded_match, repo, evidence)
    embedded_without_report = copy.deepcopy(embedded_match)
    embedded_without_report["evidence"] = []
    rejected(embedded_without_report, repo, evidence, "automated host-report")

    # An incomplete recovery attempt blocks PASS when recovery is required.
    recovery_gap = successful_report()
    recovery_gap["required_stages"].append("shutdown_recovery")
    recovery_gap["stage_results"]["shutdown_recovery"].update(status="INCOMPLETE", observations=[
        {"source": "OPERATOR", "text": "Synthetic incomplete recovery attempt"}])
    rejected(recovery_gap, repo, evidence, "required stage")

    # A declared deferred peripheral does not turn an otherwise valid report into a blocker.
    optional = successful_report()
    optional["deferred_devices"].append("another optional peripheral class intentionally deferred")
    tool.validate_report(optional, repo, evidence)

    # The CLI refuses a direct NOT TESTED -> PASS transition and preserves its output path.
    direct_output = temp / "reports/direct.json"
    direct = subprocess.run([sys.executable, str(TOOL), "update", "--repo", str(repo),
        "--report", str(report_path), "--output", str(direct_output), "--status", "PASS",
        "--confirm-physical"], capture_output=True, text=True)
    assert direct.returncode != 0 and not direct_output.exists()
    prior_bytes = report_path.read_bytes()
    overwrite = subprocess.run(command, capture_output=True, text=True)
    assert overwrite.returncode != 0 and report_path.read_bytes() == prior_bytes
    tracked_tree_output = repo / "docs/report.json"
    tracked_tree_command = [*command[:-1], str(tracked_tree_output)]
    tracked_tree = subprocess.run(tracked_tree_command, capture_output=True, text=True)
    assert tracked_tree.returncode != 0 and not tracked_tree_output.exists()
    assert "must be ignored" in tracked_tree.stderr

    # A required recovery stage left untested also prevents an overall PASS.
    recovery_not_tested = successful_report()
    recovery_not_tested["required_stages"].append("shutdown_recovery")
    rejected(recovery_not_tested, repo, evidence, "required stage")

    # Dirty/untracked source is recorded; later source changes invalidate the record.
    (repo / "draft-source.txt").write_text("uncommitted source fixture\n")
    dirty_output = temp / "reports/dirty/report.json"
    dirty_command = [*command[:-1], str(dirty_output)]
    dirty_create = subprocess.run(dirty_command, capture_output=True, text=True)
    assert dirty_create.returncode == 0, dirty_create.stderr
    dirty_report = json.loads(dirty_output.read_text())
    assert dirty_report["source_identity"]["worktree_dirty"]
    assert dirty_report["source_identity"]["untracked_file_count"] == 1
    tool.validate_report(dirty_report, repo, evidence)
    (repo / "draft-source.txt").write_text("changed uncommitted source fixture\n")
    rejected(dirty_report, repo, evidence, "working-tree identity")

print("PASS: physical report create/validate; positive, failed-boot, source/artifact/evidence,")
print("      confirmation, transition, recovery, and deferred-device controls")
