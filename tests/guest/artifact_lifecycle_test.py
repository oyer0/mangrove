#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Focused ownership and cleanup tests for generated guest artifacts."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock

from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1] / "shared"))
import artifact_lifecycle as lifecycle


class ArtifactLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="mangrove-artifact-test-")
        self.root = Path(self.temporary.name)
        self.old_root = lifecycle.ROOT
        self.old_tmpdir = os.environ.get("TMPDIR")
        lifecycle.ROOT = self.root
        test_tmpdir = self.root / "tmp"
        test_tmpdir.mkdir()
        os.environ["TMPDIR"] = str(test_tmpdir)

    def tearDown(self):
        lifecycle.ROOT = self.old_root
        if self.old_tmpdir is None:
            os.environ.pop("TMPDIR", None)
        else:
            os.environ["TMPDIR"] = self.old_tmpdir
        self.temporary.cleanup()

    def workspace(self, name="fixture"):
        return lifecycle.ArtifactWorkspace(name=name)

    def test_success_cleans_only_after_verification(self):
        workspace = self.workspace()
        image = workspace.path / "disk.img"
        image.write_bytes(b"image")
        (workspace.path / "serial.log").write_text("independent checker passed\n")

        with self.assertRaisesRegex(RuntimeError, "verification"):
            workspace.finish({"status": "PASS"}, verified=False)
        self.assertTrue(image.exists())

        report = workspace.finish({"status": "PASS"}, verified=True)
        self.assertFalse(workspace.path.exists())
        self.assertEqual(report["artifact_cleanup"]["removed_disposable_files"], 1)
        self.assertEqual(report["artifact_cleanup"]["removed_disposable_bytes"], 5)

    def test_failure_keeps_concise_diagnostics_and_drops_image(self):
        workspace = self.workspace("failed")
        (workspace.path / "disk.img").write_bytes(b"large media")
        (workspace.path / "serial.log").write_text("guest failed at phase verify\n")
        (workspace.path / "screen.ppm").write_bytes(b"bounded framebuffer evidence")
        report = workspace.finish({"status": "FAIL", "error": "checker mismatch"})
        evidence = self.root / report["failure_evidence"]
        self.assertEqual((evidence / "serial.log").read_text(),
                         "guest failed at phase verify\n")
        saved_report = json.loads((evidence / "report.json").read_text())
        self.assertEqual(saved_report["error"], "checker mismatch")
        self.assertFalse((evidence / "disk.img").exists())
        self.assertEqual((evidence / "screen.ppm").read_bytes(),
                         b"bounded framebuffer evidence")
        self.assertFalse(workspace.path.exists())

    def test_marked_uncertain_media_is_retained_alone(self):
        workspace = self.workspace("uncertain")
        selected = workspace.path / "uncertain-secondary.img"
        selected.write_bytes(b"only image needed to inspect uncertain state")
        (workspace.path / "temporary-snapshot.img").write_bytes(b"drop")
        (workspace.path / "checker.log").write_text("checker completed\n")

        report = workspace.finish({
            "status": "PASS", "retained_artifacts": [selected.name],
        }, verified=True)
        retained = self.root / report["retained_artifacts"][0]
        self.assertEqual(retained.read_bytes(),
                         b"only image needed to inspect uncertain state")
        self.assertTrue((retained.parent / "report.json").is_file())
        self.assertFalse(workspace.path.exists())
        self.assertFalse((self.root / "build/test-retained" /
                          report["retained_output"].split("/")[-1] /
                          "temporary-snapshot.img").exists())

    def test_nested_failed_media_is_retained_by_relative_path(self):
        workspace = self.workspace("nested-failure")
        phase = workspace.path / "mgfs-0" / "create"
        phase.mkdir(parents=True)
        selected = phase / "disk.img"
        selected.write_bytes(b"phase state")
        (phase / "serial.log").write_text("child failed with status 43\n")
        report = workspace.finish({
            "status": "FAIL", "error": "child status 43",
            "retained_artifacts": ["mgfs-0/create/disk.img"],
        })
        evidence = self.root / report["failure_evidence"]
        self.assertIn("child failed with status 43",
                      (evidence / "mgfs-0/create/serial.log").read_text())
        self.assertTrue((evidence / "mgfs-0/create/disk.img").is_file())
        self.assertEqual((evidence / "mgfs-0/create/disk.img").read_bytes(),
                         b"phase state")
        self.assertEqual(report["retained_artifacts"], [
            str((evidence / "mgfs-0/create/disk.img").relative_to(self.root))])
        self.assertFalse((self.root / "build/test-retained").exists())
        self.assertFalse(workspace.path.exists())

    def test_explicit_retention_and_existing_output_safety(self):
        output = self.root / "requested-output"
        workspace = lifecycle.ArtifactWorkspace(output=output, name="explicit")
        image = output / "disk.img"
        image.write_bytes(b"requested evidence")
        workspace.finish({"status": "PASS"}, verified=True)
        self.assertTrue(image.exists())

        existing = self.root / "user-data"
        existing.mkdir()
        sentinel = existing / "keep.txt"
        sentinel.write_text("user file")
        with self.assertRaises(FileExistsError):
            lifecycle.ArtifactWorkspace(output=existing, name="unsafe")
        self.assertEqual(sentinel.read_text(), "user file")

    def test_retained_paths_cannot_cross_workspace_symlinks(self):
        workspace = self.workspace("symlink")
        outside = self.root / "outside.img"
        outside.write_bytes(b"outside data")
        (workspace.path / "alias.img").symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "symlink"):
            workspace.set_failure_media(workspace.path / "alias.img")
        with self.assertRaisesRegex(ValueError, "symlink"):
            workspace.finish({
                "status": "PASS", "retained_artifacts": ["alias.img"],
            }, verified=True)
        self.assertEqual(outside.read_bytes(), b"outside data")

    def test_failure_bundle_retention_is_bounded_and_owned(self):
        failures = self.root / "build/test-failures"
        unrelated = failures / "user-owned"
        unrelated.mkdir(parents=True)
        (unrelated / "keep.txt").write_text("keep")
        for index in range(lifecycle.MAX_FAILURE_BUNDLES + 3):
            workspace = self.workspace(f"failure-{index}")
            (workspace.path / "runner.log").write_text(f"failure {index}\n")
            workspace.finish({"status": "FAIL", "error": f"failure {index}"})
        owned = [path for path in failures.iterdir()
                 if path.is_dir() and (path / lifecycle.FAILURE_MARKER).is_file()]
        self.assertEqual(len(owned), lifecycle.MAX_FAILURE_BUNDLES)
        self.assertEqual((unrelated / "keep.txt").read_text(), "keep")
        total = sum(path.stat().st_size for directory in owned
                    for path in directory.rglob("*") if path.is_file())
        self.assertLessEqual(total, lifecycle.MAX_FAILURE_EVIDENCE_TOTAL)

    def test_concurrent_workspaces_are_isolated(self):
        first = self.workspace("first")
        second = self.workspace("second")
        first_image = first.path / "one.img"
        second_image = second.path / "two.img"
        first_image.write_bytes(b"1")
        second_image.write_bytes(b"2")
        self.assertNotEqual(first.path, second.path)

        first.finish({"status": "PASS"}, verified=True)
        self.assertTrue(second_image.exists())
        second.finish({"status": "PASS"}, verified=True)
        self.assertFalse(second.path.exists())

    def test_explicit_tmpdir_is_honored_and_default_stays_in_build(self):
        selected = self.root / "selected-tmp"
        selected.mkdir()
        sentinel = selected / "caller-owned.txt"
        sentinel.write_text("leave the TMPDIR root alone\n")
        with mock.patch.dict(os.environ, {"TMPDIR": str(selected)}):
            workspace = self.workspace("tmpdir")
            self.assertEqual(workspace.path.parent, selected.resolve())
            workspace.finish({"status": "PASS"}, verified=True)
            self.assertEqual(list(selected.iterdir()), [sentinel])
            self.assertEqual(sentinel.read_text(), "leave the TMPDIR root alone\n")

        with mock.patch.dict(os.environ):
            os.environ.pop("TMPDIR", None)
            workspace = self.workspace("default-tmpdir")
            self.assertEqual(workspace.path.parent,
                             (self.root / "build" / "test-workspaces").resolve())
            workspace.finish({"status": "PASS"}, verified=True)

    def test_free_space_admission_reports_selected_filesystem(self):
        workspace = self.workspace("capacity")
        with mock.patch.object(lifecycle.shutil, "disk_usage",
                               return_value=mock.Mock(free=4096)):
            self.assertEqual(workspace.require_free_space(4096, "fixture media"), 4096)
            with self.assertRaisesRegex(lifecycle.InsufficientArtifactSpace,
                                        "need 4097 bytes, have 4096 bytes"):
                workspace.require_free_space(4097, "fixture media")
        workspace.finish({"status": "PASS"}, verified=True)

    def test_sigterm_runs_runner_cleanup_and_retains_failure_evidence(self):
        script = textwrap.dedent("""
            import sys, time
            from pathlib import Path
            sys.path.insert(0, sys.argv[1])
            import artifact_lifecycle as lifecycle
            lifecycle.ROOT = Path(sys.argv[2])
            workspace = lifecycle.ArtifactWorkspace(name='interrupted')
            (workspace.path / 'serial.log').write_text('stopped by SIGTERM\\n')
            print(workspace.path, flush=True)
            try:
                while True:
                    time.sleep(1)
            finally:
                workspace.finish({'status': 'INCOMPLETE', 'error': 'interrupted'})
        """)
        child = subprocess.Popen(
            [sys.executable, "-c", script,
             str(Path(lifecycle.__file__).resolve().parent), str(self.root)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        path = Path(child.stdout.readline().strip())
        self.assertTrue((path / lifecycle.MARKER).is_file())
        child.send_signal(signal.SIGTERM)
        child.communicate(timeout=10)
        self.assertNotEqual(child.returncode, 0)
        self.assertFalse(path.exists())
        evidence = list((self.root / "build" / "test-failures").glob("interrupted-*"))
        self.assertEqual(len(evidence), 1)
        self.assertEqual((evidence[0] / "serial.log").read_text(), "stopped by SIGTERM\n")
        report = json.loads((evidence[0] / "report.json").read_text())
        self.assertEqual(report["status"], "INCOMPLETE")

    def test_unhandled_interruption_leaves_identifiable_orphan_not_deleted(self):
        script = textwrap.dedent("""
            import sys, time
            from pathlib import Path
            sys.path.insert(0, sys.argv[1])
            import artifact_lifecycle as lifecycle
            lifecycle.ROOT = Path(sys.argv[2])
            workspace = lifecycle.ArtifactWorkspace(name='orphan')
            (workspace.path / 'owned.img').write_bytes(b'do not delete while child state is unknown')
            print(workspace.path, flush=True)
            while True:
                time.sleep(1)
        """)
        child = subprocess.Popen(
            [sys.executable, "-c", script,
             str(Path(lifecycle.__file__).resolve().parent), str(self.root)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        path = Path(child.stdout.readline().strip())
        child.send_signal(signal.SIGTERM)
        child.communicate(timeout=10)
        self.assertNotEqual(child.returncode, 0)
        self.assertTrue((path / lifecycle.MARKER).is_file())
        orphan = json.loads((path / lifecycle.ORPHAN_REPORT).read_text())
        self.assertEqual(orphan["status"], "INCOMPLETE")
        self.assertTrue((path / "owned.img").is_file())

    def test_concurrent_failure_bundles_are_not_pruned_mid_write(self):
        from concurrent.futures import ThreadPoolExecutor

        def fail(index):
            workspace = self.workspace(f"parallel-{index}")
            (workspace.path / "runner.log").write_text(f"failure {index}\n")
            return workspace.finish({"status": "FAIL", "error": "fixture"})

        with ThreadPoolExecutor(max_workers=8) as executor:
            reports = list(executor.map(fail, range(8)))
        for report in reports:
            evidence = self.root / report["failure_evidence"]
            self.assertTrue((evidence / "runner.log").is_file())
            self.assertTrue((evidence / "report.json").is_file())

    def test_timeout_terminates_and_reaps_before_cleanup(self):
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        workspace = self.workspace("timeout")
        image = workspace.path / "disk.img"
        image.write_bytes(b"not in use after reap")
        returncode = lifecycle.terminate_and_reap(child, timeout=1)
        self.assertIsNotNone(child.poll())
        self.assertNotEqual(returncode, 0)
        workspace.finish({"status": "INCOMPLETE", "error": "guest timeout"})
        self.assertFalse(workspace.path.exists())


if __name__ == "__main__":
    unittest.main()
