#include <vmm.h>
#include <pmm.h>
#include <panic.h>

static page_table_t *kernel_pml4;
static page_table_t *current_pml4;

#define VMM_MAX_ADDRESS_SPACES 64U
static page_table_t *process_address_spaces[VMM_MAX_ADDRESS_SPACES];

static bool vmm_register_address_space(page_table_t *pml4)
{
    u32 free_slot = VMM_MAX_ADDRESS_SPACES;

    if (!pml4 || pml4 == kernel_pml4) return false;
    for (u32 i = 0; i < VMM_MAX_ADDRESS_SPACES; i++) {
        if (process_address_spaces[i] == pml4) return true;
        if (!process_address_spaces[i] && free_slot == VMM_MAX_ADDRESS_SPACES) {
            free_slot = i;
        }
    }
    if (free_slot == VMM_MAX_ADDRESS_SPACES) return false;
    process_address_spaces[free_slot] = pml4;
    return true;
}

static void vmm_unregister_address_space(page_table_t *pml4)
{
    for (u32 i = 0; i < VMM_MAX_ADDRESS_SPACES; i++) {
        if (process_address_spaces[i] == pml4) {
            process_address_spaces[i] = 0;
            return;
        }
    }
}

static page_table_t *vmm_clone_page_table(const page_table_t *source)
{
    page_table_t *copy;
    u32 i;

    if (!source) return 0;
    copy = (page_table_t *)pmm_alloc_frame();
    if (!copy) return 0;
    for (i = 0; i < 512; i++) {
        copy->entries[i] = source->entries[i];
    }
    return copy;
}

/* Every user-marked intermediate entry in a process address space is private.
 * Entries without PTE_USER still point at shared supervisor-only kernel
 * tables and must never be reclaimed with the process. */
static void vmm_destroy_user_branch(page_table_t *table, u32 level)
{
    u32 i;
    if (!table) return;
    if (level == 1) {
        for (i = 0; i < 512; i++) {
            u64 entry = table->entries[i];
            if ((entry & (PTE_PRESENT | PTE_USER)) ==
                (PTE_PRESENT | PTE_USER)) {
                pmm_free_frame((void *)(entry & PTE_FRAME_MASK));
            }
        }
    } else {
        for (i = 0; i < 512; i++) {
            u64 entry = table->entries[i];
            if ((entry & (PTE_PRESENT | PTE_USER)) ==
                    (PTE_PRESENT | PTE_USER) && !(entry & PTE_HUGE)) {
                vmm_destroy_user_branch(
                    (page_table_t *)(entry & PTE_FRAME_MASK), level - 1);
            }
        }
    }
    pmm_free_frame(table);
}

void vmm_init(void) {
    current_pml4 = 0;
    kernel_pml4 = 0;
    for (u32 i = 0; i < VMM_MAX_ADDRESS_SPACES; i++) {
        process_address_spaces[i] = 0;
    }
}

