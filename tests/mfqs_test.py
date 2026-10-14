#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production MFQS foundation plus genuinely failing model negative controls."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "mfqs"))
from model import fixed_trace
from generator import Random, VECTORS, generated_trace
from runner import build, compare, encode, qualify, validate_trace
from images import Unavailable, external_tools, initial_files

BASELINE = "487b9bdd6a536820c2f2ffcddcca691568c7de23"


def generated_checks(folder, adapter, tools, compiler, backend, layout="empty"):
    seeds = (0, 1, 0x5eed52) if layout == "empty" else (0x5eed52,)
    for seed in seeds:
        trace = generated_trace(backend, layout, seed)
        assert trace == generated_trace(backend, layout, seed)
        report = qualify(folder / f"generated-{layout}-{seed}", adapter, tools, compiler,
                         trace, backend=backend, layout=layout, campaign="quick", seed=seed)
        assert report["status"] == "PASS", report.get("failure")
        replay = report["trace"]  # No generator involved in execution/replay.
        repeated = qualify(folder / f"generated-{layout}-{seed}-replay", adapter, tools, compiler,
                           replay, backend=backend, layout=layout, campaign="quick", seed=seed,
                           expected_initial=report["initial_image"]["sha256"])
        assert repeated["status"] == "PASS", repeated.get("failure")
        assert repeated["final_image_sha256"] == report["final_image_sha256"]
        assert repeated["metrics"] == report["metrics"]
        print(f"PASS generated {backend}/{layout} seed={seed}: {len(trace)} commands; " +
              json.dumps(dict(**report["metrics"], **report["timing"], trace_bytes=report["trace_bytes"]), sort_keys=True))
        if seed == 0:
            for negative in ("byte", "length", "namespace"):
                bad = qualify(folder / f"generated-{negative}", adapter, tools, compiler,
                              trace, negative, backend=backend, layout=layout, campaign="quick", seed=seed)
                failure = bad["failure"]
                assert bad["status"] == "FAIL" and failure["kind"] == "model-mismatch", failure
                assert failure["command"]["op"] == ("LIST" if negative == "namespace" else "READ")
                changed = {key for key in failure["expected"] if failure["expected"][key] != failure["observed"][key]}
                assert changed == ({"data"} if negative == "byte" else {"size"} if negative == "length" else {"count", "data"})
                prefix = json.loads((folder / f"generated-{negative}/failing-prefix.json").read_text())
                assert prefix == trace[:failure["sequence"]]
                assert (folder / f"generated-{negative}/failing-prefix.tsv").exists()
                assert bad["image_check"]["returncode"] == 0
            print(f"PASS generated {backend} byte/length/namespace oracle controls")


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else "gcc"
    directory = Path(tempfile.mkdtemp(prefix="mangrove-mfqs-regression-"))
    try:
        rng = Random(0)
        assert tuple(rng.next() for _ in VECTORS) == VECTORS
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
        generated_checks(directory, adapter, tools, compiler, "mgfs")
        # The baseline body is immutable historical production code, compiled
        # as a whole unit with today's VFS/host adapter. No behavioral patching.
        old_source = directory / "mgfs-baseline.c"
        old_source.write_bytes(subprocess.check_output(
            ["git", "show", BASELINE + ":kernel/src/storage/mgfs.c"], cwd=ROOT.parent))
        old_build = directory / "historical-build"
        old_build.mkdir()
        old_adapter, old_tools = build(old_build, compiler, old_source)
        bad = qualify(directory / "historical", old_adapter, old_tools, compiler,
                      trace, driver_source=old_source)
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
                                  "--backend", "mgfs", "--layout", "slack"], capture_output=True, text=True)
        assert unknown.returncode == 2 and "INCOMPLETE" in unknown.stdout
        # Discovery/reporting unit check only, not a mocked filesystem test.
        with patch("images.shutil.which", return_value=None):
            for backend in ("fat32", "exfat"):
                try:
                    external_tools(backend)
                except Unavailable:
                    pass
                else:
                    raise AssertionError("missing tooling was counted as available")
        for malformed in (
            [{"op": "HELLO"}, {"op": "WRITE", "data": "z0"}],
            [{"op": "HELLO"}, {"op": "OPEN", "path": "/bad/../name"}],
            [{"op": "HELLO"}, {"op": "READ", "size": 8193}],
            [{"op": "HELLO"}] * 8193,
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
        print("MGFS measured resources: " + json.dumps(report["metrics"], sort_keys=True))
        incomplete = False
        for backend, layouts in (("fat32", ("empty", "slack")),
                                 ("exfat", ("empty", "slack", "chained-slack", "short-valid"))):
            folder = directory / backend
            folder.mkdir()
            try:
                backend_adapter, backend_tools = build(folder, compiler, backend=backend)
            except Unavailable as error:
                print("INCOMPLETE " + str(error))
                incomplete = True
                continue
            for layout in layouts:
                sequence = fixed_trace(backend, initial_files(layout))
                current = qualify(folder / layout, backend_adapter, backend_tools, compiler,
                                  sequence, backend=backend, layout=layout)
                assert current["status"] == "PASS", current.get("failure")
                assert current["image_check"]["returncode"] == 0
                replayed = qualify(folder / (layout + "-replay"), backend_adapter, backend_tools,
                    compiler, current["trace"], backend=backend, layout=layout,
                    expected_initial=current["initial_image"]["sha256"])
                assert replayed["status"] == "PASS", replayed.get("failure")
                assert current["final_image_sha256"] == replayed["final_image_sha256"]
                print(f"PASS {backend}/{layout}: {len(sequence)} commands, exact model, "
                      "two persisted remounts, independent checker, deterministic replay; " +
                      json.dumps(dict(**current["metrics"], **current["timing"]), sort_keys=True))
                if layout == "slack":
                    for negative in ("byte", "length"):
                        bad = qualify(folder / negative, backend_adapter, backend_tools, compiler,
                                      sequence, negative, backend=backend, layout=layout)
                        assert bad["status"] == "FAIL"
                        assert bad["failure"]["kind"] == "model-mismatch"
                        assert bad["image_check"]["returncode"] == 0
                    print(f"PASS {backend} negative controls: wrong bytes/lengths report FAIL")
            for layout in layouts:
                generated_checks(folder, backend_adapter, backend_tools, compiler, backend, layout)
            if backend == "exfat":
                ref = "cb29138da7d20f1b0ea3cd96f86e6682d7752311"
                old_driver = folder / "exfat-baseline.c"
                old_driver.write_bytes(subprocess.check_output(
                    ["git", "show", ref + ":kernel/src/storage/exfat.c"], cwd=ROOT.parent))
                historical = folder / "historical-build"
                historical.mkdir()
                old_adapter, old_tools = build(historical, compiler, old_driver, backend="exfat")
                sequence = fixed_trace("exfat", initial_files("slack"))
                bad = qualify(folder / "historical", old_adapter, old_tools, compiler, sequence,
                              driver_source=old_driver, backend="exfat", layout="slack")
                assert bad["status"] == "FAIL", bad
                assert bad["failure"]["kind"] == "model-mismatch", bad["failure"]
                assert bad["failure"]["command"] == {"op": "READ", "size": 17}
                # Both original extension cases ran through the same actual
                # VFS backend: successful writes, no pins, exposed poison.
                observations = (folder / "historical/observations.tsv").read_text().splitlines()
                for size in (17, 4197):
                    index = next(i for i, command in enumerate(sequence)
                                 if command == {"op": "READ", "size": size})
                    fields = observations[index].split("\t")
                    assert fields[2] == "0" and fields[6] == "0"
                    data = bytes.fromhex(fields[7])
                    assert len(data) == size and data[4:16] == b"\xa5" * 12
                print("PASS historical exFAT negative control: poisoned slack exposed at "
                      "lengths 17 and 4197, independent oracle reports FAIL")
        print("INCOMPLETE broader qualification: Unicode/import matrices, directory moves, "
              "open-handle namespace mutations, large-file/fault/concurrency/guest profiles")
        if incomplete:
            print(f"INCOMPLETE backend evidence retained: {directory}")
            return 2
    except BaseException:
        print(f"MFQS verification failed; complete artifacts preserved: {directory}", file=sys.stderr)
        raise
    shutil.rmtree(directory)
    return 0


if __name__ == "__main__":
    sys.exit(main())
