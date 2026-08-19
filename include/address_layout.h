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

/* Stage-2 kernel image placement.  The image is loaded at the physical
 * address while its linked/executing address is in the canonical high half. */
#define KERNEL_PHYS_BASE 0x00200000ULL
#define KERNEL_VIRT_BASE 0xffffffff80000000ULL

static inline u64 kernel_image_virt_to_phys(uintptr_t virt)
{
    return (u64)virt - KERNEL_VIRT_BASE;
}
