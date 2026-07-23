#pragma once


#include <types.h>

#define PTE_PRESENT       (1ULL << 0)
#define PTE_READWRITE     (1ULL << 1)
#define PTE_USER          (1ULL << 2)
#define PTE_WRITETHROUGH  (1ULL << 3)
#define PTE_CACHEDISABLE  (1ULL << 4)
#define PTE_ACCESSED      (1ULL << 5)
#define PTE_DIRTY         (1ULL << 6)
#define PTE_HUGE          (1ULL << 7)
#define PTE_GLOBAL        (1ULL << 8)
#define PTE_NX            (1ULL << 63)

#define PTE_FRAME_MASK    0x000FFFFFFFFFF000ULL

typedef struct {
    u64 entries[512];
} __attribute__((aligned(4096))) page_table_t;

void vmm_init(void);
void vmm_map(page_table_t *pml4, void *virtual_addr, void *physical_addr, u64 flags);

void vmm_set_kernel_pml4(page_table_t *pml4);
page_table_t *vmm_get_kernel_pml4(void);

