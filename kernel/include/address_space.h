#pragma once

#include <types.h>
#include <address_layout.h>

/* Physical addresses are values consumed by page tables, CR3, and devices.
 * They are never C pointers.  Kernel virtual addresses are represented
 * separately so callers must make an intentional conversion at the boundary. */
typedef u64 phys_addr_t;
typedef uintptr_t virt_addr_t;

/* Canonical, supervisor-only linear mapping of ordinary system RAM.
 * MMIO is deliberately excluded: device ranges continue to require an
 * explicit ioremap/MMIO mapping. */

_Static_assert((PHYS_MAP_BASE >> 48) == 0xffffULL,
               "PHYS_MAP_BASE must be canonical in the upper half");
_Static_assert(((PHYS_MAP_BASE + PHYS_MAP_SIZE - 1) >> 48) == 0xffffULL,
               "PHYS_MAP_SIZE must remain within the canonical upper half");

/* Returns the permanent direct-map virtual address for mapped RAM.  During
 * the tightly bounded paging bootstrap it returns the temporary low identity
 * address instead; after phys_map_activate() no converted subsystem relies on
 * that bootstrap mapping.  Returns NULL for an address outside the map. */
void *phys_to_virt(phys_addr_t phys);

/* Converts a direct-map virtual address back to a physical address.  The
 * bootstrap identity conversion is available only before phys_map_activate().
 * Returns zero for addresses outside its documented domain. */
phys_addr_t virt_to_phys(const void *virt);

bool phys_map_contains(phys_addr_t phys);
bool phys_map_is_ready(void);
void phys_map_activate(void);
