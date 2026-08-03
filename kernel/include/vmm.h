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

#define VMM_PAGE_SIZE          0x1000ULL
#define VMM_USER_ADDRESS_LIMIT 0x0000800000000000ULL
/* Anonymous mappings are placed above normal static ELF images and below
 * the fixed top-of-user-space stack reserved by the ELF loader. */
#define VMM_USER_ANON_BASE      0x0000400000000000ULL
#define VMM_USER_ANON_LIMIT     0x00007fff00000000ULL

typedef struct page_table {
    u64 entries[512];
} __attribute__((aligned(4096))) page_table_t;

void vmm_init(void);
void vmm_map(page_table_t *pml4, void *virtual_addr, void *physical_addr, u64 flags);
bool vmm_map_user_page(page_table_t *pml4, void *virtual_addr,
                       void *physical_addr, u64 flags);
bool vmm_unmap_user_page(page_table_t *pml4, void *virtual_addr,
                         void **out_physical_addr);
void *vmm_map_mmio(void *physical_addr, u64 size);

void vmm_set_kernel_pml4(page_table_t *pml4);
page_table_t *vmm_get_kernel_pml4(void);
page_table_t *vmm_get_current_pml4(void);
page_table_t *vmm_create_address_space(void);
void vmm_destroy_address_space(page_table_t *pml4);
void vmm_switch_address_space(page_table_t *pml4);
bool vmm_user_range_valid(const void *address, usize length);
u64 vmm_virtual_to_physical(void *virtual_addr);
