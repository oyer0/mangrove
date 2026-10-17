#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Sanitized libc stdio/float conversion tests against the host C library."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CC = sys.argv[1] if len(sys.argv) > 1 else "gcc"
FLAGS = [CC, "-std=gnu17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
         "-fno-builtin", "-fsanitize=address,undefined",
         "-fno-sanitize-recover=all", "-I" + str(ROOT / "libc/include"),
         "-I" + str(ROOT / "include")]


def compile_obj(folder, source, name, definitions=()):
    out = folder / (name + ".o")
    command = FLAGS + ["-D" + item for item in definitions]
    command += ["-c", str(ROOT / source), "-o", str(out)]
    subprocess.run(command, cwd=ROOT, check=True, timeout=30)
    return out


def main():
    with tempfile.TemporaryDirectory(prefix="mangrove-stdio-") as temp:
        folder = Path(temp)
        errno = compile_obj(folder, "libc/src/errno.c", "errno",
                            ["__errno_location=probe_errno_location"])
        error = compile_obj(folder, "libc/src/error.c", "error",
                            ["__errno_location=probe_errno_location",
                             "strerror=probe_strerror"])
        formatter = compile_obj(folder, "libc/src/stdio.c", "formatter",
                                ["__errno_location=probe_errno_location",
                                 "printf=probe_printf", "snprintf=probe_snprintf",
                                 "perror=probe_perror", "strerror=probe_strerror"])
        streams = compile_obj(folder, "libc/src/stdio_stream.c", "streams",
                              ["__errno_location=probe_errno_location",
                               "malloc=test_malloc", "free=test_free"])
        scanner = compile_obj(folder, "libc/src/scanf.c", "scanner",
                              ["__errno_location=probe_errno_location",
                               "malloc=test_malloc", "realloc=test_realloc",
                               "free=test_free"])
        floats = compile_obj(folder, "libc/src/floatconv.c", "floatconv",
                             ["__errno_location=probe_errno_location",
                              "malloc=test_malloc", "free=test_free"])
        conversion_names = ["strtol", "strtoul", "strtoll", "strtoull",
                            "strtoimax", "strtoumax", "strtof", "strtod",
                            "strtold", "atoi", "atol", "atoll", "abs", "labs",
                            "llabs", "div", "ldiv", "lldiv", "imaxabs", "imaxdiv", "atof",
                            "rand", "srand", "getenv", "system", "bsearch", "qsort"]
        conversions = compile_obj(folder, "libc/src/stdlib.c", "conversions",
                                  ["__errno_location=probe_errno_location"] +
                                  [name + "=probe_" + name for name in conversion_names])
        binary = folder / "runtime-stdio"
        subprocess.run(FLAGS + [str(ROOT / "tests/runtime_stdio_probe.c"),
            str(ROOT / "tests/runtime_stdio_adapter.c"), str(errno), str(error),
            str(formatter), str(streams), str(scanner), str(floats),
            str(conversions), "-ldl", "-lm", "-o", str(binary)],
            cwd=ROOT, check=True, timeout=30)
        result = subprocess.run([str(binary)], cwd=ROOT, text=True,
            capture_output=True, timeout=30,
            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0", "LC_ALL": "C"})
        if result.returncode:
            raise RuntimeError(f"runtime stdio check failed ({result.returncode})\n"
                               f"{result.stdout}{result.stderr}")
        print(result.stdout, end="")
        print("Streams, formatted I/O, scanning and floating conversions: PASS")


if __name__ == "__main__":
    main()
