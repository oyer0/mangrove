#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise AHCI startup and isolation using production driver functions."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"
SOURCE = (ROOT / "kernel/src/storage/ahci.c").read_text()


def function(name):
    pattern = r"^(?:static )?[A-Za-z_][^\n;{}]*\b" + re.escape(name) + r"\s*\("
    for match in re.finditer(pattern, SOURCE, re.M):
        brace = SOURCE.index("{", match.end())
        if ";" in SOURCE[match.end():brace]:
            continue
        end, depth = brace + 1, 1
        while depth:
            depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
            end += 1
        return SOURCE[match.start():end] + "\n"
    raise AssertionError(f"missing production function: {name}")


prefix = r'''
#include <ahci.h>
#include <ahci_structs.h>
#include <block.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define AHCI_OPERATION_TIMEOUT_US 1000000ULL

typedef struct { u64 start_ticks, duration_ticks; bool expired; }
    timer_monotonic_deadline_t;

static volatile ahci_hba_registers_t fake_hba;
static volatile ahci_hba_registers_t *hba = &fake_hba;
static ahci_device_t devices[AHCI_MAX_PORTS];
static u8 dma_memory[4][8192] __attribute__((aligned(4096)));
static unsigned dma_allocations, dma_frees, execute_count, register_count;
static unsigned kprint_count;
static u8 last_command;
static bool complete_command;
static u32 completed_bytes;
static volatile ahci_port_registers_t *active_port;
static ahci_device_t *active_device;
static char diagnostic[160];

static bool timer_monotonic_deadline_start(timer_monotonic_deadline_t *d,
                                           u64 duration)
{ assert(d); d->start_ticks=0; d->duration_ticks=duration; d->expired=false; return true; }
static bool timer_monotonic_deadline_expired(timer_monotonic_deadline_t *d)
{
    (void)d;
    if (complete_command && active_port && (active_port->ci & 1U)) {
        ahci_command_header_t *header = active_device->command_list.virt;
        header->prdbc = completed_bytes;
        active_port->ci = 0;
    }
    return false;
}
dma_buffer_t dma_alloc(usize size, usize alignment)
{
    (void)size; assert(alignment == 4096U && dma_allocations < 4U);
    unsigned index = dma_allocations++;
    return (dma_buffer_t){dma_memory[index], 0x100000U + (u64)index * 4096U};
}
void dma_free(dma_buffer_t buffer)
{ if (buffer.virt) dma_frees++; }
bool block_register(block_device_t *device)
{ assert(device && device->type == BLOCK_DEVICE_SATA); register_count++; return true; }
static bool ata_supports_lba48(const ata_identify_data_t *id)
{ (void)id; return false; }
static u64 ata_lba48_sector_count(const ata_identify_data_t *id)
{ (void)id; return 4096U; }
static u64 ata_lba28_sector_count(const ata_identify_data_t *id)
{ (void)id; return 4096U; }
static void ata_copy_model(const ata_identify_data_t *id, char *out, usize cap)
{ (void)id; if (cap) strncpy(out, "fixture ATA disk", cap - 1U); }
static bool ahci_read(block_device_t *d, u64 lba, u32 count, void *buffer)
{ (void)d; (void)lba; (void)count; (void)buffer; return true; }
static bool ahci_write(block_device_t *d, u64 lba, u32 count, const void *buffer)
{ (void)d; (void)lba; (void)count; (void)buffer; return true; }
static bool ahci_flush(block_device_t *d)
{ (void)d; return true; }
static void kprint(const char *format, ...)
{
    va_list args; va_start(args, format);
    (void)vsnprintf(diagnostic, sizeof(diagnostic), format, args);
    va_end(args); kprint_count++;
}
static void reset_fixture(u32 signature, u32 task_file_status)
{
    memset((void *)&fake_hba, 0, sizeof(fake_hba));
    memset(devices, 0, sizeof(devices));
    memset(dma_memory, 0, sizeof(dma_memory));
    memset(diagnostic, 0, sizeof(diagnostic));
    dma_allocations = dma_frees = execute_count = register_count = 0;
    kprint_count = 0; last_command = 0; complete_command = true;
    completed_bytes = 512U;
    fake_hba.cap = 1U << 31;
    fake_hba.ports[2].sig = signature;
    fake_hba.ports[2].ssts = AHCI_SSTS_DET_PRESENT |
        (AHCI_SSTS_IPM_ACTIVE << 8);
    fake_hba.ports[2].tfd = task_file_status;
    active_port = &fake_hba.ports[2];
    active_device = &devices[2];
}
'''

