#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production init callback result propagation into boot tracing."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"


def production_function(source, name):
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


source = (ROOT / "kernel/src/init.c").read_text()
functions = "\n".join(production_function(source, name) for name in (
    "init_id_valid", "init_state_name", "init_trace_status",
    "init_start_node"))
prefix = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <init.h>

#define INIT_BIT(id) (1ULL << (id))

typedef init_result_t (*init_start_fn)(const char **reason);
typedef struct {
    init_status_t status;
    u64 required_deps;
    u64 optional_deps;
    bool required_for_boot;
    init_start_fn start;
} init_descriptor_t;

static init_descriptor_t descriptors[INIT_COUNT];
static bool init_graph_error;
static u32 begin_calls;
static u32 gop_inventory_calls;
static u32 enter_calls;
static u32 result_calls;
static bool last_passed;
static bool last_required;
static bool last_status_available;
static int last_status;
static char last_name[64];
static char last_state[32];
static char last_reason[128];
static init_result_t callback_result;
static const char *callback_reason;

static void x250_boot_trace_begin(void) { begin_calls++; }
static void x250_boot_trace_gop_modes(void) { gop_inventory_calls++; }
static void x250_boot_trace_stage(u32 checkpoint, bool passed,
                                 const char *description)
{
    assert(checkpoint == 37U);
    assert(!passed);
    (void)description;
    enter_calls++;
}
static void x250_boot_trace_result(u32 checkpoint, bool passed,
                                   const char *description, bool required,
                                   bool status_available, int status,
                                   const char *state, const char *reason)
{
    assert(checkpoint == 37U);
    result_calls++;
    last_passed = passed;
    last_required = required;
    last_status_available = status_available;
    last_status = status;
    snprintf(last_name, sizeof(last_name), "%s", description);
    snprintf(last_state, sizeof(last_state), "%s", state ? state : "");
    snprintf(last_reason, sizeof(last_reason), "%s", reason ? reason : "");
}
static void kprint(const char *format, ...) { (void)format; }
static init_result_t callback(const char **reason)
{
    *reason = callback_reason;
    return callback_result;
}
static init_result_t callback_ok(const char **reason)
{
    *reason = NULL;
    return INIT_RESULT_OK;
}
static void reset_trace(void)
{
    begin_calls = enter_calls = result_calls = 0;
    gop_inventory_calls = 0;
    last_passed = last_required = last_status_available = false;
    last_status = 0;
    last_name[0] = last_state[0] = last_reason[0] = '\0';
}
static void reset_nodes(void)
{
    memset(descriptors, 0, sizeof(descriptors));
    init_graph_error = false;
    reset_trace();
}
'''
suffix = r'''
int main(void)
{
    reset_nodes();
    descriptors[INIT_XHCI].status.name = "xHCI";
    descriptors[INIT_XHCI].start = callback;
    descriptors[INIT_XHCI].required_for_boot = false;
    callback_result = INIT_RESULT_FAILED;
    callback_reason = "controller initialization failed";
    assert(!init_start_node(INIT_XHCI));
    assert(begin_calls == 1U && gop_inventory_calls == 1U &&
           enter_calls == 1U && result_calls == 1U);
    assert(!last_passed && !last_required && last_status_available);
    assert(last_status == INIT_RESULT_FAILED);
    assert(strcmp(last_name, "xHCI") == 0);
    assert(strcmp(last_state, "FAILED") == 0);
    assert(strcmp(last_reason, "controller initialization failed") == 0);

    reset_nodes();
    descriptors[INIT_NETWORK_DEVICE].status.name = "network device";
    descriptors[INIT_NETWORK_DEVICE].start = callback;
    descriptors[INIT_NETWORK_DEVICE].required_for_boot = false;
    callback_result = INIT_RESULT_UNAVAILABLE;
    callback_reason = "no supported Ethernet controller";
    assert(!init_start_node(INIT_NETWORK_DEVICE));
    assert(begin_calls == 0U && gop_inventory_calls == 0U &&
           result_calls == 1U);
    assert(!last_passed && !last_required);
    assert(last_status == INIT_RESULT_UNAVAILABLE);
    assert(strcmp(last_state, "UNAVAILABLE") == 0);

    reset_nodes();
    descriptors[INIT_VFS].status.name = "vfs";
    descriptors[INIT_VFS].status.state = INIT_READY;
    descriptors[INIT_FILESYSTEMS].status.name = "filesystems";
    descriptors[INIT_FILESYSTEMS].start = callback_ok;
    descriptors[INIT_FILESYSTEMS].required_deps = INIT_BIT(INIT_VFS);
    descriptors[INIT_FILESYSTEMS].required_for_boot = true;
    descriptors[INIT_STORAGE].status.name = "storage";
    descriptors[INIT_STORAGE].start = callback;
    descriptors[INIT_STORAGE].required_deps = INIT_BIT(INIT_FILESYSTEMS);
    descriptors[INIT_STORAGE].required_for_boot = true;
    descriptors[INIT_ROOTFS].status.name = "root filesystem";
    descriptors[INIT_ROOTFS].start = callback_ok;
    descriptors[INIT_ROOTFS].required_deps =
        INIT_BIT(INIT_VFS) | INIT_BIT(INIT_STORAGE);
    descriptors[INIT_ROOTFS].required_for_boot = true;
    callback_result = INIT_RESULT_UNAVAILABLE;
    callback_reason = "no persistent block device";
    assert(!init_start_node(INIT_STORAGE));
    assert(result_calls == 2U);
    assert(last_status == INIT_RESULT_UNAVAILABLE && !last_passed);
    assert(strcmp(last_name, "storage") == 0);
    assert(strcmp(last_reason, "no persistent block device") == 0);
    assert(!init_start_node(INIT_ROOTFS));
    assert(descriptors[INIT_ROOTFS].status.state == INIT_SKIPPED);
    assert(last_status == INIT_RESULT_DEPENDENCY && !last_passed);
    assert(strcmp(last_name, "root filesystem") == 0);
    assert(strcmp(last_state, "SKIPPED") == 0);

    puts("init trace: callback PASS/FAIL, optional status, and required dependency propagation passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="mangrove-init-trace-") as temp:
    c_file = Path(temp) / "init_trace.c"
    binary = Path(temp) / "init_trace"
    c_file.write_text(prefix + functions + suffix)
    subprocess.run([
        CC, "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-Ikernel/include", "-Iinclude",
        str(c_file), "-o", str(binary)
    ], cwd=ROOT, check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=30,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
