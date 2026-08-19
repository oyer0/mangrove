#pragma once

#include <types.h>
#include <address_space.h>

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
bool vmm_map(page_table_t *pml4, void *virtual_addr, phys_addr_t physical_addr, u64 flags);
bool vmm_map_user_page(page_table_t *pml4, void *virtual_addr,
                       phys_addr_t physical_addr, u64 flags);
bool vmm_unmap_user_page(page_table_t *pml4, void *virtual_addr,
                         phys_addr_t *out_physical_addr);
void *vmm_map_mmio(phys_addr_t physical_addr, u64 size);
bool vmm_ioremap_contains(const void *address);
bool vmm_kernel_mapping_present(const void *address);
bool vmm_kernel_mapping_supervisor(const void *address);

void vmm_set_kernel_pml4(phys_addr_t pml4_phys);
page_table_t *vmm_get_kernel_pml4(void);
phys_addr_t vmm_get_kernel_pml4_phys(void);
page_table_t *vmm_get_current_pml4(void);
/* Permanently map RAM-backed physical pages at PHYS_MAP_BASE + phys. */
bool vmm_map_physical_ram(phys_addr_t start, u64 page_count);
void vmm_enable_direct_map(void);
/* Verifies one direct-map leaf and all of its supervisor-only ancestors. */
bool vmm_direct_map_valid(phys_addr_t physical_addr);
page_table_t *vmm_create_address_space(void);
void vmm_destroy_address_space(page_table_t *pml4);
void vmm_switch_address_space(page_table_t *pml4);
bool vmm_user_range_valid(const void *address, usize length);
phys_addr_t vmm_virtual_to_physical(void *virtual_addr);

/* Stage-4 ownership checks.  These are intentionally independent from
 * PTE_USER: that bit is an x86 permission, not a lifetime annotation. */
bool vmm_address_space_validate(const page_table_t *pml4);
bool vmm_kernel_mappings_supervisor_only(void);
