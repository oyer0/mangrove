#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ownership-safe lifecycle for generated QEMU test artifacts."""
from datetime import datetime, timezone
import atexit
import fcntl
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import secrets
import threading

ROOT = Path(__file__).resolve().parents[2]
MARKER = ".mangrove-test-workspace.json"
IMAGE_SUFFIXES = {".img", ".qcow", ".qcow2", ".raw", ".ppm"}
SMALL_EVIDENCE_SUFFIXES = {".json", ".log", ".txt", ".tsv"}
MAX_DIAGNOSTIC_FILE = 8 * 1024 * 1024
MAX_DIAGNOSTIC_TOTAL = 32 * 1024 * 1024
MAX_FAILURE_MEDIA = 2
MAX_FAILURE_MEDIA_TOTAL = 256 * 1024 * 1024
MAX_FAILURE_SCREENSHOT_BYTES = 16 * 1024 * 1024
MAX_FAILURE_BUNDLES = 12
MAX_FAILURE_EVIDENCE_TOTAL = 1024 * 1024 * 1024
FAILURE_MARKER = ".mangrove-failure-evidence.json"
ORPHAN_REPORT = "orphaned-workspace.json"

_ACTIVE_WORKSPACES = set()
_PREVIOUS_SIGTERM = None


class InsufficientArtifactSpace(RuntimeError):
    """A test workspace cannot safely hold its declared disposable artifacts."""


def _workspace_sigterm(signum, frame):
    raise KeyboardInterrupt(f"received signal {signum}; stopping owned test workflow")


def check_free_space(path, required_bytes, purpose="test artifacts"):
    """Fail before allocation when the selected filesystem cannot fit the workload."""
    target = Path(path)
    target.mkdir(parents=True, exist_ok=True)
    available = shutil.disk_usage(target).free
    if required_bytes < 0:
        raise ValueError("required free space must not be negative")
    if available < required_bytes:
        raise InsufficientArtifactSpace(
            f"insufficient space for {purpose} in {target}: "
            f"need {required_bytes} bytes, have {available} bytes; "
            "select a disk-backed TMPDIR or build workspace with sufficient free space")
    return available


def add_retention_argument(parser):
    parser.add_argument(
        "--retain-artifacts", action="store_true",
        help="keep generated images and intermediate guest artifacts for debugging")


def terminate_and_reap(process, timeout=2):
    """Terminate and wait for an owned child before its files may be cleaned."""
    if process.poll() is None:
        try:
            process.terminate()
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                process.kill()
            except ProcessLookupError:
                pass
            process.wait()
    else:
        process.wait()
    return process.returncode