execute = function("ahci_execute")
execute = execute.replace(
    "{\n    volatile ahci_port_registers_t *port = device->port;",
    "{\n    execute_count++;\n    last_command = command;\n"
    "    volatile ahci_port_registers_t *port = device->port;",
    1,
)
assert "execute_count++;" in execute
functions = "\n".join(function(name) for name in (
    "ahci_wait_clear", "ahci_stop_port", "ahci_start_port",
    "ahci_get_port", "ahci_isolate", "ahci_ready"))
functions += "\n" + execute
functions += "\n" + function("ahci_port_present")
functions += "\n" + function("ahci_port_init")
# The host fixture models AHCI's write-one-to-clear status registers by
# translating those writes to zero; the rest of the production command path is
# compiled and executed without modification.
functions = functions.replace("port->is = (u32)-1;", "port->is = 0U;")
functions = functions.replace("port->serr = (u32)-1;", "port->serr = 0U;")

test = r'''
int main(void)
{
    const u32 unsupported[] = {
        AHCI_SIG_ATAPI, AHCI_SIG_SEMB, AHCI_SIG_PM, 0xffffffffU
    };
    for (usize i = 0; i < sizeof(unsupported)/sizeof(unsupported[0]); ++i) {
        reset_fixture(unsupported[i], 0);
        ahci_port_init(2);
        assert(dma_allocations == 0U && execute_count == 0U);
        assert(register_count == 0U && dma_frees == 0U);
        assert(devices[2].port == NULL && fake_hba.ports[2].cmd == 0U);
        assert(kprint_count == 0U);
    }

    reset_fixture(AHCI_SIG_ATA, 0);
    ahci_port_init(2);
    assert(dma_allocations == 4U && execute_count == 1U);
    assert(last_command == 0xecU && register_count == 1U);
    assert(devices[2].port == &fake_hba.ports[2]);
    assert(!devices[2].isolated && !devices[2].quiesced);
    assert(fake_hba.ports[2].cmd & AHCI_PORT_CMD_ST);
    assert(dma_frees == 0U && kprint_count == 0U);

    reset_fixture(AHCI_SIG_ATA, AHCI_PORT_TFD_ERR);
    ahci_port_init(2);
    assert(dma_allocations == 4U && execute_count == 1U);
    assert(last_command == 0xecU && register_count == 0U);
    assert(devices[2].isolated && devices[2].quiesced);
    assert((fake_hba.ports[2].cmd &
            (AHCI_PORT_CMD_ST | AHCI_PORT_CMD_FRE)) == 0U);
    assert(dma_frees == 0U);
    assert(kprint_count == 1U);
    assert(strcmp(diagnostic,
        "[AHCI] port isolated; DMA retained; engines stopped\n") == 0);

    reset_fixture(AHCI_SIG_ATA, 0);
    fake_hba.ports[2].ssts = 0U;
    ahci_port_init(2);
    assert(dma_allocations == 0U && execute_count == 0U);
    assert(register_count == 0U && kprint_count == 0U);

    puts("AHCI signatures: unsupported devices skip DMA; ATA probe and failure isolation passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="mangrove-ahci-signature-") as tmp:
    source = Path(tmp) / "ahci_signature_test.c"
    binary = Path(tmp) / "ahci_signature_test"
    source.write_text(prefix + functions + test)
    subprocess.run([
        CC, "-O1", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-Ikernel/include",
        "-Ikernel/include/storage", "-Iinclude", str(source), "-o",
        str(binary),
    ], cwd=ROOT, check=True, timeout=60)
    subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=30,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
