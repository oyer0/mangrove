#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production MFQS foundation plus genuinely failing model negative controls."""
import json
from pathlib import Path
import subprocess
import sys
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "filesystems/mfqs"))
sys.path.insert(0, str(ROOT / "shared"))
import artifact_lifecycle as artifacts
from model import fixed_trace
from generator import Random, VECTORS, generated_trace
from runner import build, compare, encode, qualify, validate_trace, response
from witnesses import evidence
from images import Unavailable, external_tools, initial_files

BASELINE = "487b9bdd6a536820c2f2ffcddcca691568c7de23"


def release_verified_media(directory):
    # Called only after all assertions for this case pass. Keep reports, wire
    # traces, diagnostics and prefixes until suite completion; retain media
    # for any unexpected failure. Standalone --artifacts is unaffected.
    for name in ("initial.img", "media.img"):
        (Path(directory) / name).unlink(missing_ok=True)


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
        assert report["coverage"]["substantive_content_mutations"] == report["coverage"]["verified_content_mutations"]
        assert not report["coverage"]["missing"] and "mixed-safe-lifecycle" in report["qualified_behaviors"]
        release_verified_media(folder / f"generated-{layout}-{seed}")
        release_verified_media(folder / f"generated-{layout}-{seed}-replay")
        if seed == 0:
            # Relabeling the same successful short trace must not invent 128 mutations.
            observed = [response(command, line, i, backend) for i, (command, line) in enumerate(
                zip(trace, (folder / f"generated-{layout}-{seed}/observations.tsv").read_text().splitlines()), 1)]
            weak = evidence(trace, observed, backend, layout, "extended")
            assert weak["substantive_content_mutations"] < 128 and "at-least-128-content-mutations" in weak["missing"]
            for negative in ("byte", "length", "namespace", "offset", "count", "status", "pins", "middle", "tail"):
                bad = qualify(folder / f"generated-{negative}", adapter, tools, compiler,
                              trace, negative, backend=backend, layout=layout, campaign="quick", seed=seed)
                failure = bad["failure"]
                assert bad["status"] == "FAIL" and failure["kind"] == "model-mismatch", failure
                assert failure["command"]["op"] == ("LIST" if negative == "namespace" else "WRITE" if negative in ("status", "pins") else "READ")
                changed = {key for key in failure["expected"] if failure["expected"][key] != failure["observed"][key]}
                changes = {"byte": {"data"}, "length": {"size"}, "namespace": {"count", "data"},
                           "offset": {"offset"}, "count": {"count"}, "status": {"status"},
                           "pins": {"pins"}, "middle": {"data"}, "tail": {"data"}}
                assert changed == changes[negative], failure
                assert "adapter_returncode" not in bad, bad.get("adapter_returncode")
                prefix = json.loads((folder / f"generated-{negative}/failing-prefix.json").read_text())
                assert prefix == trace[:failure["sequence"]]
                assert (folder / f"generated-{negative}/failing-prefix.tsv").exists()
                assert bad["image_check"]["returncode"] == 0
                release_verified_media(folder / f"generated-{negative}")
            print(f"PASS generated {backend} byte/length/namespace/offset/count/status/pins/middle/tail controls")
    # An accepted replay label is not evidence of substantive qualification.
    if layout == "empty":
        hello = qualify(folder / "hello-only", adapter, tools, compiler, [{"op": "HELLO"}],
                        backend=backend, campaign="quick", seed=0)
        assert hello["execution_status"] == "PASS" and hello["status"] == "INCOMPLETE"
        assert hello["qualified_behaviors"] == [] and hello["coverage"]["substantive_content_mutations"] == 0
        replayed = qualify(folder / "hello-only-replay", adapter, tools, compiler, hello["trace"],
                          backend=backend, campaign="quick", seed=0,
                          expected_initial=hello["initial_image"]["sha256"])
        assert replayed["status"] == "INCOMPLETE" and replayed["image_check"]["returncode"] == 0
        cli = subprocess.run([sys.executable, str(ROOT / "filesystems/mfqs/runner.py"), "--cc", compiler,
            "--replay", str(folder / "hello-only/report.json"), "--artifacts", str(folder / "hello-cli")],
            capture_output=True, text=True, timeout=150)
        assert cli.returncode == 2 and "INCOMPLETE" in cli.stdout, cli.stdout + cli.stderr
        known = [{"op": "HELLO"}, {"op": "MKDIR", "path": "/known"},
                 {"op": "STAT", "path": "/known"}, {"op": "STAT", "path": "/"},
                 {"op": "CREATE", "path": "/known/file"}, {"op": "CLOSE"},
                 {"op": "STAT", "path": "/known/file"}, {"op": "STAT", "path": "/absent"}]
        want = validate_trace(known, backend)
        assert want[2]["count"] == want[3]["count"] == 2 and want[2]["size"] is None
        assert want[6]["status"] == 0 and want[6]["count"] == 1 and want[6]["size"] == 0
        assert want[7]["status"] == -2
        checked = qualify(folder / "directory-stat", adapter, tools, compiler, known, backend=backend)
        assert checked["execution_status"] == "PASS" and checked["status"] == "INCOMPLETE"
        for name in ("hello-only", "hello-only-replay", "hello-cli", "directory-stat"):
            release_verified_media(folder / name)
        print(f"PASS {backend} production-backed insufficient-coverage replay and directory/file/absent STAT")



