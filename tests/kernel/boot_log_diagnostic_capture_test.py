#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production opt-in boot-log rotation and completion writer."""

from pathlib import Path
import re
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"
SOURCE = (ROOT / "kernel/src/init.c").read_text()


def production_function(name):
    match = re.search(r"^static bool " + re.escape(name) + r"\s*\(",
                      SOURCE, re.M)
    if not match:
        raise ValueError(f"missing production function {name}")
    brace = SOURCE.index("{", match.end())
    end, depth = brace + 1, 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[match.start():end] + "\n"


FUNCTIONS = production_function("init_prepare_boot_log_file") + \
    production_function("init_write_boot_log_diagnostic")
PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define X250_BOOT_TRACE 1
#include <x250_boot_trace.h>
#include <boot_log_diagnostics.h>
#define INIT_BOOT_LOG_MGFS_BLOCK_BYTES 4096U
#define VFS_OK 0
#define VFS_ERR_NOT_FOUND (-2)
#define VFS_ERR_IO (-3)
#define VFS_TYPE_FILE 1
#define VFS_TYPE_DIRECTORY 2
typedef struct vfs_node { int type; u64 size; int tag; } vfs_node_t;

static vfs_node_t current_node, previous_node, new_node, logs_node;
static bool current_exists, previous_exists, lookup_error;
static bool rename_fails, allocation_fails, short_snapshot;
static bool trace_complete_setting;
static unsigned unlink_calls, rename_calls, create_calls;
static unsigned write_calls;
static unsigned fail_write_call;
static u64 timer_now;
static x250_boot_trace_measurement_snapshot_t measurement_fixture;
static char snapshot_data[128] = "kernel boot snapshot\n";
static const char trace_data[] = "47 [INFO] USB BOT commands=3\n";
static unsigned char persisted[131072];
static u64 persisted_size;

static int vfs_lookup(const char *path, vfs_node_t **out)
{
    if (lookup_error) return VFS_ERR_IO;
    if (!strcmp(path, "/sys/logs/boot.log")) {
        if (!current_exists) return VFS_ERR_NOT_FOUND;
        *out = &current_node;
        return VFS_OK;
    }
    if (!strcmp(path, "/sys/logs/boot.previous.log")) {
        if (!previous_exists) return VFS_ERR_NOT_FOUND;
        *out = &previous_node;
        return VFS_OK;
    }
    return VFS_ERR_IO;
}
static int vfs_unlink(vfs_node_t *dir, const char *name)
{
    assert(dir == &logs_node && !strcmp(name, "boot.previous.log"));
    unlink_calls++;
    previous_exists = false;
    return VFS_OK;
}
static int vfs_rename(vfs_node_t *src, const char *old_name,
                      vfs_node_t *dst, const char *new_name)
{
    assert(src == &logs_node && dst == &logs_node);
    assert(!strcmp(old_name, "boot.log") &&
           !strcmp(new_name, "boot.previous.log"));
    rename_calls++;
    if (rename_fails) return VFS_ERR_IO;
    previous_node = current_node;
    previous_exists = true;
    current_exists = false;
    return VFS_OK;
}
static int vfs_create(vfs_node_t *dir, const char *name, vfs_node_t **out)
{
    assert(dir == &logs_node && !strcmp(name, "boot.log"));
    create_calls++;
    new_node = (vfs_node_t){.type = VFS_TYPE_FILE, .tag = 3};
    current_exists = true;
    *out = &new_node;
    return VFS_OK;
}
static void *kmalloc(usize size)
{
    if (allocation_fails) return NULL;
    assert(size <= sizeof(persisted));
    return malloc((size_t)size);
}
static void kfree(void *ptr) { free(ptr); }
static usize kprint_boot_log_read(usize offset, char *buffer, usize capacity)
{
    assert(offset == 0U);
    usize size = strlen(snapshot_data);
    if (size > capacity) size = capacity;
    if (short_snapshot && size) size--;
    memcpy(buffer, snapshot_data, (size_t)size);
    return size;
}
static u64 timer_uptime_ms(void) { return timer_now += 5U; }
usize x250_boot_trace_export(char *buffer, usize capacity,
                             u32 *line_count, bool *complete)
{
    usize size = sizeof(trace_data) - 1U;
    assert(size <= capacity);
    memcpy(buffer, trace_data, (size_t)size);
    *line_count = 1U;
    *complete = trace_complete_setting;
    return size;
}
static void xhci_storage_trace_begin(void) { }
static void xhci_storage_trace_end(xhci_storage_trace_snapshot_t *io)
{
    memset(io, 0, sizeof(*io));
    io->block_write_requests = 2U;
    io->block_write_sectors = 16U;
    io->bot_commands = 3U;
    io->bot_write10 = 2U;
    io->bot_elapsed_ms = 240U;
}
void x250_boot_trace_storage_io(u64 now, const char *phase,
    const xhci_storage_trace_snapshot_t *io)
{ (void)now; (void)phase; (void)io; }
void x250_boot_trace_timing_value(u64 now, const char *event,
    const char *value, u64 count, bool valid, u64 duration,
    const char *state)
{ (void)now; (void)event; (void)value; (void)count; (void)valid;
  (void)duration; (void)state; }
