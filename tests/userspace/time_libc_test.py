#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host-reference checks for the production C time implementation."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else "gcc"
ALIASES = {
    "clock": "mg_time_clock",
    "difftime": "mg_time_difftime",
    "mktime": "mg_time_mktime",
    "time": "mg_time_time",
    "timespec_get": "mg_time_timespec_get",
    "asctime": "mg_time_asctime",
    "asctime_r": "mg_time_asctime_r",
    "ctime": "mg_time_ctime",
    "ctime_r": "mg_time_ctime_r",
    "gmtime": "mg_time_gmtime",
    "gmtime_r": "mg_time_gmtime_r",
    "localtime": "mg_time_localtime",
    "localtime_r": "mg_time_localtime_r",
    "strftime": "mg_time_strftime",
}


def run(args):
    result = subprocess.run(args, cwd=ROOT, text=True, capture_output=True,
                            timeout=60, env={**os.environ,
                                "ASAN_OPTIONS": "detect_leaks=0",
                                "LC_ALL": "C", "TZ": "UTC"})
    if result.returncode:
        raise RuntimeError(f"{' '.join(map(str,args))}\n"
                           f"{result.stdout}{result.stderr}")
    return result


def main():
    with tempfile.TemporaryDirectory(prefix="mangrove-time-libc-") as temp:
        folder = Path(temp)
        flags = [CC, "-std=gnu17", "-O1", "-g", "-Wall", "-Wextra",
                 "-Werror", "-fno-builtin", "-fsanitize=address,undefined",
                 "-fno-sanitize-recover=all"]
        common = ["-nostdinc", "-I" + str(ROOT / "libc/include"),
                  "-I" + str(ROOT / "include")]
        production = folder / "time-standard.o"
        command = flags + common
        command += ["-D" + source + "=" + alias
                    for source, alias in ALIASES.items()]
        command += ["-D__errno_location=probe_errno_location",
                    "-c", str(ROOT / "libc/src/time_standard.c"),
                    "-o", str(production)]
        run(command)
        conversion = folder / "time-convert.o"
        run(flags + common + ["-c", str(ROOT / "libc/src/time_convert.c"),
                              "-o", str(conversion)])
        output = folder / "time-libc-test"
        run(flags + ["-D_DEFAULT_SOURCE=1",
                     str(ROOT / "tests/userspace/time_libc_test.c"),
                     str(production), str(conversion), "-o", str(output)])
        result = run([output])
        print(result.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