def extended_check(folder, adapter, tools, compiler, backend, layout="empty"):
    trace = generated_trace(backend, layout, 0x5eed54, "extended")
    folder = folder / ("campaign-" + layout)
    report = qualify(folder / "extended", adapter, tools, compiler, trace,
                     backend=backend, layout=layout, campaign="extended", seed=0x5eed54)
    assert report["status"] == "PASS", report.get("failure") or report.get("coverage")
    assert report["coverage"]["substantive_content_mutations"] >= 128
    replayed = qualify(folder / "extended-replay", adapter, tools, compiler, report["trace"],
                       backend=backend, layout=layout, campaign="extended", seed=0x5eed54,
                       expected_initial=report["initial_image"]["sha256"])
    assert replayed["status"] == "PASS" and replayed["metrics"] == report["metrics"]
    assert replayed["final_image_sha256"] == report["final_image_sha256"]
    release_verified_media(folder / "extended")
    release_verified_media(folder / "extended-replay")
    print(f"PASS extended {backend}/{layout} seed=0x5eed54: " + json.dumps(dict(
        mutations=report["coverage"]["substantive_content_mutations"],
        distinct_offsets=report["coverage"]["distinct_write_offsets"],
        commands=len(trace), trace_bytes=report["trace_bytes"],
        **report["metrics"], **report["timing"]), sort_keys=True))


