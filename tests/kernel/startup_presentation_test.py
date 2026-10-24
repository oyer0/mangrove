#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check normal startup output and optional-network warning classification."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"


def production_function(path, name):
    source = (ROOT / path).read_text()
    pattern = r"^(?:static )?[A-Za-z_][^\n;{}]*\b" + name + r"\s*\("
    for match in re.finditer(pattern, source, re.M):
        brace = source.index("{", match.end())
        if ";" in source[match.end():brace]:
            continue
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end] + "\n"
    raise ValueError(f"missing production function {path}:{name}")


main_source = (ROOT / "kernel/src/main.c").read_text()
init_source = (ROOT / "kernel/src/init.c").read_text()
assert 'kprint("%s %s\\n", MANGROVE_NAME, MANGROVE_VERSION);' in main_source
for routine in ("Starting system...", "Input ready\\n", "Platform ready\\n",
                "Starting Sprout...\\n"):
    assert routine not in main_source, f"routine startup message remains: {routine}"
for routine in ("Storage ready\\n", "Network ready\\n", "Filesystem ready\\n",
                "[ALLOC] hybrid slab backend active"):
    assert routine not in init_source, f"routine startup message remains: {routine}"
assert '[WARN] Kernel boot log could not be saved\\n' in init_source
assert '[WARN] hybrid slab backend unavailable' in init_source
assert '[FAIL] %s unavailable: %s\\n' in init_source
assert '[WARN] ACPI platform unavailable\\n' in main_source
assert '[WARN] realtime clock unavailable\\n' in main_source
assert '[WARN] monotonic platform timing unavailable\\n' in main_source

function = production_function("kernel/src/init.c",
                               "init_report_optional_network_status")
prefix = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
enum { INIT_READY = 2, INIT_UNAVAILABLE = 3, INIT_FAILED = 4,
       INIT_SKIPPED = 5, INIT_NETWORK_DEVICE = 0 };
typedef int init_state_t;
typedef struct { int state; } status_t;
typedef struct { status_t status; } descriptor_t;
static descriptor_t descriptors[1];
static char output[128];
static size_t output_size;
static void kprint(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int count = vsnprintf(output + output_size, sizeof(output) - output_size,
                          format, arguments);
    va_end(arguments);
    assert(count >= 0 && (size_t)count < sizeof(output) - output_size);
    output_size += (size_t)count;
}
'''
suffix = r'''
int main(void)
{
    descriptors[INIT_NETWORK_DEVICE].status.state = INIT_READY;
    init_report_optional_network_status();
    assert(output_size == 0U);

    descriptors[INIT_NETWORK_DEVICE].status.state = INIT_UNAVAILABLE;
    init_report_optional_network_status();
    assert(strcmp(output, "[WARN] Network unavailable\n") == 0);

    output_size = 0U;
    output[0] = '\0';
    descriptors[INIT_NETWORK_DEVICE].status.state = INIT_SKIPPED;
    init_report_optional_network_status();
    assert(strcmp(output, "[WARN] Network unavailable\n") == 0);
    puts("startup presentation: silent success and visible warning passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="mangrove-startup-presentation-") as tmp:
    source = Path(tmp) / "startup.c"
    binary = Path(tmp) / "startup-test"
    source.write_text(prefix + function + suffix)
    subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(source), "-o",
                    str(binary)], check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=30,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