void x250_boot_trace_timing(u64 now, const char *event, u32 port,
    bool valid, u64 duration, const char *state)
{ (void)now; (void)event; (void)port; (void)valid; (void)duration;
  (void)state; }
void x250_boot_trace_measurements_begin(void) { }
void x250_boot_trace_measurement_note(
    x250_boot_trace_measurement_kind_t kind, u64 elapsed_ms)
{ (void)kind; (void)elapsed_ms; }
void x250_boot_trace_measurements_end(
    x250_boot_trace_measurement_snapshot_t *snapshot)
{ if (snapshot) *snapshot = measurement_fixture; }
static u64 vfs_write(vfs_node_t *node, u64 offset, u64 size,
                     const void *buffer)
{
    assert(node == &new_node && size <= sizeof(persisted));
    write_calls++;
    if (write_calls == 1U) {
        persisted_size = size;
        memset(persisted, 0, sizeof(persisted));
        memcpy(persisted, buffer, (size_t)size);
        return fail_write_call == write_calls ? size - 1U : size;
    }
    assert(write_calls == 2U && offset + size == persisted_size);
    if (fail_write_call == write_calls) return size - 1U;
    memcpy(persisted + offset, buffer, (size_t)size);
    return size;
}

static void reset_fixture(void)
{
    current_node = (vfs_node_t){.type = VFS_TYPE_FILE, .size = 111U, .tag = 1};
    previous_node = (vfs_node_t){.type = VFS_TYPE_FILE, .size = 222U, .tag = 2};
    new_node = (vfs_node_t){.type = VFS_TYPE_FILE, .tag = 3};
    logs_node = (vfs_node_t){.type = VFS_TYPE_DIRECTORY};
    current_exists = previous_exists = lookup_error = false;
    rename_fails = allocation_fails = short_snapshot = false;
    trace_complete_setting = true;
    unlink_calls = rename_calls = create_calls = write_calls = 0U;
    fail_write_call = 0U;
    timer_now = 1000U;
    memset(&measurement_fixture, 0, sizeof(measurement_fixture));
    persisted_size = 0U;
    memset(persisted, 0, sizeof(persisted));
}
'''
SUFFIX = r'''
static u32 read_u32(const u8 *p)
{ return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 read_u64(const u8 *p)
{ return (u64)read_u32(p) | ((u64)read_u32(p + 4U) << 32); }
int main(void)
{
    vfs_node_t *file = NULL;
    u64 old_size = 0;
    bool old_present = false;

    reset_fixture();
    current_exists = previous_exists = true;
    current_node.size = 1000U;
    assert(init_prepare_boot_log_file(&logs_node, &file, &old_size,
                                      &old_present));
    assert(old_present && old_size == 1000U && unlink_calls == 1U &&
           rename_calls == 1U && create_calls == 1U && file == &new_node);
    assert(previous_exists && previous_node.size == 1000U && current_exists);

    reset_fixture();
    previous_exists = true;
    assert(init_prepare_boot_log_file(&logs_node, &file, &old_size,
                                      &old_present));
    assert(old_present == false && unlink_calls == 0U && rename_calls == 0U &&
           previous_exists && create_calls == 1U);

    reset_fixture();
    current_exists = true;
    rename_fails = true;
    assert(!init_prepare_boot_log_file(&logs_node, &file, &old_size,
                                       &old_present));
    assert(current_exists && create_calls == 0U);

    reset_fixture();
    lookup_error = true;
    assert(!init_prepare_boot_log_file(&logs_node, &file, &old_size,
                                       &old_present));
    assert(unlink_calls == 0U && rename_calls == 0U && create_calls == 0U);

    reset_fixture();
    x250_boot_trace_measurement_snapshot_t preparation_measurements = {0};
    preparation_measurements.kinds[
        X250_BOOT_TRACE_MEASURE_BLOCK_IO_GATE_ACQUIRE] =
        (x250_boot_trace_measurement_stats_t){1U, 2U, 3U};
    measurement_fixture.kinds[X250_BOOT_TRACE_MEASURE_MGFS_MUTATION] =
        (x250_boot_trace_measurement_stats_t){4U, 5U, 6U};
    assert(init_write_boot_log_diagnostic(&new_node, strlen(snapshot_data),
                                          55U, 900U,
                                          &preparation_measurements));
    assert(write_calls == 2U && persisted_size % 4096U == 0U);
    assert(memcmp(persisted + strlen(snapshot_data),
                  BOOT_LOG_DIAGNOSTIC_TRACE_HEADER,
                  sizeof(BOOT_LOG_DIAGNOSTIC_TRACE_HEADER) - 1U) == 0);
    assert(memcmp(persisted + strlen(snapshot_data) +
                  sizeof(BOOT_LOG_DIAGNOSTIC_TRACE_HEADER) - 1U,
                  trace_data, sizeof(trace_data) - 1U) == 0);
    u64 footer_offset = persisted_size - BOOT_LOG_DIAGNOSTIC_FOOTER_BYTES;
    const u8 *footer = persisted + footer_offset;
    assert(memcmp(footer, "MGBLOG01", 8U) == 0);
    assert(read_u32(footer + 16U) == 1U && read_u32(footer + 20U) == 0U);
    assert(read_u32(footer + 64U) == 1U);
    assert(read_u32(footer + 112U) == 2U);
    assert(read_u32(footer + 176U) == 3U);
    assert(read_u32(footer + 192U) == 2U);
    assert(read_u64(footer + 40U) == 55U);
    assert(read_u64(footer + 48U) > 0U);
    assert(read_u64(footer + 272U) == 1U &&
           read_u64(footer + 280U) == 2U &&
           read_u64(footer + 288U) == 3U);
    assert(read_u64(footer + 440U + 5U * 24U) == 4U &&
           read_u64(footer + 448U + 5U * 24U) == 5U &&
           read_u64(footer + 456U + 5U * 24U) == 6U);

    reset_fixture();
    trace_complete_setting = false;
    assert(init_write_boot_log_diagnostic(&new_node, strlen(snapshot_data),
                                          55U, 900U,
                                          &preparation_measurements));
    footer_offset = persisted_size - BOOT_LOG_DIAGNOSTIC_FOOTER_BYTES;
    footer = persisted + footer_offset;
    assert(read_u32(footer + 16U) == 1U && read_u32(footer + 20U) == 1U);

    reset_fixture();
    allocation_fails = true;
    assert(!init_write_boot_log_diagnostic(&new_node, strlen(snapshot_data),
                                          55U, 900U,
                                          &preparation_measurements));
    assert(write_calls == 0U);

    reset_fixture();
    short_snapshot = true;
    assert(!init_write_boot_log_diagnostic(&new_node, strlen(snapshot_data),
                                          55U, 900U,
                                          &preparation_measurements));
    assert(write_calls == 0U);

    reset_fixture();
    fail_write_call = 1U;
    assert(!init_write_boot_log_diagnostic(&new_node, strlen(snapshot_data),
                                          55U, 900U,
                                          &preparation_measurements));
    footer = persisted + persisted_size - BOOT_LOG_DIAGNOSTIC_FOOTER_BYTES;
    assert(write_calls == 1U && memcmp(footer, "MGBLOG01", 8U) == 0 &&
           read_u32(footer + 16U) == 0U);

    reset_fixture();
    fail_write_call = 2U;
    assert(!init_write_boot_log_diagnostic(&new_node, strlen(snapshot_data),
                                          55U, 900U,
                                          &preparation_measurements));
    footer = persisted + persisted_size - BOOT_LOG_DIAGNOSTIC_FOOTER_BYTES;
    assert(write_calls == 2U && memcmp(footer, "MGBLOG01", 8U) == 0 &&
           read_u32(footer + 16U) == 0U);

    puts("diagnostic boot log: rotation, bounded aligned write, footer completion and write-failure controls passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="mangrove-boot-diagnostic-capture-") as temp:
    source = Path(temp) / "capture.c"
    binary = Path(temp) / "capture"
    source.write_text(PREFIX + FUNCTIONS + SUFFIX)
    subprocess.run([
        CC, "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-Ikernel/include",
        "-Ikernel/include/usb", "-Iinclude", str(source), "-o", str(binary),
    ], cwd=ROOT, check=True, timeout=30)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=30,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
