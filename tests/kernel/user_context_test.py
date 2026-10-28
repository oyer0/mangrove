#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production Ring 3 entry-context and FS-base handoff checks."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"


def extract(path, name):
    source = (ROOT / path).read_text()
    pattern = re.compile(r"^(?:static )?[A-Za-z_][^\n;{}]*\b" +
                         re.escape(name) + r"\s*\(", re.M)
    for match in pattern.finditer(source):
        brace = source.index("{", match.end())
        if ";" in source[match.end():brace]:
            continue
        depth, end = 1, brace + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end] + "\n"
    raise ValueError(f"missing production function: {path}:{name}")


fixture = (ROOT / "tests/kernel/user_context_test.c").read_text()
fixture = fixture.replace("/* PROCESS_INITIAL_CONTEXT */",
                          extract("kernel/src/process.c",
                                  "process_user_context_prepare_initial"))
fixture = fixture.replace("/* PROCESS_WORKER_CONTEXT */", "\n".join(
    extract("kernel/src/process.c", name) for name in (
        "process_user_context_prepare_worker",
        "process_user_context_discard_worker")))
fixture = fixture.replace("/* SCHEDULER_CONTEXT */", "\n".join(
    extract("kernel/src/scheduler.c", name) for name in (
        "scheduler_context_switch_saved",
        "scheduler_context_restore_current",
        "scheduler_context_switch_complete")))
fixture = fixture.replace("/* WORKER_RETURN */",
                          extract("kernel/src/syscall.c",
                                  "syscall_finish_worker_return"))

with tempfile.TemporaryDirectory(prefix="mangrove-user-context-") as folder:
    folder = Path(folder)
    cfile = folder / "user_context_test.c"
    binary = folder / "user_context_test"
    cfile.write_text(fixture)
    subprocess.run([CC, "-std=gnu17", "-O1", "-g", "-Wall", "-Wextra",
        "-Werror", "-fsanitize=address,undefined", "-pthread",
        str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

    obj = folder / "ring3.o"
    subprocess.run([CC, "-m64", "-c", str(ROOT / "kernel/src/ring3.s"),
                    "-o", str(obj)], check=True)
    disassembly = subprocess.run(["objdump", "-d", str(obj)], check=True,
        capture_output=True, text=True).stdout.lower()
    assert "iretq" in disassembly
    assert "wrmsr" in disassembly and "c0000100" in disassembly
    assert "xor    %r15d,%r15d" in disassembly

print("Ring 3 transition clears stale registers and installs FS base PASS")