def witness_known_answers():
    # Predicate unit tests only; production-backed witness controls run above.
    def analyze(trace, backend="mgfs", layout="empty"):
        return evidence(trace, validate_trace(trace, backend, layout), backend, layout, "quick")
    prefix = [{"op": "HELLO"}, {"op": "CREATE_ORDINARY", "path": "/f"},
              {"op": "WRITE", "data": "010203"}, {"op": "SEEK", "offset": 0}]
    unchecked = analyze(prefix + [{"op": "CLOSE"}])
    assert unchecked["substantive_content_mutations"] == 1 and unchecked["verified_content_mutations"] == 0
    assert "every-content-mutation-verified" in unchecked["missing"]
    partial = analyze(prefix + [{"op": "READ", "size": 2}, {"op": "CLOSE"}])
    assert partial["verified_content_mutations"] == 0
    checked = prefix + [{"op": "READ", "size": 3}, {"op": "CLOSE"}]
    assert analyze(checked)["verified_content_mutations"] == 1
    remounted = checked + [{"op": "REMOUNT"}, {"op": "OPEN", "path": "/f"}]
    assert "every-remount-file-verified" in analyze(remounted + [{"op": "CLOSE"}])["missing"]
    assert "every-remount-file-verified" not in analyze(remounted + [{"op": "READ", "size": 3}, {"op": "CLOSE"}])["missing"]
    unrelated = prefix + [{"op": "CHECK"}, {"op": "CLOSE"}, {"op": "CREATE", "path": "/g"},
                           {"op": "SNAPSHOT"}, {"op": "CHECK"}, {"op": "CLOSE"}]
    assert "rebind" not in analyze(unrelated)["witnesses"]
    recreated = [{"op":"HELLO"}, {"op":"LIST"},
        {"op":"CREATE_ORDINARY", "path":"/f"}, {"op":"CLOSE"},
        {"op":"LIST"}, {"op":"UNLINK", "path":"/f"},
        {"op":"CREATE_ORDINARY", "path":"/f"}, {"op":"CLOSE"},
        {"op":"REMOUNT"}, {"op":"LIST"}]
    assert "preallocated-directory-persistence" in analyze(recreated, layout="preallocated-directory")["missing"]
    imported = [{"op": "HELLO"}, {"op": "OPEN", "path": "/old.txt"},
                {"op": "WRITE", "data": "58"}, {"op": "SEEK", "offset": 0},
                {"op": "READ", "size": 4}, {"op": "CLOSE"}]
    assert "imported-read" not in analyze(imported, "fat32", "slack")["witnesses"]


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else "gcc"
    workspace = artifacts.ArtifactWorkspace(name="mfqs-regression")
    directory = workspace.path
    incomplete = False
    try:
        workspace.require_free_space(1024 * 1024 * 1024,
                                     "MFQS aggregate regression")
        witness_known_answers()
        rng = Random(0)
        assert tuple(rng.next() for _ in VECTORS) == VECTORS
        trace = fixed_trace()
        expected = validate_trace(trace)
        adapter, tools = build(directory, compiler)
        report = qualify(directory / "normal", adapter, tools, compiler, trace)
        assert report["status"] == "PASS", report.get("failure")
        assert report["mgfsck"]["returncode"] == 0
        assert (directory / "normal/initial.img").read_bytes() != (directory / "normal/media.img").read_bytes()
        release_verified_media(directory / "normal")
        for negative in ("byte", "length"):
            bad = qualify(directory / negative, adapter, tools, compiler, trace, negative)
            assert bad["status"] == "FAIL", bad
            assert bad["failure"]["kind"] == "model-mismatch", bad["failure"]
            assert bad["failure"]["command"]["op"] == "READ"
            assert bad["mgfsck"]["returncode"] == 0
            release_verified_media(directory / negative)
        generated_checks(directory, adapter, tools, compiler, "mgfs")
        extended_check(directory, adapter, tools, compiler, "mgfs")
        # Imported root has a partial logical tail and four allocated,
        # fragmented, poisoned blocks, independently validated by mgfsck.
        imported_trace = fixed_trace()
        imported = qualify(directory / "preallocated-fixed", adapter, tools, compiler,
                           imported_trace, backend="mgfs", layout="preallocated-directory")
        assert imported["status"] == "PASS" and "preallocated-directory-persistence" in imported["qualified_behaviors"]
        no_initial_list = [c for index,c in enumerate(imported_trace) if index != 1]
        missing = qualify(directory / "preallocated-unwitnessed", adapter, tools, compiler,
                          no_initial_list, backend="mgfs", layout="preallocated-directory")
        assert missing["execution_status"] == "PASS" and missing["status"] == "INCOMPLETE"
        assert "preallocated-directory-persistence" in missing["coverage"]["missing"]
        bad = qualify(directory / "preallocated-negative", adapter, tools, compiler,
                      imported_trace, "namespace", backend="mgfs", layout="preallocated-directory")
        assert bad["status"] == "FAIL" and bad["failure"]["command"]["op"] == "LIST"
        for name in ("preallocated-fixed", "preallocated-unwitnessed", "preallocated-negative"):
            release_verified_media(directory / name)
        allocation_baseline = "d49b92aef05be842a823472b17f13b24c28cb712"
        allocation_source = directory / "mgfs-allocation-baseline.c"
        allocation_source.write_bytes(subprocess.check_output(
            ["git", "show", allocation_baseline + ":kernel/src/storage/mgfs.c"], cwd=ROOT.parent))
        allocation_build = directory / "allocation-historical-build"
        allocation_build.mkdir()
        allocation_adapter, allocation_tools = build(allocation_build, compiler, allocation_source)
        allocation_trace = [{"op":"HELLO"}, {"op":"LIST"},
                            {"op":"MKDIR", "path":"/box"}, {"op":"LIST"}]
        old_allocation = qualify(directory / "allocation-historical", allocation_adapter,
            allocation_tools, compiler, allocation_trace, driver_source=allocation_source,
            backend="mgfs", layout="preallocated-directory")
        assert old_allocation["status"] == "FAIL" and old_allocation["failure"]["kind"] == "model-mismatch"
        assert old_allocation["failure"]["sequence"] == 4 and old_allocation["failure"]["command"]["op"] == "LIST"
        assert old_allocation["initial_check"]["returncode"] == 0 and old_allocation["image_check"]["returncode"] != 0
        print("PASS historical MGFS directory-allocation control: mkdir succeeds but namespace is inaccessible; independent checker rejects")
        release_verified_media(directory / "allocation-historical")
        generated_checks(directory, adapter, tools, compiler, "mgfs", "preallocated-directory")
        extended_check(directory, adapter, tools, compiler, "mgfs", "preallocated-directory")
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
        release_verified_media(directory / "historical")

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
        unknown_output = directory / "unsupported-layout-cli"
        unknown = subprocess.run([sys.executable, str(ROOT / "filesystems/mfqs/runner.py"),
                                  "--backend", "mgfs", "--layout", "slack",
                                  "--artifacts", str(unknown_output)],
                                 capture_output=True, text=True)
        assert unknown.returncode == 2 and "INCOMPLETE" in unknown.stdout
        unknown_report = json.loads((unknown_output / "incomplete.json").read_text())
        assert unknown_report["status"] == "INCOMPLETE"
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
            [{"op": "HELLO"}, {"op": "MKDIR", "path": "/d"}, {"op": "UNLINK", "path": "/d"}],
            [{"op": "HELLO"}, {"op": "LIST", "path": "/missing"}],
            [{"op": "HELLO"}, {"op": "CREATE", "path": "/file"}, {"op": "CLOSE"},
             {"op": "STAT", "path": "/file/child"}],
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
        release_verified_media(directory / "replayed")
        print(f"PASS MFQS MGFS foundation: {len(trace)} commands, complete production VFS/driver, "
              "exclusive-create recovery handoff, exact bytes/lengths/offsets/namespace, "
              "two clean remounts, teardown and independent mgfsck")
        print("PASS negative-control checks: incorrect byte and length each report FAIL; "
              "historical production shrink-on-overwrite reports FAIL (4096 -> 1)")
        print("MGFS measured resources: " + json.dumps(report["metrics"], sort_keys=True))
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
                release_verified_media(folder / layout)
                release_verified_media(folder / (layout + "-replay"))
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
                        release_verified_media(folder / negative)
                    print(f"PASS {backend} negative controls: wrong bytes/lengths report FAIL")
            for layout in layouts:
                generated_checks(folder, backend_adapter, backend_tools, compiler, backend, layout)
            extended_check(folder, backend_adapter, backend_tools, compiler, backend)
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
                release_verified_media(folder / "historical")
                print("PASS historical exFAT negative control: poisoned slack exposed at "
                      "lengths 17 and 4197, independent oracle reports FAIL")
        print("INCOMPLETE broader qualification: Unicode/import matrices, directory moves, "
              "open-handle namespace mutations, large-file/fault/concurrency/guest profiles")
        if incomplete:
            outcome = dict(suite="MFQS", status="INCOMPLETE",
                           error="one or more optional filesystem backends unavailable",
                           compiler=compiler)
            retained = workspace.finish(outcome, verified=False)
            print("INCOMPLETE backend evidence retained: " +
                  str(retained.get("failure_evidence") or directory))
            return 2
    except BaseException as error:
        # Preserve concise reports and the image(s) named by a failed case;
        # the complete collection of successful scratch images is disposable.
        failed_media = []
        reports = sorted(directory.rglob("report.json"),
                         key=lambda path: path.stat().st_mtime_ns, reverse=True)
        for path in reports:
            try:
                data = json.loads(path.read_text())
            except (OSError, ValueError):
                continue
            if data.get("status") in {"FAIL", "INCOMPLETE"}:
                media = path.parent / "media.img"
                if media.is_file() and not media.is_symlink():
                    failed_media.append(media)
                    if len(failed_media) == artifacts.MAX_FAILURE_MEDIA:
                        break
        if failed_media:
            workspace.set_failure_media(*failed_media)
        status = "INCOMPLETE" if isinstance(error, KeyboardInterrupt) else "FAIL"
        outcome = dict(suite="MFQS", status=status,
                       error=f"{type(error).__name__}: {error}", compiler=compiler)
        retained = workspace.finish(outcome, verified=False)
        print("MFQS verification failed; bounded evidence: " +
              str(retained.get("failure_evidence") or directory), file=sys.stderr)
        raise
    workspace.finish(dict(suite="MFQS", status="PASS", compiler=compiler), verified=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