class ArtifactWorkspace:
    """A newly-created output root; cleanup is limited to its generated files."""

    def __init__(self, output=None, name="guest", retain=False):
        self.name = name
        self.temporary = output is None
        # An explicitly selected output is user-owned evidence. The runner
        # creates only its new leaf and never prunes it.
        self.retain = bool(retain or output is not None)
        self.token = secrets.token_hex(16)
        if self.temporary:
            configured_tmp = os.environ.get("TMPDIR")
            base = (Path(configured_tmp).expanduser() if configured_tmp else
                    ROOT / "build" / "test-workspaces")
            base.mkdir(parents=True, exist_ok=True)
            self.path = Path(tempfile.mkdtemp(
                prefix=f"mangrove-{name}-", dir=base)).resolve()
        else:
            self.path = Path(output).expanduser().resolve()
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self.path.mkdir(exist_ok=False)
        self.marker = self.path / MARKER
        with self.marker.open("x", encoding="utf-8") as stream:
            json.dump({"schema": 1, "token": self.token, "name": name,
                       "pid": os.getpid(),
                       "created_utc": datetime.now(timezone.utc).isoformat()}, stream)
            stream.write("\n")
        self._failure_media = []
        self._finished = False
        _register_workspace(self)

    def require_free_space(self, required_bytes, purpose="test artifacts"):
        """Check capacity on the workspace's filesystem before a large workload."""
        return check_free_space(self.path, required_bytes, purpose)

    def _record_unfinished(self):
        """Leave a small, explicit recovery marker; never reap an unknown guest."""
        if self._finished:
            return
        try:
            self._verify_owner()
            (self.path / ORPHAN_REPORT).write_text(json.dumps({
                "status": "INCOMPLETE",
                "reason": "runner exited before artifact cleanup completed",
                "workspace": str(self.path),
                "pid": os.getpid(),
                "safe_action": "inspect the owner marker and ensure child processes are stopped before manual removal",
            }, indent=2, sort_keys=True) + "\n")
        except (OSError, RuntimeError):
            # The marker itself still identifies the workspace if this small
            # best-effort report cannot be written.
            pass

    def set_failure_media(self, *paths):
        """Select only exact media needed to inspect the current failed phase."""
        selected = []
        for path in paths:
            if path is None:
                continue
            candidate = self._owned_path(path)
            selected.append(candidate)
        self._failure_media = selected

    def _owned_path(self, path):
        candidate = Path(path)
        if not candidate.is_absolute():
            candidate = self.path / candidate
        try:
            relative = candidate.relative_to(self.path)
        except ValueError as error:
            raise ValueError("artifact path must remain inside the owned workspace") from error
        current = self.path
        for part in relative.parts:
            current = current / part
            if current.is_symlink():
                raise ValueError("artifact path may not cross a workspace symlink")
        try:
            resolved = candidate.resolve(strict=False)
            resolved.relative_to(self.path)
        except (ValueError, OSError) as error:
            raise ValueError("artifact path must remain inside the owned workspace") from error
        return resolved

    def _verify_owner(self):
        if self.path.is_symlink() or not self.path.is_dir() or self.marker.is_symlink():
            raise RuntimeError("test output ownership marker is missing or unsafe")
        try:
            owner = json.loads(self.marker.read_text())
        except (OSError, ValueError) as error:
            raise RuntimeError("test output ownership marker is invalid") from error
        if (not isinstance(owner, dict) or owner.get("schema") != 1 or
                owner.get("token") != self.token or
                owner.get("name") != self.name):
            raise RuntimeError("test output ownership marker changed")

    def _is_disposable(self, path):
        return (path.suffix.lower() in IMAGE_SUFFIXES or
                path.name in {"vars.fd", "qmp.in", "qmp.out"})

    def _copy_failure_evidence(self, report, keep):
        base = ROOT / "build" / "test-failures"
        base.mkdir(parents=True, exist_ok=True)
        # Serialize publication and rotation so one runner cannot prune a
        # bundle another runner is still writing.
        with (base / ".retention.lock").open("a+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                return self._copy_failure_evidence_locked(base, report, keep)
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)

    def _copy_failure_evidence_locked(self, base, report, keep):
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        destination = base / f"{self.name}-{stamp}-{os.getpid()}-{self.token[:8]}"
        destination.mkdir(exist_ok=False)
        (destination / FAILURE_MARKER).write_text(json.dumps({
            "schema": 1, "token": self.token, "name": self.name,
        }, sort_keys=True) + "\n")
        report["failure_evidence"] = str(destination.relative_to(ROOT))
        total = 0
        omitted = []
        for source in sorted(self.path.rglob("*")):
            if source == self.marker or source.is_symlink() or not source.is_file():
                continue
            if source.name == "report.json" or source.suffix.lower() in SMALL_EVIDENCE_SUFFIXES:
                size = source.stat().st_size
                if size > MAX_DIAGNOSTIC_FILE or total + size > MAX_DIAGNOSTIC_TOTAL:
                    omitted.append(str(source.relative_to(self.path)))
                    continue
                target = destination / source.relative_to(self.path)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
                total += size
        kept_media = 0
        copied_media = []
        media_candidates = list(keep)
        # Preserve a small number of runner-captured framebuffer images on
        # failure even when a runner did not explicitly select one. QMP
        # screenshots are bounded by the guest runner; larger media remains
        # opt-in through set_failure_media().
        for screenshot in sorted(self.path.rglob("*.ppm")):
            if (screenshot.is_file() and not screenshot.is_symlink() and
                    screenshot.stat().st_size <= MAX_FAILURE_SCREENSHOT_BYTES and
                    screenshot not in media_candidates):
                media_candidates.append(screenshot)
        for source in media_candidates:
            if not source.is_file() or source.is_symlink():
                continue
            size = source.stat().st_size
            if (kept_media >= MAX_FAILURE_MEDIA or
                    total + size > MAX_FAILURE_MEDIA_TOTAL):
                omitted.append(str(source.relative_to(self.path)))
                continue
            target = destination / source.relative_to(self.path)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            total += size
            kept_media += 1
            copied_media.append(str(target.relative_to(ROOT)))
        report["retained_artifacts"] = copied_media
        report["artifact_cleanup"]["retained_media"] = copied_media
        if omitted:
            (destination / "omitted-evidence.txt").write_text(
                "Evidence omitted by configured size/count bounds:\n" +
                "\n".join(omitted) + "\n")
        report["failure_evidence_bytes"] = total
        self._prune_failure_evidence(base, destination)
        return destination

    @staticmethod
    def _prune_failure_evidence(base, current):
        """Bound only bundles carrying this helper's ownership marker."""
        owned = []
        for path in base.iterdir():
            if not path.is_dir() or path.is_symlink():
                continue
            marker = path / FAILURE_MARKER
            if marker.is_symlink():
                continue
            try:
                metadata = json.loads(marker.read_text())
                if (metadata.get("schema") != 1 or not metadata.get("token") or
                        not metadata.get("name")):
                    continue
                size = sum(item.stat().st_size for item in path.rglob("*")
                           if item.is_file() and not item.is_symlink())
            except (OSError, ValueError):
                continue
            owned.append((path.stat().st_mtime_ns, path, size))
        owned.sort()
        total = sum(item[2] for item in owned)
        while len(owned) > MAX_FAILURE_BUNDLES or total > MAX_FAILURE_EVIDENCE_TOTAL:
            removable = next((i for i, item in enumerate(owned)
                              if item[1] != current), None)
            if removable is None:
                break
            _, path, size = owned.pop(removable)
            shutil.rmtree(path)
            total -= size

    def finish(self, report, verified=False):
        """Record outcome, clean disposable files, then discard a private workspace."""
        self._verify_owner()
        status = report.get("status")
        completed = status in {"PASS", "EXPECTED_FAILURE"}
        if completed and not verified:
            raise RuntimeError("successful artifact cleanup requires completed verification")
        before = sum(p.stat().st_size for p in self.path.rglob("*")
                     if p.is_file() and not p.is_symlink())
        keep = list(self._failure_media) if not completed else []
        retained_names = set(report.get("retained_artifacts", []))
        for name in retained_names:
            candidate = self._owned_path(name)
            if candidate not in keep:
                keep.append(candidate)
        if not completed:
            for screenshot in sorted(self.path.rglob("*.ppm")):
                if (screenshot.is_file() and not screenshot.is_symlink() and
                        screenshot.stat().st_size <= MAX_FAILURE_SCREENSHOT_BYTES and
                        screenshot not in keep):
                    keep.append(screenshot)
        # Parent orchestrators own nested child workspaces. Honor child
        # reports that identify exact media needed to diagnose uncertainty.
        for child_report in self.path.rglob("report.json"):
            if child_report == self.path / "report.json" or child_report.is_symlink():
                continue
            try:
                child_data = json.loads(child_report.read_text())
            except (OSError, ValueError):
                continue
            for name in child_data.get("retained_artifacts", []):
                candidate = (child_report.parent / name).resolve(strict=False)
                try:
                    candidate.relative_to(self.path)
                except ValueError:
                    continue
                if candidate not in keep:
                    keep.append(candidate)
        retained = []
        retained_root = None
        if self.temporary and not self.retain and completed and keep:
            base = ROOT / "build" / "test-retained"
            base.mkdir(parents=True, exist_ok=True)
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            retained_root = base / f"{self.name}-{stamp}-{os.getpid()}-{self.token[:8]}"
            retained_root.mkdir(exist_ok=False)
            kept_paths = []
            for source in keep:
                source = source.resolve(strict=False)
                if (not source.is_file() or source.is_symlink() or
                        source == self.marker or
                        self._is_disposable(source) is False):
                    continue
                relative = source.relative_to(self.path)
                target = retained_root / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                os.replace(source, target)
                kept_paths.append(str(target.relative_to(ROOT)))
            report["retained_artifacts"] = kept_paths
            report["retained_output"] = str(retained_root.relative_to(ROOT))
            retained = kept_paths
        removed = 0
        removed_bytes = 0
        errors = []
        if not self.retain:
            keep_set = {path.absolute() for path in keep}
            for path in sorted(self.path.rglob("*"), reverse=True):
                if path == self.marker or path.is_symlink() or not path.is_file():
                    continue
                if not self._is_disposable(path) or path.absolute() in keep_set:
                    if self._is_disposable(path) and path.absolute() in keep_set:
                        retained.append(str(path.relative_to(self.path)))
                    continue
                try:
                    size = path.stat().st_size
                    path.unlink()
                    removed += 1
                    removed_bytes += size
                except OSError as error:
                    errors.append(f"{path.relative_to(self.path)}: {error}")
        report["artifact_cleanup"] = {
            "mode": "retained-by-request" if self.retain else
                    ("temporary-workspace" if self.temporary else "owned-output"),
            "independent_verification_complete": bool(verified),
            "removed_disposable_files": removed,
            "removed_disposable_bytes": removed_bytes,
            "retained_media": retained,
            "errors": errors,
        }
        if errors and completed:
            report["status"] = "FAIL"
            report["error"] = "could not remove all disposable test artifacts"
            completed = False
            keep = list(self._failure_media)
        report["artifact_bytes_before_cleanup"] = before
        report["artifact_bytes"] = sum(p.stat().st_size for p in self.path.rglob("*")
                                       if p.is_file() and not p.is_symlink())
        destination = None
        if self.temporary and self.retain:
            base = ROOT / "build" / "test-retained"
            base.mkdir(parents=True, exist_ok=True)
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            destination = base / f"{self.name}-{stamp}-{os.getpid()}-{self.token[:8]}"
            self.path.rename(destination)
            self.path = destination
            self.marker = self.path / MARKER
            report["retained_output"] = str(destination.relative_to(ROOT))
        elif self.temporary and not completed:
            destination = self._copy_failure_evidence(report, keep)
        if self.temporary and not self.retain:
            report["temporary_output_removed"] = True
        report_path = self.path / "report.json"
        report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        if self.temporary and not self.retain:
            if destination is not None:
                shutil.copy2(report_path, destination / "report.json")
            if retained_root is not None:
                shutil.copy2(report_path, retained_root / "report.json")
            self.marker.unlink(missing_ok=True)
            shutil.rmtree(self.path)
        else:
            self.marker.unlink(missing_ok=True)
        self._finished = True
        _unregister_workspace(self)
        return report


def _register_workspace(workspace):
    global _PREVIOUS_SIGTERM
    _ACTIVE_WORKSPACES.add(workspace)
    atexit.register(workspace._record_unfinished)
    # Only the main Python thread may install signal handlers. A workspace
    # created by a worker thread is still uniquely owned and cleaned normally.
    if threading.current_thread() is threading.main_thread():
        if _PREVIOUS_SIGTERM is None:
            _PREVIOUS_SIGTERM = signal.getsignal(signal.SIGTERM)
            signal.signal(signal.SIGTERM, _workspace_sigterm)


def _unregister_workspace(workspace):
    global _PREVIOUS_SIGTERM
    if workspace not in _ACTIVE_WORKSPACES:
        return
    _ACTIVE_WORKSPACES.remove(workspace)
    atexit.unregister(workspace._record_unfinished)
    if not _ACTIVE_WORKSPACES and _PREVIOUS_SIGTERM is not None:
        if (threading.current_thread() is threading.main_thread() and
                signal.getsignal(signal.SIGTERM) is _workspace_sigterm):
            signal.signal(signal.SIGTERM, _PREVIOUS_SIGTERM)
        _PREVIOUS_SIGTERM = None
