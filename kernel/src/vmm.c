#include <vmm.h>
#include <pmm.h>

static page_table_t *kernel_pml4;

void vmm_init(void) {
    
}

void vmm_map(page_table_t *pml4, void *virtual_addr, void *physical_addr, u64 flags) {
    u64 vaddr = (u64)virtual_addr;
    u64 paddr = (u64)physical_addr;

    u64 pml4_idx = (vaddr >> 39) & 0x1FF;
    u64 pdpt_idx = (vaddr >> 30) & 0x1FF;
    u64 pd_idx   = (vaddr >> 21) & 0x1FF;
    u64 pt_idx   = (vaddr >> 12) & 0x1FF;

    page_table_t *pdpt = 0;
    if (!(pml4->entries[pml4_idx] & PTE_PRESENT)) {
        pdpt = (page_table_t *)pmm_alloc_frame();
        for (int i = 0; i < 512; i++) {
            pdpt->entries[i] = 0;
        }
        pml4->entries[pml4_idx] = (u64)pdpt | PTE_PRESENT | PTE_READWRITE;
    } else {
        pdpt = (page_table_t *)(pml4->entries[pml4_idx] & PTE_FRAME_MASK);
    }

    page_table_t *pd = 0;
    if (!(pdpt->entries[pdpt_idx] & PTE_PRESENT)) {
        pd = (page_table_t *)pmm_alloc_frame();
        for (int i = 0; i < 512; i++) {
            pd->entries[i] = 0;
        }
        pdpt->entries[pdpt_idx] = (u64)pd | PTE_PRESENT | PTE_READWRITE;
    } else {
        pd = (page_table_t *)(pdpt->entries[pdpt_idx] & PTE_FRAME_MASK);
    }

    page_table_t *pt = 0;
    if (!(pd->entries[pd_idx] & PTE_PRESENT)) {
        pt = (page_table_t *)pmm_alloc_frame();
        for (int i = 0; i < 512; i++) {
            pt->entries[i] = 0;
        }
        pd->entries[pd_idx] = (u64)pt | PTE_PRESENT | PTE_READWRITE;
    } else {
        pt = (page_table_t *)(pd->entries[pd_idx] & PTE_FRAME_MASK);
    }

    pt->entries[pt_idx] = (paddr & PTE_FRAME_MASK) | flags | PTE_PRESENT;
}

void vmm_set_kernel_pml4(page_table_t *pml4) {
    kernel_pml4 = pml4;
}

page_table_t *vmm_get_kernel_pml4(void) {
    return kernel_pml4;
}
