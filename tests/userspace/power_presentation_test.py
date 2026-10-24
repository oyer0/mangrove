#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check normal shutdown/reboot output and retained failure diagnostics."""
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


def run_c_fixture(directory, name, source):
    cfile = directory / f"{name}.c"
    binary = directory / name
    cfile.write_text(source)
    subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(cfile), "-o",
                    str(binary)], check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=30,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})


shutdown_main = production_function("userspace/shutdown/main.c", "main")
shutdown_main = shutdown_main.replace("int main(", "int shutdown_command_main(", 1)
reboot_main = production_function("userspace/reboot/main.c", "main")
reboot_main = reboot_main.replace("int main(", "int reboot_command_main(", 1)
command_prefix = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef long mg_result_t;
enum { MG_OK = 0, MG_ERR_IO = -5 };
static char output[256];
static size_t output_size;
static unsigned flush_count, poweroff_calls, reboot_calls;
static bool jump_from_transition;
static mg_result_t transition_result;
static jmp_buf transition_return;
static const char *transition_text;
static int probe_printf(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int count = vsnprintf(output + output_size,
                          sizeof(output) - output_size, format, arguments);
    va_end(arguments);
    assert(count >= 0 && (size_t)count < sizeof(output) - output_size);
    output_size += (size_t)count;
    return count;
}
static int probe_fflush(FILE *stream)
{
    assert(stream == (FILE *)1);
    flush_count++;
    return 0;
}
static bool command_help_requested(int argc, char **argv)
{ (void)argc; (void)argv; return false; }
static int command_print_help(const char *name) { (void)name; return 0; }
static void command_usage_error(const char *name, const char *usage,
                                const char *argument)
{ (void)name; (void)usage; (void)argument; }
static bool result_is_error(mg_result_t result) { return result < 0; }
static const char *error_string(mg_result_t result)
{ (void)result; return "I/O failure"; }
static void check_transition(const char *message)
{
    assert(strcmp(output, transition_text) == 0);
    assert(flush_count == 1U);
    assert(strcmp(message, transition_text) == 0);
}
static mg_result_t system_poweroff(void)
{
    poweroff_calls++;
    check_transition("Shutting down...\n");
    if (jump_from_transition) longjmp(transition_return, 1);
    return transition_result;
}
static mg_result_t system_reboot(void)
{
    reboot_calls++;
    check_transition("Restarting...\n");
    if (jump_from_transition) longjmp(transition_return, 1);
    return transition_result;
}
#define printf probe_printf
#define fflush probe_fflush
#undef stdout
#define stdout ((FILE *)1)
'''
command_suffix = r'''
#undef stdout
#undef fflush
#undef printf
static void reset_command_test(const char *message)
{
    memset(output, 0, sizeof(output));
    output_size = 0U;
    flush_count = 0U;
    transition_text = message;
}
int main(void)
{
    char *shutdown_argv[] = {"shutdown", NULL};
    char *reboot_argv[] = {"reboot", NULL};
    char *invalid_argv[] = {"shutdown", "extra", NULL};

    reset_command_test("Shutting down...\n");
    jump_from_transition = true;
    if (setjmp(transition_return) == 0) {
        (void)shutdown_command_main(1, shutdown_argv);
        assert(false);
    }
    assert(strcmp(output, "Shutting down...\n") == 0);
    assert(poweroff_calls == 1U && flush_count == 1U);

    reset_command_test("Restarting...\n");
    if (setjmp(transition_return) == 0) {
        (void)reboot_command_main(1, reboot_argv);
        assert(false);
    }
    assert(strcmp(output, "Restarting...\n") == 0);
    assert(reboot_calls == 1U && flush_count == 1U);

    reset_command_test("Shutting down...\n");
    jump_from_transition = false;
    transition_result = MG_ERR_IO;
    assert(shutdown_command_main(1, shutdown_argv) == 1);
    assert(strcmp(output, "Shutting down...\n"
                          "Could not shut down: I/O failure.\n") == 0);
    assert(poweroff_calls == 2U && flush_count == 1U);

    reset_command_test("Restarting...\n");
    assert(reboot_command_main(1, reboot_argv) == 1);
    assert(strcmp(output, "Restarting...\n"
                          "Could not reboot: I/O failure.\n") == 0);
    assert(reboot_calls == 2U && flush_count == 1U);

    reset_command_test("");
    assert(shutdown_command_main(2, invalid_argv) == 1);
    assert(output_size == 0U && flush_count == 0U && poweroff_calls == 2U);
    puts("shutdown commands: messages precede transition; failures remain visible");
    return 0;
}
'''

power_source = (ROOT / "kernel/src/platform_power.c").read_text()
process_source = (ROOT / "kernel/src/process.c").read_text()
for routine in (
        "applications and services stopped; mounted filesystems synced",
        "[ACPI] mode already enabled", "[ACPI] enabling ACPI mode",
        "[ACPI] shutdown requested", "[ACPI] entering S5",
        "[ACPI] reboot requested"):
    assert routine not in power_source, f"routine power message remains: {routine}"
for diagnostic in (
        "[ERROR] [shutdown] filesystem sync failed",
        "[ERROR] [ACPI] shutdown unavailable",
        "[ERROR] [ACPI] shutdown failed",
        "[ERROR] [ACPI] reboot unavailable",
        "[ERROR] [ACPI] reboot failed"):
    assert diagnostic in power_source, f"power failure diagnostic missing: {diagnostic}"
assert "[WARN] shutdown grace period expired" in process_source
assert "[ERROR] shutdown timeout waiting for safe process exit" in process_source
assert "[shutdown] terminated pid=" not in process_source

power_sync = production_function("kernel/src/platform_power.c",
                                  "power_sync_result")
power_quiesce = production_function("kernel/src/platform_power.c",
                                     "power_quiesce_and_sync")
sync_prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef long i64;
typedef int process_t;
enum { MG_OK = 0, MG_ERR_DEVICE_GONE = -2, MG_ERR_UNCERTAIN = -3,
       MG_ERR_BUSY = -4, MG_ERR_NO_MEMORY = -5, MG_ERR_IO = -6 };
enum { VFS_OK = 0, VFS_ERR_IO = -1, VFS_ERR_DEVICE_GONE = -2,
       VFS_ERR_UNCERTAIN = -3, VFS_ERR_BUSY = -4, VFS_ERR_NO_MEM = -5 };
static char output[256];
static size_t output_size;
static int quiesce_result, sync_result, abort_count;
static bool quiesced, sync_called;
static process_t requester;
static process_t *process_current(void) { return &requester; }
static int process_shutdown_quiesce(process_t *process)
{ assert(process == &requester); quiesced = true; return quiesce_result; }
static void process_shutdown_abort(void) { abort_count++; }
static int vfs_sync_all(void)
{ assert(quiesced); sync_called = true; return sync_result; }
static void kprint(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int count = vsnprintf(output + output_size,
                          sizeof(output) - output_size, format, arguments);
    va_end(arguments);
    assert(count >= 0 && (size_t)count < sizeof(output) - output_size);
    output_size += (size_t)count;
}
'''
sync_suffix = r'''
int main(void)
{
    quiesce_result = MG_OK;
    sync_result = VFS_OK;
    assert(power_quiesce_and_sync() == MG_OK);
    assert(quiesced && sync_called && output_size == 0U && abort_count == 0);

    memset(output, 0, sizeof(output));
    output_size = 0U;
    quiesced = false;
    sync_called = false;
    sync_result = VFS_ERR_IO;
    assert(power_quiesce_and_sync() == MG_ERR_IO);
    assert(sync_called && abort_count == 1);
    assert(strcmp(output,
        "[ERROR] [shutdown] filesystem sync failed: -1; power transition cancelled\n") == 0);

    memset(output, 0, sizeof(output));
    output_size = 0U;
    quiesced = false;
    sync_called = false;
    quiesce_result = MG_ERR_BUSY;
    assert(power_quiesce_and_sync() == MG_ERR_BUSY);
    assert(quiesced && !sync_called && output_size == 0U);
    puts("power lifecycle: normal quiesce/sync is silent; sync errors remain visible");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="mangrove-power-presentation-") as tmp:
    directory = Path(tmp)
    run_c_fixture(directory, "power-commands",
                  command_prefix + shutdown_main + reboot_main + command_suffix)
    run_c_fixture(directory, "power-sync",
                  sync_prefix + power_sync + power_quiesce + sync_suffix)
