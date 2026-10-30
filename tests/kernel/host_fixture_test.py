#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host-fixture cleanup covers failure, interruption, bounds and concurrency."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/shared"))
import artifact_lifecycle
from host_fixture import HostFixture


class FixtureTests(unittest.TestCase):
    def test_owned_lifetimes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(artifact_lifecycle, "ROOT", root), \
                 patch.dict(os.environ, {"TMPDIR": str(root / "scratch")}):
                with HostFixture("success") as first, HostFixture("parallel") as second:
                    self.assertNotEqual(first.path, second.path)
                    first.run([sys.executable, "-c", "print('ok')"])
                    first_path, second_path = first.path, second.path
                self.assertFalse(first_path.exists())
                self.assertFalse(second_path.exists())
                for name, command, exception in (
                    ("failure", "raise SystemExit(3)", subprocess.CalledProcessError),
                    ("deadline", "import time; time.sleep(60)", subprocess.TimeoutExpired),
                    ("interrupt", "import os,signal;os.kill(os.getppid(),signal.SIGINT)", KeyboardInterrupt),
                ):
                    with self.assertRaises(exception):
                        with HostFixture(name) as fixture:
                            path = fixture.path
                            (path / "fixture.c").write_text("source evidence\n")
                            fixture.run([sys.executable, "-c", command], timeout=.3)
                    self.assertFalse(path.exists())
                failures = list((root / "build/test-failures").glob("*/fixture.source.txt"))
                self.assertEqual(len(failures), 3)
                self.assertEqual(list((root / "scratch").iterdir()), [])


if __name__ == "__main__":
    unittest.main()