static bool vmm_map_one(page_table_t *pml4, void *virtual_addr,
                        void *physical_addr, u64 flags)
{
    u64 vaddr = (u64)virtual_addr;
    u64 paddr = (u64)physical_addr;

    if (!pml4) return false;

    u64 pml4_idx = (vaddr >> 39) & 0x1FF;
    u64 pdpt_idx = (vaddr >> 30) & 0x1FF;
    u64 pd_idx   = (vaddr >> 21) & 0x1FF;
    u64 pt_idx   = (vaddr >> 12) & 0x1FF;

    bool private_user_mapping = pml4 != kernel_pml4 && (flags & PTE_USER);
    page_table_t *pdpt = 0;
    if (!(pml4->entries[pml4_idx] & PTE_PRESENT)) {
        pdpt = (page_table_t *)pmm_alloc_frame();
        if (!pdpt) return false;
        for (int i = 0; i < 512; i++) {
            pdpt->entries[i] = 0;
        }
        pml4->entries[pml4_idx] = (u64)pdpt | PTE_PRESENT | PTE_READWRITE |
            (flags & PTE_USER);
    } else {
        pdpt = (page_table_t *)(pml4->entries[pml4_idx] & PTE_FRAME_MASK);
        if (private_user_mapping &&
            !(pml4->entries[pml4_idx] & PTE_USER)) {
            page_table_t *private_pdpt = vmm_clone_page_table(pdpt);
            if (!private_pdpt) return false;
            pml4->entries[pml4_idx] = (u64)private_pdpt |
                (pml4->entries[pml4_idx] & ~PTE_FRAME_MASK) | PTE_USER;
            pdpt = private_pdpt;
        }
        if (private_user_mapping) pml4->entries[pml4_idx] |= PTE_USER;
    }

    page_table_t *pd = 0;
    if (!(pdpt->entries[pdpt_idx] & PTE_PRESENT)) {
        pd = (page_table_t *)pmm_alloc_frame();
        if (!pd) return false;
        for (int i = 0; i < 512; i++) {
            pd->entries[i] = 0;
        }
        pdpt->entries[pdpt_idx] = (u64)pd | PTE_PRESENT | PTE_READWRITE |
            (flags & PTE_USER);
    } else {
        pd = (page_table_t *)(pdpt->entries[pdpt_idx] & PTE_FRAME_MASK);
        if (private_user_mapping &&
            !(pdpt->entries[pdpt_idx] & PTE_USER)) {
            page_table_t *private_pd = vmm_clone_page_table(pd);
            if (!private_pd) return false;
            pdpt->entries[pdpt_idx] = (u64)private_pd |
                (pdpt->entries[pdpt_idx] & ~PTE_FRAME_MASK) | PTE_USER;
            pd = private_pd;
        }
        if (private_user_mapping) pdpt->entries[pdpt_idx] |= PTE_USER;
    }

    page_table_t *pt = 0;
    if (!(pd->entries[pd_idx] & PTE_PRESENT)) {
        pt = (page_table_t *)pmm_alloc_frame();
        if (!pt) return false;
        for (int i = 0; i < 512; i++) {
            pt->entries[i] = 0;
        }
        pd->entries[pd_idx] = (u64)pt | PTE_PRESENT | PTE_READWRITE |
            (flags & PTE_USER);
    } else {
        pt = (page_table_t *)(pd->entries[pd_idx] & PTE_FRAME_MASK);
        if (private_user_mapping && !(pd->entries[pd_idx] & PTE_USER)) {
            page_table_t *private_pt = vmm_clone_page_table(pt);
            if (!private_pt) return false;
            pd->entries[pd_idx] = (u64)private_pt |
                (pd->entries[pd_idx] & ~PTE_FRAME_MASK) | PTE_USER;
            pt = private_pt;
        }
        if (private_user_mapping) pd->entries[pd_idx] |= PTE_USER;
    }

    pt->entries[pt_idx] = (paddr & PTE_FRAME_MASK) | flags | PTE_PRESENT;
    if (pml4 == current_pml4) {
        __asm__ volatile("invlpg (%0)" :: "r"(virtual_addr) : "memory");
    }
    return true;
}

static bool vmm_page_present(page_table_t *pml4, u64 virtual_addr)
{
    page_table_t *pdpt;
    page_table_t *pd;
    page_table_t *pt;
    u64 entry;

    if (!pml4) return false;
    entry = pml4->entries[(virtual_addr >> 39) & 0x1ff];
    if (!(entry & PTE_PRESENT)) return false;
    if (entry & PTE_HUGE) return true;
    pdpt = (page_table_t *)(entry & PTE_FRAME_MASK);
    entry = pdpt->entries[(virtual_addr >> 30) & 0x1ff];
    if (!(entry & PTE_PRESENT)) return false;
    if (entry & PTE_HUGE) return true;
    pd = (page_table_t *)(entry & PTE_FRAME_MASK);
    entry = pd->entries[(virtual_addr >> 21) & 0x1ff];
    if (!(entry & PTE_PRESENT)) return false;
    if (entry & PTE_HUGE) return true;
    pt = (page_table_t *)(entry & PTE_FRAME_MASK);
    return (pt->entries[(virtual_addr >> 12) & 0x1ff] & PTE_PRESENT) != 0;
}

