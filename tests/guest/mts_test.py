#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cheap orchestration controls; simulated outcomes are never guest evidence."""
import copy
import json
import os
from pathlib import Path
import signal
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
import mts


class Orchestration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mangrove-mts-")
        self.addCleanup(self.temp.cleanup)
        self.output = Path(self.temp.name)
        self.env = dict(os.environ)

    def run_group(self, code, **options):
        return mts.execute(mts.Group("control", [sys.executable, "-c", code], **options),
                           self.output, self.env)

    def test_command_results(self):
        for code, status in (("print('completed')", "PASS"),
                             ("print('failed'); raise SystemExit(1)", "FAIL"),
                             ("print('failed'); raise SystemExit(2)", "FAIL"),
                             ("print('SKIP independent image: missing checker')", "INCOMPLETE"),
                             ("print('INCOMPLETE math reference dependency unavailable')", "INCOMPLETE"),
                             ("print('No space left on device'); raise SystemExit(1)", "INCOMPLETE"),
                             ("pass", "INCOMPLETE")):
            with self.subTest(code=code):
                result = self.run_group(code)
                self.assertTrue(result["executed"])
                self.assertEqual(result["status"], status, result)
                self.assertEqual(mts.overall([result]), status)
        missing = self.run_group("raise SystemExit('must not execute')", tools=("mts-nonexistent-prerequisite",))
        self.assertEqual(missing["status"], "INCOMPLETE")
        self.assertFalse(missing["executed"])
        self.assertIn("missing prerequisite", missing["reason"])
        optional = self.run_group("raise SystemExit('must not execute')", skip="hardware requires explicit selection")
        self.assertEqual(optional["status"], "SKIPPED")
        self.assertFalse(optional["executed"])
        self.assertEqual(mts.overall([dict(optional, optional=True), dict(status="PASS")]), "PASS")
        self.assertEqual(mts.overall([dict(optional, optional=True)]), "INCOMPLETE")
        self.assertEqual(mts.overall([]), "INCOMPLETE")
        # Skipping a required group cannot qualify, even alongside passing work.
        self.assertEqual(mts.overall([optional, dict(status="PASS")]), "INCOMPLETE")

    def test_timeout_and_interruption(self):
        # A blocked pipe is deterministic; no sleeps or external resources.
        result = self.run_group("import os; r,w=os.pipe(); os.read(r,1)", seconds=.1)
        self.assertEqual(result["status"], "INCOMPLETE")
        self.assertIn("deadline", result["reason"])
        result = self.run_group("raise KeyboardInterrupt")
        self.assertEqual(result["status"], "INCOMPLETE")
        result = self.run_group("import os,signal; os.kill(os.getpid(),signal.SIGTERM)")
        self.assertEqual(result["status"], "INCOMPLETE")
        # Interrupt the orchestrator during its owned child, then require that
        # the entire session, including a descendant, is gone (or reaped zombie).
        code = ("import os,signal,subprocess,sys; "
                "p=subprocess.Popen([sys.executable,'-c','import os; r,w=os.pipe(); os.read(r,1)']); "
                f"open({str(self.output / 'pid')!r},'w').write(str(p.pid)); "
                "os.kill(os.getppid(),signal.SIGINT); r,w=os.pipe(); os.read(r,1)")
        result = self.run_group(code)
        self.assertTrue(result.get("interrupted"), result)
        pid = int((self.output / "pid").read_text())
        stat = Path(f"/proc/{pid}/stat")
        if stat.exists():
            self.assertEqual(stat.read_text().split()[2], "Z")

    def test_output_bound_and_unfinished_descendant(self):
        with patch.object(mts, "LOG_LIMIT", 128):
            result = self.run_group("print('x'*256)")
        self.assertEqual(result["status"], "INCOMPLETE")
        code = ("import subprocess,sys; subprocess.Popen([sys.executable,'-c',"
                "'import os; r,w=os.pipe(); os.read(r,1)']); print('completed leader')")
        result = self.run_group(code)
        self.assertEqual(result["status"], "INCOMPLETE")
        self.assertIn("outlived", result["reason"])

    def test_existing_runner_receipts(self):
        path = self.output / "report.json"
        spec = dict(path=str(path), status="PASS", collection="profiles", count=3,
                    phases=["create", "verify"], backends=["mgfs", "fat32", "exfat"])
        valid = dict(status="PASS", profiles=[dict(status="PASS", backend=backend,
                     create=dict(child_status=0), verify=dict(child_status=0))
                     for backend in spec["backends"]])
        absent = self.run_group("print('PASS')", receipt=spec)
        self.assertEqual(absent["status"], "INCOMPLETE")
        malformed = ["", "{", "[]", json.dumps(dict(status="PASS"))]
        for mutation in ("status", "count", "backend", "phase", "active", "failed", "missing-status", "row"):
            data = copy.deepcopy(valid)
            if mutation == "status": data["status"] = "FAIL"
            if mutation == "count": data["profiles"].pop()
            if mutation == "backend": data["profiles"][0]["backend"] = "fat32"
            if mutation == "phase": del data["profiles"][0]["verify"]
            if mutation == "active": data["profiles"][0]["active_phase"] = "verify"
            if mutation == "failed": data["profiles"][0]["status"] = "FAIL"
            if mutation == "missing-status": del data["profiles"][0]["status"]
            if mutation == "row": data["profiles"][0] = None
            malformed.append(json.dumps(data))
        for text in malformed:
            path.write_text(text)
            result = self.run_group("print('PASS')", receipt=spec)
            self.assertEqual(result["status"], "INCOMPLETE", text)
        path.write_text(json.dumps(valid))
        self.assertEqual(self.run_group("print('completed')", receipt=spec)["status"], "PASS")
        self.assertEqual(self.run_group("print('failed'); raise SystemExit(1)", receipt=spec)["status"], "FAIL")
        for extra in (dict(fields=["wrong_replacement"]), dict(row_fields=["wrong_content"]),
                      dict(stress_controls=True), dict(phases=["create", "verify", "controls"])):
            self.assertEqual(self.run_group("print('completed')", receipt={**spec, **extra})["status"], "INCOMPLETE")

    def test_profiles_and_atomic_summary(self):
        plans = {p: mts.plan(p, "make", self.output, "tcg", (Path("code"), Path("vars")))
                 for p in ("host", "guest", "all")}
        hosts = [g.command[-1] for g in plans["host"] if not g.skip]
        self.assertEqual(hosts, ["test-" + name for name in mts.HOST])
        standard = [g.name for g in plans["guest"] if not g.skip]
        self.assertEqual(standard, ["guest-build", "guest-root", "guest-secondary"])
        for profile, groups in plans.items():
            self.assertEqual(len({g.name for g in groups}), len(groups))
            for group in groups:
                self.assertTrue(group.skip or group.command)
                self.assertFalse(any("physical_sandisk" in arg or "stress_kvm_boot" in arg
                                     for arg in group.command))
        self.assertTrue(all(g.skip for g in plans["host"] if g.name == "guest"))
        mts.save(self.output, dict(status="INCOMPLETE"))
        mts.save(self.output, dict(status="PASS"))
        self.assertEqual(json.loads((self.output / "summary.json").read_text())["status"], "PASS")
        self.assertFalse((self.output / "summary.tmp").exists())

    def test_cli_missing_firmware(self):
        import subprocess
        output = self.output / "cli"
        # This case verifies the missing-firmware diagnostic and never boots a
        # guest. Select TCG explicitly so the test remains independent of host
        # KVM access without changing the native guest qualification default.
        result = subprocess.run([sys.executable, str(Path(mts.__file__)), "guest",
                                 "--accel", "tcg",
                                 "--output", str(output), "--ovmf-code", str(self.output / "absent-code"),
                                 "--ovmf-vars", str(self.output / "absent-vars")],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 2, result.stderr)
        summary = json.loads((output / "summary.json").read_text())
        self.assertEqual(summary["status"], "INCOMPLETE")
        self.assertTrue(any("missing OVMF" in row["reason"] for row in summary["groups"]))
        self.assertFalse(any(row["status"] == "PASS" for row in summary["groups"]))

    def test_real_guest_argument_parsers(self):
        # Parse every comprehensive invocation with its real runner, then stop
        # before image construction, build checks or guest execution.
        import argparse
        import runpy
        class Parsed(Exception):
            pass
        original = argparse.ArgumentParser.parse_args
        def parse_then_stop(parser, *args, **kwargs):
            original(parser, *args, **kwargs)
            raise Parsed()
        for group in mts.plan("all", "make", self.output, "tcg", (Path("code"), Path("vars"))):
            if group.receipt:
                with self.subTest(group=group.name), patch.object(sys, "argv", group.command[1:]), \
                     patch.object(argparse.ArgumentParser, "parse_args", parse_then_stop):
                    with self.assertRaises(Parsed):
                        runpy.run_path(group.command[1], run_name="__main__")


if __name__ == "__main__":
    unittest.main()
