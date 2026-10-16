#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Focused production-source tests for Stage 3 runtime foundations."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CC = sys.argv[1] if len(sys.argv) > 1 else "gcc"
BASE = [CC, "-std=gnu17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fno-builtin", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all"]
RENAMED_STRING = ["strlen", "strcmp", "strncmp", "strchr", "strcpy",
                  "strncpy", "strncat", "memset", "memcpy", "memmove",
                  "memcmp", "strerror"]
CTYPE = ["isalnum", "isalpha", "isblank", "iscntrl", "isdigit", "isgraph",
         "islower", "isprint", "ispunct", "isspace", "isupper", "isxdigit",
         "tolower", "toupper"]
STDLIB = ["malloc", "calloc", "realloc", "free", "abort", "atexit",
          "at_quick_exit", "exit", "quick_exit", "_Exit", "abs", "labs",
          "llabs", "div", "ldiv", "lldiv", "getenv", "system", "rand",
          "srand", "strtol", "strtoul", "strtoll", "strtoull", "atoi",
          "atol", "atoll", "bsearch", "qsort", "strtoimax", "strtoumax",
          "imaxabs", "imaxdiv", "atof"]


def run(args, *, expected=0):
    result = subprocess.run(args, cwd=ROOT, text=True, capture_output=True,
                            timeout=30,
                            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0",
                                 "LC_ALL": "C"})
    if result.returncode != expected:
        raise RuntimeError(f"command status {result.returncode}, expected {expected}: "
                           f"{' '.join(map(str, args))}\n{result.stdout}{result.stderr}")
    return result


def compile_object(folder, source, name, definitions=(), includes=True):
    output = folder / (name + ".o")
    command = BASE.copy()
    if includes:
        command += ["-nostdinc", "-I" + str(ROOT / "libc/include"),
                    "-I" + str(ROOT / "include")]
    command += ["-c", str(ROOT / source), "-o", str(output)]
    command += ["-D" + item for item in definitions]
    run(command)
    return output


def link_fixture(folder, source, name, objects, definitions=()):
    output = folder / name
    command = BASE + [str(ROOT / source)] + [str(item) for item in objects]
    command += ["-o", str(output)] + ["-D" + item for item in definitions]
    run(command)
    return output


def main():
    with tempfile.TemporaryDirectory(prefix="mangrove-runtime-foundations-") as temp:
        folder = Path(temp)
        for macro in ("", "-DRUNTIME_MG_FIRST"):
            command = [CC, "-std=c17", "-pedantic", "-Wall", "-Wextra",
                       "-Werror", "-nostdinc", "-I" + str(ROOT / "libc/include"),
                       "-I" + str(ROOT / "include"), macro,
                       "-fsyntax-only", str(ROOT / "tests/runtime_headers_probe.c")]
            run([item for item in command if item])
        headers = folder / "runtime-headers"
        run([CC, "-std=c17", "-pedantic", "-Wall", "-Wextra", "-Werror",
             "-nostdinc", "-I" + str(ROOT / "libc/include"),
             "-I" + str(ROOT / "include"), str(ROOT / "tests/runtime_headers_probe.c"),
             "-o", str(headers)])
        run([headers])
        print("C17 headers, ABI types, include order and va_copy: PASS")

        errno = compile_object(folder, "libc/src/errno.c", "errno",
                               ["__errno_location=probe_errno_location"])

        ctype_objects = [compile_object(folder, "libc/src/ctype.c", "ctype",
                        [name + "=probe_" + name for name in CTYPE])]
        stdlib_objects = [compile_object(folder, "libc/src/stdlib.c", "stdlib",
                         ["__errno_location=probe_errno_location"] +
                         [name + "=probe_" + name for name in STDLIB + CTYPE])]
        float_object = compile_object(folder, "libc/src/floatconv.c", "floatconv",
                                      ["__errno_location=probe_errno_location"])
        libc_probe = link_fixture(folder, "tests/runtime_libc_probe.c", "runtime-libc",
                                  ctype_objects + stdlib_objects + [float_object, errno])
        run([libc_probe])
        print("ASCII C ctype, integer conversion, errno, qsort/bsearch and rand: PASS")

        memory = compile_object(folder, "libc/src/allocator.c", "allocator",
                                ["__errno_location=probe_errno_location"] +
                                [name + "=probe_" + name for name in
                                 ["malloc", "calloc", "realloc", "free", "memset", "memcpy"]])
        string = compile_object(folder, "libc/src/string.c", "string",
                                [name + "=probe_" + name for name in
                                 ["strlen", "strcmp", "strncmp", "strchr", "strcpy",
                                  "strncpy", "strncat", "memset", "memcpy", "memmove", "memcmp"]])
        memory_probe = link_fixture(folder, "tests/runtime_allocator_probe.c",
                                    "runtime-allocator", [memory, string, errno])
        run([memory_probe])
        print("Allocator alignment, calloc overflow, failed realloc preservation: PASS")

        exit_defs = ["__errno_location=probe_errno_location"] + [
            f"{name}=probe_{name}" for name in
            ["abort", "atexit", "at_quick_exit", "exit", "quick_exit", "_Exit"]]
        exit_object = compile_object(folder, "libc/src/exit.c", "exit", exit_defs)
        exit_probe = link_fixture(folder, "tests/runtime_exit_probe.c", "runtime-exit",
                                  [exit_object], exit_defs)
        for mode in ("normal", "quick", "immediate", "capacity"):
            run([exit_probe, mode])
        run([exit_probe, "abort"], expected=-signal.SIGILL)
        print("Exit status, callback order/registration/capacity and abort trap: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