bool vmm_map_user_page(page_table_t *pml4, void *virtual_addr,
                       void *physical_addr, u64 flags)
{
    u64 virtual_value = (u64)virtual_addr;
    u64 physical_value = (u64)physical_addr;

    if (!pml4 || pml4 == kernel_pml4 ||
        (virtual_value & (VMM_PAGE_SIZE - 1)) ||
        (physical_value & (VMM_PAGE_SIZE - 1)) ||
        !(flags & PTE_USER) || vmm_page_present(pml4, virtual_value)) {
        return false;
    }
    return vmm_map_one(pml4, virtual_addr, physical_addr, flags | PTE_USER);
}

bool vmm_unmap_user_page(page_table_t *pml4, void *virtual_addr,
                         void **out_physical_addr)
{
    u64 virtual_value = (u64)virtual_addr;
    page_table_t *pdpt;
    page_table_t *pd;
    page_table_t *pt;
    u64 entry;
    u64 *pte;

    if (!pml4 || pml4 == kernel_pml4 || !out_physical_addr ||
        (virtual_value & (VMM_PAGE_SIZE - 1))) {
        return false;
    }
    entry = pml4->entries[(virtual_value >> 39) & 0x1ff];
    if ((entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER) ||
        (entry & PTE_HUGE)) return false;
    pdpt = (page_table_t *)(entry & PTE_FRAME_MASK);
    entry = pdpt->entries[(virtual_value >> 30) & 0x1ff];
    if ((entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER) ||
        (entry & PTE_HUGE)) return false;
    pd = (page_table_t *)(entry & PTE_FRAME_MASK);
    entry = pd->entries[(virtual_value >> 21) & 0x1ff];
    if ((entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER) ||
        (entry & PTE_HUGE)) return false;
    pt = (page_table_t *)(entry & PTE_FRAME_MASK);
    pte = &pt->entries[(virtual_value >> 12) & 0x1ff];
    if ((*pte & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER)) {
        return false;
    }
    *out_physical_addr = (void *)(*pte & PTE_FRAME_MASK);
    *pte = 0;
    if (pml4 == current_pml4) {
        __asm__ volatile("invlpg (%0)" :: "r"(virtual_addr) : "memory");
    }
    return true;
}

/* Publish an already-created kernel leaf into a process page-table tree.
 * User mappings make only the ancestors they touch private.  At the first
 * missing process entry, share the corresponding supervisor-only kernel
 * subtree; if every ancestor is private, copy only the final kernel PTE.
 * This requires no allocation and preserves private user leaves. */
