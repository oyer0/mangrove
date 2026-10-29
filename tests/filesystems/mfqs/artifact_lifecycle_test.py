#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""MFQS runner integration with owned artifact cleanup."""
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import runner


class MFQSArtifactLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mfqs-lifecycle-test-")
        self.root = Path(self.temp.name)
        self.old_root = runner.artifacts.ROOT
        self.old_tmpdir = os.environ.get("TMPDIR")
        runner.artifacts.ROOT = self.root
        self.tmpdir = self.root / "tmp"
        self.tmpdir.mkdir()
        os.environ["TMPDIR"] = str(self.tmpdir)

    def tearDown(self):
        runner.artifacts.ROOT = self.old_root
        if self.old_tmpdir is None:
            os.environ.pop("TMPDIR", None)
        else:
            os.environ["TMPDIR"] = self.old_tmpdir
        self.temp.cleanup()

    def _patch_build(self):
        return mock.patch.object(runner, "build", return_value=(Path("adapter"), {}))

    @staticmethod
    def _pass(directory, *args, **kwargs):
        directory = Path(directory)
        (directory / "media.img").write_bytes(b"reproducible media")
        report = {"status": "PASS", "profile": "mgfs-fixed-foundation",
                  "coverage": {}, "image_check": {"returncode": 0}}
        (directory / "report.json").write_text(json.dumps(report))
        return report

    def test_success_removes_default_workspace_and_honors_tmpdir(self):
        with self._patch_build(), mock.patch.object(runner, "qualify", side_effect=self._pass):
            self.assertEqual(runner.main([]), 0)
        self.assertEqual(list(self.tmpdir.iterdir()), [])

    def test_explicit_output_remains_for_caller(self):
        output = self.root / "requested-output"
        with self._patch_build(), mock.patch.object(runner, "qualify", side_effect=self._pass):
            self.assertEqual(runner.main(["--artifacts", str(output)]), 0)
        self.assertTrue((output / "media.img").is_file())
        self.assertTrue((output / "report.json").is_file())
        self.assertFalse((output / runner.artifacts.MARKER).exists())

    def test_failure_keeps_bounded_report_and_selected_media(self):
        def fail(directory, *args, **kwargs):
            directory = Path(directory)
            (directory / "media.img").write_bytes(b"failed media")
            (directory / "serial.log").write_text("model mismatch at sequence 42\n")
            report = {"status": "FAIL", "profile": "mgfs-fixed-foundation",
                      "failure": {"sequence": 42}, "coverage": {}}
            (directory / "report.json").write_text(json.dumps(report))
            return report

        with self._patch_build(), mock.patch.object(runner, "qualify", side_effect=fail):
            self.assertEqual(runner.main([]), 1)
        self.assertEqual(list(self.tmpdir.iterdir()), [])
        evidence = list((self.root / "build/test-failures").glob("mfqs-run-*"))
        self.assertEqual(len(evidence), 1)
        self.assertIn("sequence 42", (evidence[0] / "serial.log").read_text())
        self.assertTrue((evidence[0] / "media.img").is_file())
        self.assertEqual(json.loads((evidence[0] / "report.json").read_text())["status"], "FAIL")

    def test_harness_error_cleans_tmp_and_retains_incomplete_diagnostic(self):
        with self._patch_build():
            self.assertEqual(runner.main(["--replay", str(self.root / "missing.json")]), 2)
        self.assertEqual(list(self.tmpdir.iterdir()), [])
        evidence = list((self.root / "build/test-failures").glob("mfqs-run-*"))
        self.assertEqual(len(evidence), 1)
        incomplete = json.loads((evidence[0] / "incomplete.json").read_text())
        self.assertEqual(incomplete["status"], "INCOMPLETE")

    def test_interruption_publishes_evidence_and_cleans_workspace(self):
        with self._patch_build(), mock.patch.object(runner, "qualify", side_effect=KeyboardInterrupt):
            self.assertEqual(runner.main([]), 130)
        self.assertEqual(list(self.tmpdir.iterdir()), [])
        evidence = list((self.root / "build/test-failures").glob("mfqs-run-*"))
        self.assertEqual(len(evidence), 1)
        self.assertTrue((evidence[0] / "incomplete.json").is_file())


if __name__ == "__main__":
    unittest.main(verbosity=2)
