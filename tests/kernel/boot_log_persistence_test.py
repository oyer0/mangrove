#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production boot-log snapshot writer with bounded I/O stubs."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"
SOURCE = (ROOT / "kernel/src/init.c").read_text()


def production_function(source: str, name: str) -> str:
    pattern = r"^static [^\n;{}]*\b" + re.escape(name) + r"\s*\("
    match = re.search(pattern, source, re.M)
    if not match:
        raise ValueError(f"missing production function {name}")
    brace = source.index("{", match.end())
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


FUNCTION = production_function(SOURCE, "init_write_boot_log_snapshot")
PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;
typedef size_t usize;
typedef struct vfs_node { unsigned identity; } vfs_node_t;
#define X250_BOOT_TRACE 0

static usize snapshot_size;
static unsigned read_calls, allocation_calls, free_calls, write_calls;
static bool allocation_fails, read_is_short;
static u64 write_result;
static u64 write_offset, write_size;
static unsigned char snapshot[65536];
static unsigned char received[65536];

static usize kprint_boot_log_size(void) { return snapshot_size; }
static usize kprint_boot_log_read(usize offset, char *buffer, usize capacity)
{
    read_calls++;
    assert(offset == 0U);
    usize count = snapshot_size < capacity ? snapshot_size : capacity;
    if (read_is_short && count) count--;
    memcpy(buffer, snapshot, count);
    return count;
}
static void *kmalloc(usize size)
{
    allocation_calls++;
    assert(size == snapshot_size);
    if (allocation_fails) return NULL;
    return malloc(size);
}
static void kfree(void *buffer)
{
    if (buffer) { free_calls++; free(buffer); }
}
static u64 vfs_write(vfs_node_t *node, u64 offset, u64 size,
                     const void *buffer)
{
    assert(node && node->identity == 0x250U);
    write_calls++;
    write_offset = offset;
    write_size = size;
    assert(size <= sizeof(received));
    memcpy(received, buffer, (usize)size);
    return write_result == UINT64_MAX ? size : write_result;
}

static void reset_fixture(usize size)
{
    snapshot_size = size;
    read_calls = allocation_calls = free_calls = write_calls = 0;
    allocation_fails = read_is_short = false;
    write_result = UINT64_MAX;
    write_offset = write_size = 0;
    memset(snapshot, 0, sizeof(snapshot));
    memset(received, 0, sizeof(received));
    for (usize index = 0; index < size; index++)
        snapshot[index] = (unsigned char)((index * 37U + 11U) & 0xffU);
}
'''
SUFFIX = r'''
int main(void)
{
    vfs_node_t node = { .identity = 0x250U };

    reset_fixture(sizeof(snapshot));
    assert(init_write_boot_log_snapshot(&node));
    assert(read_calls == 1U && allocation_calls == 1U && free_calls == 1U);
    assert(write_calls == 1U && write_offset == 0U &&
           write_size == sizeof(snapshot));
    assert(memcmp(received, snapshot, sizeof(snapshot)) == 0);

    reset_fixture(0U);
    assert(init_write_boot_log_snapshot(&node));
    assert(read_calls == 0U && allocation_calls == 0U && free_calls == 0U &&
           write_calls == 0U);

    reset_fixture(1024U);
    allocation_fails = true;
    assert(!init_write_boot_log_snapshot(&node));
    assert(read_calls == 0U && write_calls == 0U && free_calls == 0U);

    reset_fixture(2048U);
    read_is_short = true;
    assert(!init_write_boot_log_snapshot(&node));
    assert(read_calls == 1U && write_calls == 0U && free_calls == 1U);

    reset_fixture(8192U);
    write_result = snapshot_size - 1U;
    assert(!init_write_boot_log_snapshot(&node));
    assert(read_calls == 1U && write_calls == 1U && free_calls == 1U);
    assert(write_size == snapshot_size);

    puts("boot-log persistence: one bounded snapshot write, exact bytes, zero-length, allocation/read/write failures and cleanup passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="mangrove-boot-log-persistence-") as temp:
    source = Path(temp) / "boot_log_persistence.c"
    binary = Path(temp) / "boot_log_persistence"
    source.write_text(PREFIX + FUNCTION + SUFFIX)
    subprocess.run([
        CC, "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-Ikernel/include", "-Iinclude",
        str(source), "-o", str(binary),
    ], cwd=ROOT, check=True, timeout=30)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=30,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
