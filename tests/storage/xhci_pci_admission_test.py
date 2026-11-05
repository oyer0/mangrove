#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production xHCI admission with disabled/refused PCI DMA and no device."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
s = (root / 'kernel/src/init.c').read_text()
a = s.index('static init_result_t init_xhci(')
b = s.index('    bool msix_prepared', a)
# Exercise the unmodified production admission prefix through xhci_init.
# Return at its success boundary; interrupt configuration is a separate contract.
production = s[a:b] + '    return INIT_RESULT_OK;\n}\n'
fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <init.h>
typedef uintptr_t phys_addr_t;
typedef struct {unsigned class_code, subclass, prog_if;} pci_device_t;
static pci_device_t device = {0x0c, 3, 0x30};
static bool present = true, writable = true, map_ok = true, irq_ok = true;
static unsigned command, enable_calls, maps, inits, unregisters, routes, shutdowns;
static bool routing_ok = true;
typedef struct {bool supported;} xhci_pci_routing_t;
static void *g_xhc;
#define XHCI_MMIO_BASE_APERTURE_SIZE 32768U
#define IRQ_VECTOR_XHCI 99
#define xhci_trace_capture_ports(...) ((void)0)
static void init_xhci_irq_handler(void) {}
static unsigned pci_get_device_count(void) {return 1;}
static const pci_device_t *pci_get_device(unsigned i) {(void)i;return present?&device:NULL;}
typedef struct {uintptr_t address;} bar_t;
static bar_t pci_get_bar(const pci_device_t *p,unsigned i) {(void)p;(void)i;return (bar_t){4096};}
static bool pci_enable_memory_busmaster(const pci_device_t *p) {
 assert(p==&device);enable_calls++;if(writable)command|=6;return (command&6)==6;
}
static void *vmm_map_mmio(phys_addr_t p,usize n) {
 assert((command&6)==6);assert(p==4096&&n==32768);maps++;return map_ok?(void*)4096:NULL;
}
static bool irq_register_vector(unsigned n, void (*f)(void)) {(void)n;(void)f;return irq_ok;}
static void irq_unregister_vector(unsigned n) {(void)n;unregisters++;}
static void *xhci_init(uintptr_t v,u64 p,u8 irq) {
 assert(v==4096&&p==4096&&irq==11);assert((command&6)==6);inits++;return (void*)1;
}
'''
fixture += r'''
static bool xhci_pci_route_ports(const pci_device_t *p,xhci_pci_routing_t *r) {
 assert(p==&device&&inits);r->supported=true;routes++;return routing_ok;
}
static void xhci_shutdown(void *p) {assert(p==g_xhc);shutdowns++;}
'''
checks = r'''
int main(void) {
 const char *reason=NULL;
 /* Firmware leaves bus master disabled. Driver must enable before MMIO/DMA. */
 assert(init_xhci(&reason)==INIT_RESULT_OK);assert(enable_calls==1&&maps==1&&inits==1);
 routing_ok=false;
 assert(init_xhci(&reason)==INIT_RESULT_FAILED);
 assert(routes==2&&shutdowns==1&&unregisters==1&&g_xhc==NULL);
 routing_ok=true;
 command=0;writable=false;maps=inits=0;
 assert(init_xhci(&reason)==INIT_RESULT_FAILED);assert(reason&&maps==0&&inits==0);
 writable=true;map_ok=false;
 assert(init_xhci(&reason)==INIT_RESULT_FAILED);assert(maps==1&&inits==0);
 map_ok=true;irq_ok=false;maps=0;
 assert(init_xhci(&reason)==INIT_RESULT_FAILED);assert(maps==1&&inits==0);
 present=false;enable_calls=maps=0;
 assert(init_xhci(&reason)==INIT_RESULT_UNAVAILABLE);assert(enable_calls==0&&maps==0);
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-xhci-pci-') as temp:
    source=Path(temp)/'test.c'; binary=Path(temp)/'test'
    source.write_text(fixture+production+checks)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Ikernel/include','-Iinclude',str(source),'-o',str(binary)],cwd=root,check=True)
    subprocess.run([str(binary)],check=True)
print('xHCI PCI admission: disabled DMA, refused enable, mapping/IRQ failures and absent controller passed')