static bool vmm_publish_kernel_mapping(page_table_t *target,
                                       void *virtual_addr)
{
    u64 vaddr = (u64)virtual_addr;
    u64 pml4_idx = (vaddr >> 39) & 0x1ff;
    u64 pdpt_idx = (vaddr >> 30) & 0x1ff;
    u64 pd_idx = (vaddr >> 21) & 0x1ff;
    u64 pt_idx = (vaddr >> 12) & 0x1ff;
    u64 kernel_pml4e;
    u64 kernel_pdpte;
    u64 kernel_pde;
    page_table_t *kernel_pdpt;
    page_table_t *kernel_pd;
    page_table_t *kernel_pt;
    page_table_t *target_pdpt;
    page_table_t *target_pd;
    page_table_t *target_pt;

    if (!target || !kernel_pml4) return false;
    kernel_pml4e = kernel_pml4->entries[pml4_idx];
    if (!(kernel_pml4e & PTE_PRESENT) || (kernel_pml4e & PTE_USER)) {
        return false;
    }
    if (!(target->entries[pml4_idx] & PTE_PRESENT)) {
        target->entries[pml4_idx] = kernel_pml4e;
        goto published;
    }

    kernel_pdpt = (page_table_t *)(kernel_pml4e & PTE_FRAME_MASK);
    target_pdpt = (page_table_t *)(target->entries[pml4_idx] & PTE_FRAME_MASK);
    if (kernel_pdpt == target_pdpt) goto published;

    kernel_pdpte = kernel_pdpt->entries[pdpt_idx];
    if (!(kernel_pdpte & PTE_PRESENT) || (kernel_pdpte & PTE_USER) ||
        (kernel_pdpte & PTE_HUGE)) {
        return false;
    }
    if (!(target_pdpt->entries[pdpt_idx] & PTE_PRESENT)) {
        target_pdpt->entries[pdpt_idx] = kernel_pdpte;
        goto published;
    }
    if (target_pdpt->entries[pdpt_idx] & PTE_HUGE) return false;

    kernel_pd = (page_table_t *)(kernel_pdpte & PTE_FRAME_MASK);
    target_pd = (page_table_t *)(target_pdpt->entries[pdpt_idx] &
                                 PTE_FRAME_MASK);
    if (kernel_pd == target_pd) goto published;

    kernel_pde = kernel_pd->entries[pd_idx];
    if (!(kernel_pde & PTE_PRESENT) || (kernel_pde & PTE_USER) ||
        (kernel_pde & PTE_HUGE)) {
        return false;
    }
    if (!(target_pd->entries[pd_idx] & PTE_PRESENT)) {
        target_pd->entries[pd_idx] = kernel_pde;
        goto published;
    }
    if (target_pd->entries[pd_idx] & PTE_HUGE) return false;

    kernel_pt = (page_table_t *)(kernel_pde & PTE_FRAME_MASK);
    target_pt = (page_table_t *)(target_pd->entries[pd_idx] & PTE_FRAME_MASK);
    if (kernel_pt != target_pt) {
        u64 kernel_pte = kernel_pt->entries[pt_idx];
        if (!(kernel_pte & PTE_PRESENT) || (kernel_pte & PTE_USER)) {
            return false;
        }
        target_pt->entries[pt_idx] = kernel_pte;
    }

published:
    if (target == current_pml4) {
        __asm__ volatile("invlpg (%0)" :: "r"(virtual_addr) : "memory");
    }
    return true;
}

void vmm_map(page_table_t *pml4, void *virtual_addr, void *physical_addr,
             u64 flags)
{
    if (!vmm_map_one(pml4, virtual_addr, physical_addr, flags)) return;

    /* Process roots contain private user-bearing ancestors, so changing only
     * the kernel tree is insufficient after an ancestor splits. */
    if (pml4 == kernel_pml4 && !(flags & PTE_USER)) {
        for (u32 i = 0; i < VMM_MAX_ADDRESS_SPACES; i++) {
            page_table_t *target = process_address_spaces[i];
            if (target &&
                !vmm_publish_kernel_mapping(target, virtual_addr)) {
                panic("VMM failed to publish a kernel mapping");
            }
        }
    }
}

void *vmm_map_mmio(void *physical_addr, u64 size) {
    u64 phys = (u64)physical_addr;
    u64 start = phys & ~0xFFFULL;
    u64 end = (phys + size + 0xFFF) & ~0xFFFULL;

    for (u64 addr = start; addr < end; addr += 0x1000) {
        vmm_map(kernel_pml4, (void *)addr, (void *)addr,
                PTE_READWRITE);
    }

    return physical_addr;
}

u64 vmm_virtual_to_physical(void *virtual_addr)
{
    if (!current_pml4)
    {
        return 0;
    }

    u64 vaddr = (u64)virtual_addr;

    u64 pml4_idx = (vaddr >> 39) & 0x1FF;
    u64 pdpt_idx = (vaddr >> 30) & 0x1FF;
    u64 pd_idx   = (vaddr >> 21) & 0x1FF;
    u64 pt_idx   = (vaddr >> 12) & 0x1FF;

    u64 offset = vaddr & 0xFFF;

    if (!(current_pml4->entries[pml4_idx] & PTE_PRESENT)) {
        return 0;
    }
    
    page_table_t *pdpt =
        (page_table_t *)(current_pml4->entries[pml4_idx] & PTE_FRAME_MASK);

    if (!(pdpt->entries[pdpt_idx] & PTE_PRESENT)) {
        return 0;
    }
    
    page_table_t *pd =
        (page_table_t *)(pdpt->entries[pdpt_idx] & PTE_FRAME_MASK);

    if (!(pd->entries[pd_idx] & PTE_PRESENT))
    {
        return 0;
    }
    
    page_table_t *pt =
        (page_table_t *)(pd->entries[pd_idx] & PTE_FRAME_MASK);
    
    if (!(pt->entries[pt_idx] & PTE_PRESENT))
    {
        return 0;
    }
    
    u64 frame = pt->entries[pt_idx] & PTE_FRAME_MASK;
    
    return frame + offset;

}

