/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/* Permanent physical-memory map used by the kernel and its bootstrap CR3. */
#define PHYS_MAP_BASE  0xffff800000000000ULL
#define PHYS_MAP_SIZE  0x0000400000000000ULL
#define PHYS_MAP_LIMIT PHYS_MAP_SIZE

/* Permanent supervisor-only kernel virtual regions.  Heap pages are backed
 * by ordinary PMM RAM; device ranges are deliberately isolated in ioremap. */
#define KERNEL_HEAP_BASE  0xffffc00000000000ULL
#define KERNEL_HEAP_LIMIT 0xffffc08000000000ULL
#define IOREMAP_BASE      0xffffc08000000000ULL
#define IOREMAP_LIMIT     0xffffc10000000000ULL
/* Independently leased kernel virtual space.  This occupies the PML4 branch
 * immediately after the fixed MMIO window; reservation alone creates no PTEs. */
#define KERNEL_VMM_RANGE_BASE  0xffffc10000000000ULL
#define KERNEL_VMM_RANGE_LIMIT 0xffffc18000000000ULL

/* Linked kernel image placement. ELF physical addresses are preferred
 * offsets from KERNEL_PHYS_BASE; the loader may choose another physical base
 * while the image remains linked in the canonical high half. */
#define KERNEL_PHYS_BASE 0x00100000ULL
#define KERNEL_VIRT_BASE 0xffffffff80000000ULL
#define KERNEL_IMAGE_VIRT_BASE (KERNEL_VIRT_BASE + KERNEL_PHYS_BASE)

_Static_assert(KERNEL_HEAP_BASE < KERNEL_HEAP_LIMIT &&
               PHYS_MAP_BASE + PHYS_MAP_SIZE == KERNEL_HEAP_BASE &&
               KERNEL_HEAP_LIMIT == IOREMAP_BASE &&
               IOREMAP_BASE < IOREMAP_LIMIT &&
               IOREMAP_LIMIT == KERNEL_VMM_RANGE_BASE &&
               KERNEL_VMM_RANGE_BASE < KERNEL_VMM_RANGE_LIMIT &&
               KERNEL_VMM_RANGE_LIMIT < KERNEL_VIRT_BASE,
               "kernel virtual regions must remain ordered and disjoint");
_Static_assert(((KERNEL_VMM_RANGE_BASE >> 39) & 0x1ffULL) ==
               (((KERNEL_VMM_RANGE_LIMIT - 1ULL) >> 39) & 0x1ffULL),
               "owned kernel virtual range must fit one PML4 branch");

static inline u64 kernel_image_virt_to_phys(uintptr_t virt,
                                            u64 physical_base)
{
    return physical_base + ((u64)virt - KERNEL_IMAGE_VIRT_BASE);
}

static inline u64 kernel_image_relocate_phys(u64 linked_physical,
                                            u64 physical_base)
{
    return physical_base + (linked_physical - KERNEL_PHYS_BASE);
}
