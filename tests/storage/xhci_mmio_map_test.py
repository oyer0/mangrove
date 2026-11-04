#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the production xHCI extended-capability mapping helper."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def production_function(source, name):
    match = re.search(
        r'^static volatile u32 \*' + re.escape(name) + r'\s*\(',
        source, re.M)
    if not match:
        raise ValueError(f"missing production function {name}")
    start = source.index('{', match.end())
    end = start + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


production = production_function(
    (ROOT / 'kernel/src/usb/xhci.c').read_text(),
    'xhci_extended_capability')
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <types.h>
#include <xhci_regs.h>

typedef u64 phys_addr_t;
typedef struct xhci_controller {
    uintptr_t mmio_base;
    u64 mmio_phys_base;
} xhci_controller_t;

static volatile u32 mapped_words[2];
static u64 mapped_physical;
static u64 mapped_size;
static u32 mapping_calls;

void *vmm_map_mmio(phys_addr_t physical, u64 size)
{
    mapped_physical = physical;
    mapped_size = size;
    mapping_calls++;
    return (void *)mapped_words;
}

'''
checks = r'''
int main(void)
{
    u32 base_registers[XHCI_MMIO_BASE_APERTURE_SIZE / sizeof(u32)] = {0};
    xhci_controller_t controller = {
        .mmio_base = (uintptr_t)base_registers,
        .mmio_phys_base = 0xFED00000ULL,
    };
    volatile u32 *capability;

    capability = xhci_extended_capability(
        &controller, 0x3FFCU, XHCI_EXT_CAP_HEADER_SIZE);
    assert(capability == (volatile u32 *)(controller.mmio_base + 0x3FFCU));
    assert(mapping_calls == 0U);

    capability = xhci_extended_capability(
        &controller, XHCI_MMIO_BASE_APERTURE_SIZE,
        XHCI_EXT_CAP_HEADER_SIZE);
    assert(capability == mapped_words);
    assert(mapping_calls == 1U);
    assert(mapped_physical == controller.mmio_phys_base +
                              XHCI_MMIO_BASE_APERTURE_SIZE);
    assert(mapped_size == XHCI_EXT_CAP_HEADER_SIZE);

    capability = xhci_extended_capability(
        &controller, 0x3FFCU, 2U * sizeof(u32));
    assert(capability == mapped_words);
    assert(mapping_calls == 2U);
    assert(mapped_physical == controller.mmio_phys_base + 0x3FFCU);
    assert(mapped_size == 2U * sizeof(u32));

    assert(xhci_extended_capability(
               &controller, XHCI_MMIO_APERTURE_SIZE,
               XHCI_EXT_CAP_HEADER_SIZE) == NULL);
    assert(mapping_calls == 2U);

    controller.mmio_phys_base = UINT64_MAX - 0x3FFFU;
    assert(xhci_extended_capability(
               &controller, XHCI_MMIO_BASE_APERTURE_SIZE,
               XHCI_EXT_CAP_HEADER_SIZE) == NULL);
    assert(mapping_calls == 2U);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='mangrove-xhci-mmio-map-') as tmp:
    source = Path(tmp) / 'xhci_mmio_map_test.c'
    binary = Path(tmp) / 'xhci_mmio_map_test'
    source.write_text(fixture + production + checks)
    subprocess.run([
        sys.argv[1] if len(sys.argv) > 1 else 'cc',
        '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I' + str(ROOT / 'include'),
        '-I' + str(ROOT / 'kernel/include'),
        '-I' + str(ROOT / 'kernel/include/usb'),
        str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)

print('PASS: production xHCI extended-capability reads reuse the BAR mapping '
      'or map only the advertised physical register range')
