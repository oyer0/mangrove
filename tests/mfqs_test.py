#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production MFQS foundation plus genuinely failing model negative controls."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "mfqs"))
from model import fixed_trace
from runner import build, compare, encode, qualify, validate_trace

BASELINE = "487b9bdd6a536820c2f2ffcddcca691568c7de23"


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else "gcc"
    directory = Path(tempfile.mkdtemp(prefix="mangrove-mfqs-regression-"))
    try:
        trace = fixed_trace()
        expected = validate_trace(trace)
        adapter, tools = build(directory, compiler)
        report = qualify(directory / "normal", adapter, tools, compiler, trace)
        assert report["status"] == "PASS", report.get("failure")
        assert report["mgfsck"]["returncode"] == 0
        assert (directory / "normal/initial.img").read_bytes() != (directory / "normal/media.img").read_bytes()
        for negative in ("byte", "length"):
            bad = qualify(directory / negative, adapter, tools, compiler, trace, negative)
            assert bad["status"] == "FAIL", bad
            assert bad["failure"]["kind"] == "model-mismatch", bad["failure"]
            assert bad["failure"]["command"]["op"] == "READ"
            assert bad["mgfsck"]["returncode"] == 0
        # The baseline body is immutable historical production code, compiled
        # as a whole unit with today's VFS/host adapter. No behavioral patching.
        old_source = directory / "mgfs-baseline.c"
        old_source.write_bytes(subprocess.check_output(
            ["git", "show", BASELINE + ":kernel/src/storage/mgfs.c"], cwd=ROOT.parent))
        old_build = directory / "historical-build"
        old_build.mkdir()
        old_adapter, old_tools = build(old_build, compiler, old_source)
        bad = qualify(directory / "historical", old_adapter, old_tools, compiler,
                      trace, mgfs_source=old_source)
        assert bad["status"] == "FAIL", bad
        assert bad["failure"]["kind"] == "model-mismatch", bad["failure"]
        assert bad["failure"]["command"] == {"op": "WRITE", "data": "5a"}
        assert bad["failure"]["expected"]["size"] == 4096
        assert bad["failure"]["observed"]["size"] == 1

        # Response ordering/version and duplicate membership are meaningful
        # checks, not lossy set comparisons or unchecked success summaries.
        wire = (directory / "normal/observations.tsv").read_text()
        assert compare(trace, expected, wire.replace("MFQS1", "MFQS2", 1))["kind"] == "protocol"
        assert compare(trace, expected, wire.rsplit("\n", 2)[0] + "\n")["kind"] == "protocol"
        membership_trace = [{"op": "LIST"}]
        want = [dict(status=0, size=0, offset=0, count=2, pins=0,
                     data=b"a\0b\0".hex())]
        duplicate = "MFQS1\t1\t0\t0\t0\t2\t0\t" + b"a\0a\0".hex() + "\n"
        assert compare(membership_trace, want, duplicate)["kind"] == "model-mismatch"
        unknown = subprocess.run([sys.executable, str(ROOT / "mfqs/runner.py"),
                                  "--backend", "fat32"], capture_output=True, text=True)
        assert unknown.returncode == 2 and "INCOMPLETE" in unknown.stdout
        for malformed in (
            [{"op": "HELLO"}, {"op": "WRITE", "data": "z0"}],
            [{"op": "HELLO"}, {"op": "OPEN", "path": "/bad/name"}],
            [{"op": "HELLO"}, {"op": "READ", "size": 8193}],
            [{"op": "HELLO"}] * 257,
        ):
            try:
                validate_trace(malformed)
            except ValueError:
                pass
            else:
                raise AssertionError("accepted malformed/out-of-bound replay")

        # Replay is identical model input, independent of saved expected data.
        replay = json.loads((directory / "normal/report.json").read_text())["trace"]
        assert replay == trace
        assert validate_trace(replay) == expected
        assert "".join(encode(c, i) for i, c in enumerate(replay, 1)) == (directory / "normal/commands.tsv").read_text()
        replayed = qualify(directory / "replayed", adapter, tools, compiler, replay,
                           expected_initial=report["initial_image"]["sha256"])
        assert replayed["status"] == "PASS", replayed
        assert replayed["final_image_sha256"] == report["final_image_sha256"]
        print(f"PASS MFQS MGFS foundation: {len(trace)} commands, complete production VFS/driver, "
              "exclusive-create recovery handoff, exact bytes/lengths/offsets/namespace, "
              "two clean remounts, teardown and independent mgfsck")
        print("PASS negative-control checks: incorrect byte and length each report FAIL; "
              "historical production shrink-on-overwrite reports FAIL (4096 -> 1)")
        print("INCOMPLETE qualification: FAT32/exFAT, broader operations, random/fault/concurrency/guest profiles")
        print("MFQS measured resources: " + json.dumps(report["metrics"], sort_keys=True))
    except BaseException:
        print(f"MFQS verification failed; complete artifacts preserved: {directory}", file=sys.stderr)
        raise
    shutil.rmtree(directory)


if __name__ == "__main__":
    main()
