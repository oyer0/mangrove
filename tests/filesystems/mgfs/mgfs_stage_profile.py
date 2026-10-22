#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Time and count production MGFS work in temporary instrumented source copies.

The same sanitizers, optimization and fixture groups are used for both revisions.
Headers come from this checkout; --revision selects only MGFS and its fixture.
No benchmark hooks or counters enter target binaries. Block/heap/lock/pin
fixtures have the same limitations as mgfs_stage_cancel_test.py.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[2] / "shared"))
from host_build import dead_strip_flags

root = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("compiler", nargs="?", default="gcc")
parser.add_argument("--revision", help="read MGFS and fixture from this Git revision")
args = parser.parse_args()


def read(path):
    if args.revision:
        return subprocess.check_output(
            ["git", "show", args.revision + ":" + path], cwd=root, text=True)
    return (root / path).read_text()


def instrument(source, name, statement):
    match = re.search(r"^static bool " + name + r"\([^;{}]+\n\{", source, re.M)
    if not match:
        raise RuntimeError("missing production function: " + name)
    return source[:match.end()] + "\n    " + statement + source[match.end():]


source = read("kernel/src/storage/mgfs.c")
source = "static unsigned long long profile_reads, profile_checks, profile_bytes, profile_walks;\n" + source
source = instrument(source, "mgfs_read_block", "profile_reads++;")
source = instrument(source, "mgfs_checksum_block", "profile_checks++; profile_bytes += covered_bytes;")
# Count the full chain validator, not the compatibility wrapper.
walker = "mgfs_read_directory_mapping" if "static bool mgfs_read_directory_mapping(" in source else "mgfs_read_directory_block"
source = instrument(source, walker, "profile_walks++;")
fixture = read("tests/filesystems/mgfs/mgfs_stage_cancel_test.c")
fixture = fixture.replace('void *kmalloc(usize size)\n{',
                          'void *kmalloc(usize size)\n{\n    profile_allocations++; profile_requested += size;')
for name in ("version_admission_tests", "publication_tests", "large_replacement_tests",
             "create_contents_tests", "large_create_tests"):
    call = "    " + name + "();"
    if call not in fixture:
        raise RuntimeError("missing fixture group: " + name)
    fixture = fixture.replace(call, '''    {
        clock_t cpu = clock(); struct timespec a, b;
        assert(!clock_gettime(CLOCK_MONOTONIC, &a));
        unsigned long long r = profile_reads, c = profile_checks,
            z = profile_bytes, w = profile_walks, n = profile_allocations,
            bytes = profile_requested;
        ''' + name + '''();
        assert(!clock_gettime(CLOCK_MONOTONIC, &b));
        fprintf(stderr, "PROFILE ''' + name + ''' cpu=%.6f wall=%.6f reads=%llu checks=%llu checksum_bytes=%llu directory_walks=%llu allocations=%llu requested_bytes=%llu\\n",
            (double)(clock() - cpu) / CLOCKS_PER_SEC,
            (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9,
            profile_reads-r, profile_checks-c, profile_bytes-z, profile_walks-w,
            profile_allocations-n, profile_requested-bytes);
    }''', 1)
with tempfile.TemporaryDirectory(prefix="mangrove-mgfs-profile-") as directory:
    temp = Path(directory)
    production = temp / "mgfs.c"
    production.write_text(source)
    fixture = fixture.replace('#include "../../../kernel/src/storage/mgfs.c"',
        '#include <time.h>\nstatic unsigned long long profile_allocations, profile_requested;\n'
        + '#include "' + str(production) + '"')
    test = temp / "fixture.c"
    test.write_text(fixture)
    executable = temp / "profile"
    subprocess.run([args.compiler, "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-ffunction-sections", "-fdata-sections",
                    "-fsanitize=address,undefined", *dead_strip_flags(),
                    "-I" + str(root / "kernel/include"), "-I" + str(root / "include"),
                    "-idirafter", str(root / "libc/include"), str(test),
                    str(root / "libc/src/utf8.c"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
