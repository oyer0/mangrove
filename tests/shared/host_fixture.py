# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded host fixtures using the shared owned-artifact lifecycle."""
import os
from pathlib import Path
import signal
import subprocess

from artifact_lifecycle import ArtifactWorkspace


class HostFixture:
    def __init__(self, name):
        self.workspace = ArtifactWorkspace(name=name)
        self.path = self.workspace.path
        self.step = 0

    def __enter__(self):
        self.workspace.require_free_space(16 * 1024 * 1024, "host fixture")
        return self

    def run(self, command, *, timeout=30, check=True, env=None):
        self.step += 1
        log = self.path / f"step-{self.step}.log"
        process = None
        with log.open("w") as stream:
            stream.write(repr(command) + "\n")
            stream.flush()
            try:
                process = subprocess.Popen(command, cwd=self.path,
                    env=env, stdout=stream, stderr=subprocess.STDOUT,
                    start_new_session=True)
                result = process.wait(timeout=timeout)
            except BaseException:
                if process is not None:
                    # This group was created by this invocation, never borrowed.
                    try:
                        os.killpg(process.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        process.wait(timeout=2)
                raise
        output = log.read_text(errors="replace")[-1024 * 1024:]
        completed = subprocess.CompletedProcess(command, result, output, output)
        if check and result:
            raise subprocess.CalledProcessError(result, command, output=output)
        return completed

    def __exit__(self, kind, value, traceback):
        if kind is not None:
            for source in self.path.glob("*.c"):
                if source.stat().st_size <= 8 * 1024 * 1024:
                    source.with_suffix(".source.txt").write_bytes(source.read_bytes())
        report = self.workspace.finish({
            "status": "PASS" if kind is None else "FAIL",
            "error": None if kind is None else str(value),
        }, verified=kind is None)
        if kind is not None:
            print("host fixture evidence:", report.get("failure_evidence"))
        return False