void vmm_set_kernel_pml4(page_table_t *pml4) {
    kernel_pml4 = pml4;
    current_pml4 = pml4;
}

page_table_t *vmm_get_kernel_pml4(void) {
    return kernel_pml4;
}

page_table_t *vmm_get_current_pml4(void)
{
    return current_pml4;
}

page_table_t *vmm_create_address_space(void)
{
    page_table_t *pml4;
    u32 i;

    if (!kernel_pml4) return 0;
    /* Start with shared supervisor-only kernel branches.  vmm_map() clones
     * only the branch it must modify for a user mapping; untouched kernel
     * branches remain shared and are never reclaimed with the process. */
    pml4 = (page_table_t *)pmm_alloc_frame();
    if (!pml4) return 0;
    for (i = 0; i < 512; i++) {
        pml4->entries[i] = kernel_pml4->entries[i];
    }
    if (!vmm_register_address_space(pml4)) {
        pmm_free_frame(pml4);
        return 0;
    }
    return pml4;
}

void vmm_destroy_address_space(page_table_t *pml4)
{
    u32 i;
    if (!pml4 || pml4 == kernel_pml4) return;
    if (current_pml4 == pml4) {
        vmm_switch_address_space(kernel_pml4);
    }
    vmm_unregister_address_space(pml4);
    for (i = 0; i < 512; i++) {
        u64 entry = pml4->entries[i];
        if ((entry & (PTE_PRESENT | PTE_USER)) !=
            (PTE_PRESENT | PTE_USER)) continue;
        vmm_destroy_user_branch((page_table_t *)(entry & PTE_FRAME_MASK), 3);
    }
    pmm_free_frame(pml4);
}

void vmm_switch_address_space(page_table_t *pml4)
{
    if (!pml4 || pml4 == current_pml4) return;
    current_pml4 = pml4;
    __asm__ volatile("mov %0, %%cr3" :: "r"(pml4) : "memory");
}

bool vmm_user_range_valid(const void *address, usize length)
{
    uintptr_t start = (uintptr_t)address;
    uintptr_t end;
    uintptr_t page;

    if (!current_pml4 || (!address && length) ||
        length > ~(uintptr_t)0 - start) return false;
    end = start + length;
    if (length == 0) return true;

    for (page = start & ~(uintptr_t)0xfff; page < end; ) {
        u64 pml4e = current_pml4->entries[(page >> 39) & 0x1ff];
        page_table_t *pdpt;
        page_table_t *pd;
        page_table_t *pt;
        u64 pdpte;
        u64 pde;
        u64 pte;

        if (!(pml4e & PTE_PRESENT) || !(pml4e & PTE_USER)) return false;
        pdpt = (page_table_t *)(pml4e & PTE_FRAME_MASK);
        pdpte = pdpt->entries[(page >> 30) & 0x1ff];
        if (!(pdpte & PTE_PRESENT) || !(pdpte & PTE_USER) ||
            (pdpte & PTE_HUGE)) return false;
        pd = (page_table_t *)(pdpte & PTE_FRAME_MASK);
        pde = pd->entries[(page >> 21) & 0x1ff];
        if (!(pde & PTE_PRESENT) || !(pde & PTE_USER) ||
            (pde & PTE_HUGE)) return false;
        pt = (page_table_t *)(pde & PTE_FRAME_MASK);
        pte = pt->entries[(page >> 12) & 0x1ff];
        if (!(pte & PTE_PRESENT) || !(pte & PTE_USER)) return false;
        if (page > ~(uintptr_t)0 - 0x1000) break;
        page += 0x1000;
    }
    return true;
}
