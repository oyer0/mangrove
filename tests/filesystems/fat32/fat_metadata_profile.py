#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Profile production FAT metadata requests using disposable instrumented fixtures.

No physical block cache, real mutex waits or guest timing is measured. --revision
selects both production source and its matching fixture. Target files are not
instrumented. Allocation/read counters include failed attempts.
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
parser.add_argument("--revision")
parser.add_argument("--filesystem", choices=("fat32", "exfat"))
parser.add_argument("--enforce-write-budgets", action="store_true",
                    help="enforce coalesced 2 MiB transaction write budgets")
parser.add_argument("--enforce-budgets", action="store_true",
                    help="also enforce new directory budgets against the old source")
args = parser.parse_args()


def read(path):
    if args.revision:
        return subprocess.check_output(["git", "show", args.revision + ":" + path],
                                       cwd=root, text=True)
    return (root / path).read_text()


def function_span(source, name):
    match = re.search(r"^(?:static )?(?:bool|int|void) " + name + r"\([^;{}]+\n\{", source, re.M)
    if not match:
        raise RuntimeError("missing function " + name)
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return match, end


with tempfile.TemporaryDirectory(prefix="mangrove-fat-profile-") as directory:
    temp = Path(directory)
    for kind in ("fat32", "exfat"):
        if args.filesystem and kind != args.filesystem:
            continue
        source = read("kernel/src/storage/" + kind + ".c")
        source = '''#include <stdio.h>
#include <time.h>
static unsigned long long pr_reads, pr_writes, pr_fat, pr_repeat, pr_alloc;
static unsigned long long pr_seen[16384];
static unsigned pr_depth;
''' + source
        reader = kind + ("_mounted_read" if kind == "fat32" else "_read")
        writer = kind + ("_mounted_write" if kind == "fat32" else "_write")
        getter = kind + ("_get_next_checked" if kind == "fat32" else "_fat_get")
        if "static " + ("int " if kind == "fat32" else "bool ") + getter + "_view(" in source:
            getter += "_view"
        for name, statement in ((reader, '''pr_reads += count;
    for (u32 i=0; i<count; i++) if (lba+i<16384) {
        if (pr_seen[lba+i]++) pr_repeat++;
    }'''), (writer, "pr_writes += count;"), (getter, "pr_fat++;")):
            match, _ = function_span(source, name)
            source = source[:match.end()] + "\n    " + statement + source[match.end():]
        # Timed wrappers preserve all production returns and failure assertions.
        timed = [kind + "_replace_locked", kind + "_create_contents_locked",
                 kind + "_ownership_preflight_locked"]
        if kind == "exfat":
            timed += ["exfat_load_upcase", "exfat_read_system_entries"]
        for name in timed:
            match, end = function_span(source, name)
            signature = source[match.start():source.index("{", match.start())].strip()
            parameters = signature[signature.index("(")+1:signature.rindex(")")]
            names = [re.findall(r"[A-Za-z_][A-Za-z_0-9]*", p)[-1] for p in parameters.split(",")]
            original = source[match.start():end].replace(name + "(", name + "_profile_inner(", 1)
            length = "length" if "length" in names else (
                "(fs ? fs->upcase_length : 0ULL)" if name == "exfat_load_upcase" else "0ULL")
            budget = ""
            if args.enforce_write_budgets and "length" in names:
                ceiling = 8500 if name == "fat32_create_contents_locked" else 12500
                budget = ("    if (result == VFS_OK && length == 2097152ULL) "
                          f"assert(pr_writes-w < {ceiling});\n")
            wrapper = '\n' + signature + '''\n{
    if (!pr_depth++) memset(pr_seen,0,sizeof(pr_seen));
    clock_t cpu=clock(); struct timespec a,b;
    assert(!clock_gettime(CLOCK_MONOTONIC,&a));
    unsigned long long r=pr_reads,w=pr_writes,f=pr_fat,p=pr_repeat,n=pr_alloc;
    int result = ''' + name + '_profile_inner(' + ','.join(names) + ''');
    assert(!clock_gettime(CLOCK_MONOTONIC,&b));
    pr_depth--;
    fprintf(stderr,"PROFILE ''' + name + ''' length=%llu result=%d reads=%llu writes=%llu FAT_entries=%llu repeated_sector_loads=%llu allocations=%llu cpu=%.6f wall=%.6f\\n",
        (unsigned long long)''' + length + ''',result,pr_reads-r,pr_writes-w,pr_fat-f,pr_repeat-p,pr_alloc-n,
        (double)(clock()-cpu)/CLOCKS_PER_SEC,(b.tv_sec-a.tv_sec)+(b.tv_nsec-a.tv_nsec)/1e9);
''' + budget + '''    return result;
}
'''
            source = source[:match.start()] + original + wrapper + source[end:]
        production = temp / (kind + ".c")
        production.write_text(source)
        fixture = read("tests/filesystems/" + kind + "/" + kind + "_mount_identity_test.c")
        fixture = fixture.replace('#include "../../../kernel/src/storage/' + kind + '.c"',
                                  '#include "' + str(production) + '"')
        if args.revision and "static void metadata_directory_workload(" not in fixture:
            # Add the same new public-operation workload to the historical
            # fixture. Only its performance budget is disabled for measurement;
            # all content/collision/corruption assertions remain enabled.
            current = (root / ("tests/filesystems/" + kind + "/" + kind + "_mount_identity_test.c")).read_text()
            match, end = function_span(current, "metadata_directory_workload")
            workload = current[match.start():end]
            if not args.enforce_budgets:
                workload, count = re.subn(r"    assert\(reads - before_reads < [0-9]+\);",
                    "    /* Baseline benchmark: measure before enforcing this budget. */", workload)
                assert count == 1
            position = fixture.index("int main(")
            fixture = fixture[:position] + workload + "\n" + fixture[position:]
            position = fixture.index("{", fixture.index("int main("))
            fixture = fixture[:position+1] + "\n    metadata_directory_workload(false);\n    metadata_directory_workload(true);" + fixture[position+1:]
        marker = "void *kmalloc(usize size)\n{"
        assert marker in fixture
        fixture = fixture.replace(marker, marker + "\n    pr_alloc++;")
        test = temp / (kind + "_fixture.c")
        test.write_text(fixture)
        executable = temp / kind
        subprocess.run([args.compiler, "-O1", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-Wno-type-limits", "-ffunction-sections",
                        "-fdata-sections", "-fsanitize=address,undefined", *dead_strip_flags(),
                        "-I" + str(root / "kernel/src/storage"), "-I" + str(root / "kernel/include"), "-I" + str(root / "include"),
                        "-idirafter", str(root / "libc/include"), str(test), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
