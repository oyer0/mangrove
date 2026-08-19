#include <vmm.h>
#include <pmm.h>
#include <heap.h>
#include <panic.h>

#define VMM_MAX_PROCESS_METADATA 64U

/* The high-half layout is fixed at Stage 3.  Direct map spans 128 PML4
 * entries; heap and ioremap occupy the next two; the image occupies slot 511.
 * Process roots may share exactly these supervisor-only root entries. */
#define VMM_DIRECT_MAP_PML4_FIRST ((PHYS_MAP_BASE >> 39) & 0x1ffU)
#define VMM_DIRECT_MAP_PML4_LAST \
    (((PHYS_MAP_BASE + PHYS_MAP_LIMIT - 1) >> 39) & 0x1ffU)
#define VMM_HEAP_PML4_INDEX ((KERNEL_HEAP_BASE >> 39) & 0x1ffU)
#define VMM_IOREMAP_PML4_INDEX ((IOREMAP_BASE >> 39) & 0x1ffU)
#define VMM_IMAGE_PML4_INDEX ((KERNEL_VIRT_BASE >> 39) & 0x1ffU)

typedef struct vmm_owned_frame {
    phys_addr_t physical;
    struct vmm_owned_frame *next;
} vmm_owned_frame_t;

typedef struct vmm_address_space_metadata {
    page_table_t *root;
    vmm_owned_frame_t *tables;
    vmm_owned_frame_t *leaves;
} vmm_address_space_metadata_t;

/* Bump allocator for permanent supervisor-only device mappings.  MMIO is
 * never part of PHYS_MAP_BASE: hardware ranges retain their own cache policy
 * and cannot be mistaken for PMM-owned RAM. */
static virt_addr_t ioremap_next = IOREMAP_BASE;

static page_table_t *kernel_pml4;
static page_table_t *current_pml4;

/* Metadata records exactly what a process may reclaim; page-table permission
 * bits are never consulted for ownership or freeing. */
static vmm_address_space_metadata_t address_space_metadata[
    VMM_MAX_PROCESS_METADATA];

static bool vmm_kernel_shared_pml4_index(u32 index)
{
    return (index >= VMM_DIRECT_MAP_PML4_FIRST &&
            index <= VMM_DIRECT_MAP_PML4_LAST) ||
           index == VMM_HEAP_PML4_INDEX ||
           index == VMM_IOREMAP_PML4_INDEX ||
           index == VMM_IMAGE_PML4_INDEX;
}

static vmm_address_space_metadata_t *vmm_metadata_find(const page_table_t *root)
{
    for (u32 i = 0; i < VMM_MAX_PROCESS_METADATA; i++) {
        if (address_space_metadata[i].root == root) {
            return &address_space_metadata[i];
        }
    }
    return 0;
}

static vmm_address_space_metadata_t *vmm_metadata_create(page_table_t *root)
{
    for (u32 i = 0; i < VMM_MAX_PROCESS_METADATA; i++) {
        if (!address_space_metadata[i].root) {
            address_space_metadata[i].root = root;
            address_space_metadata[i].tables = 0;
            address_space_metadata[i].leaves = 0;
            return &address_space_metadata[i];
        }
    }
    return 0;
}

static bool vmm_owned_contains(const vmm_owned_frame_t *frames,
                               phys_addr_t physical)
{
    physical &= PTE_FRAME_MASK;
    for (; frames; frames = frames->next) {
        if (frames->physical == physical) return true;
    }
    return false;
}

static bool vmm_owned_add(vmm_owned_frame_t **frames, phys_addr_t physical)
{
    vmm_owned_frame_t *node;

    physical &= PTE_FRAME_MASK;
    if (vmm_owned_contains(*frames, physical)) return false;
    node = (vmm_owned_frame_t *)kmalloc(sizeof(*node));
    if (!node) return false;
    node->physical = physical;
    node->next = *frames;
    *frames = node;
    return true;
}

static bool vmm_owned_remove(vmm_owned_frame_t **frames, phys_addr_t physical)
{
    vmm_owned_frame_t **cursor = frames;
    physical &= PTE_FRAME_MASK;
    while (*cursor) {
        if ((*cursor)->physical == physical) {
            vmm_owned_frame_t *node = *cursor;
            *cursor = node->next;
            kfree(node);
            return true;
        }
        cursor = &(*cursor)->next;
    }
    return false;
}

/* Page-table entries and CR3 always contain physical addresses.  All C
 * dereferences of those frames pass through the permanent direct map (the
 * helper uses the short bootstrap identity window only while this hierarchy
 * is being created). */
static page_table_t *vmm_table_from_phys(phys_addr_t phys)
{
    return (page_table_t *)phys_to_virt(phys & PTE_FRAME_MASK);
}

static phys_addr_t vmm_table_phys(const page_table_t *table)
{
    return virt_to_phys(table);
}

static page_table_t *vmm_alloc_table(void)
{
    phys_addr_t phys = pmm_alloc_frame();
    page_table_t *table = phys ? (page_table_t *)phys_to_virt(phys) : 0;
    if (table) {
        for (u32 i = 0; i < 512; i++) table->entries[i] = 0;
    }
    return table;
}

void vmm_init(void) {
    current_pml4 = 0;
    kernel_pml4 = 0;
    for (u32 i = 0; i < VMM_MAX_PROCESS_METADATA; i++) {
        address_space_metadata[i].root = 0;
        address_space_metadata[i].tables = 0;
        address_space_metadata[i].leaves = 0;
    }
    ioremap_next = IOREMAP_BASE;
}

/* Kernel mappings are restricted to the master root.  The lower-half user
 * mapper below is deliberately separate: it cannot clone or promote any
 * existing kernel table. */
static bool vmm_map_kernel_one(page_table_t *pml4, void *virtual_addr,
                               phys_addr_t physical_addr, u64 flags)
{
    u64 vaddr = (u64)virtual_addr;
    phys_addr_t paddr = physical_addr;

    if (!pml4 || pml4 != kernel_pml4 || !phys_map_contains(paddr) ||
        vaddr < VMM_USER_ADDRESS_LIMIT || (flags & PTE_USER)) {
        return false;
    }

    u64 pml4_idx = (vaddr >> 39) & 0x1FF;
    u64 pdpt_idx = (vaddr >> 30) & 0x1FF;
    u64 pd_idx   = (vaddr >> 21) & 0x1FF;
    u64 pt_idx   = (vaddr >> 12) & 0x1FF;

    if (!vmm_kernel_shared_pml4_index((u32)pml4_idx)) return false;

    page_table_t *pdpt = 0;
    if (!(pml4->entries[pml4_idx] & PTE_PRESENT)) {
        pdpt = vmm_alloc_table();
        if (!pdpt) return false;
        pml4->entries[pml4_idx] = vmm_table_phys(pdpt) | PTE_PRESENT | PTE_READWRITE;
    } else {
        pdpt = vmm_table_from_phys(pml4->entries[pml4_idx]);
        if (pml4->entries[pml4_idx] & PTE_USER) return false;
    }

    page_table_t *pd = 0;
    if (!(pdpt->entries[pdpt_idx] & PTE_PRESENT)) {
        pd = vmm_alloc_table();
        if (!pd) return false;
        pdpt->entries[pdpt_idx] = vmm_table_phys(pd) | PTE_PRESENT | PTE_READWRITE;
    } else {
        pd = vmm_table_from_phys(pdpt->entries[pdpt_idx]);
        if (pdpt->entries[pdpt_idx] & PTE_USER) return false;
    }

    page_table_t *pt = 0;
    if (!(pd->entries[pd_idx] & PTE_PRESENT)) {
        pt = vmm_alloc_table();
        if (!pt) return false;
        pd->entries[pd_idx] = vmm_table_phys(pt) | PTE_PRESENT | PTE_READWRITE;
    } else {
        pt = vmm_table_from_phys(pd->entries[pd_idx]);
        if (pd->entries[pd_idx] & PTE_USER) return false;
    }

    pt->entries[pt_idx] = (paddr & PTE_FRAME_MASK) | flags | PTE_PRESENT;
    if (pml4 == current_pml4) {
        __asm__ volatile("invlpg (%0)" :: "r"(virtual_addr) : "memory");
    }
    return true;
}

static page_table_t *vmm_user_child(vmm_address_space_metadata_t *metadata,
                                    page_table_t *parent, u32 index)
{
    u64 entry = parent->entries[index];
    page_table_t *child;

    if (entry & PTE_PRESENT) {
        if ((entry & (PTE_USER | PTE_HUGE)) != PTE_USER) return 0;
        child = vmm_table_from_phys(entry);
        return vmm_owned_contains(metadata->tables, vmm_table_phys(child))
            ? child : 0;
    }
    child = vmm_alloc_table();
    if (!child) return 0;
    if (!vmm_owned_add(&metadata->tables, vmm_table_phys(child))) {
        pmm_free_frame(vmm_table_phys(child));
        return 0;
    }
    parent->entries[index] = vmm_table_phys(child) |
        PTE_PRESENT | PTE_READWRITE | PTE_USER;
    return child;
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
    pdpt = vmm_table_from_phys(entry);
    entry = pdpt->entries[(virtual_addr >> 30) & 0x1ff];
    if (!(entry & PTE_PRESENT)) return false;
    if (entry & PTE_HUGE) return true;
    pd = vmm_table_from_phys(entry);
    entry = pd->entries[(virtual_addr >> 21) & 0x1ff];
    if (!(entry & PTE_PRESENT)) return false;
    if (entry & PTE_HUGE) return true;
    pt = vmm_table_from_phys(entry);
    return (pt->entries[(virtual_addr >> 12) & 0x1ff] & PTE_PRESENT) != 0;
}

bool vmm_map_user_page(page_table_t *pml4, void *virtual_addr,
                       phys_addr_t physical_addr, u64 flags)
{
    u64 virtual_value = (u64)virtual_addr;
    phys_addr_t physical_value = physical_addr;
    vmm_address_space_metadata_t *metadata;
    page_table_t *pdpt;
    page_table_t *pd;
    page_table_t *pt;
    u32 pml4_index;
    u64 kernel_pml4e_before;

    if (!pml4 || pml4 == kernel_pml4 ||
        (virtual_value & (VMM_PAGE_SIZE - 1)) ||
        (physical_value & (VMM_PAGE_SIZE - 1)) ||
        !phys_map_contains(physical_value) ||
        virtual_value >= VMM_USER_ADDRESS_LIMIT ||
        ((virtual_value >> 39) & 0x1ffU) >= 256 ||
        !(flags & PTE_USER) || vmm_page_present(pml4, virtual_value)) {
        return false;
    }
    pml4_index = (virtual_value >> 39) & 0x1ffU;
    kernel_pml4e_before = kernel_pml4->entries[pml4_index];
    metadata = vmm_metadata_find(pml4);
    if (!metadata || !vmm_owned_add(&metadata->leaves, physical_value)) {
        return false;
    }
    pdpt = vmm_user_child(metadata, pml4, pml4_index);
    pd = pdpt ? vmm_user_child(metadata, pdpt,
                               (virtual_value >> 30) & 0x1ffU) : 0;
    pt = pd ? vmm_user_child(metadata, pd,
                             (virtual_value >> 21) & 0x1ffU) : 0;
    if (!pt) {
        (void)vmm_owned_remove(&metadata->leaves, physical_value);
        return false;
    }
    pt->entries[(virtual_value >> 12) & 0x1ffU] =
        (physical_value & PTE_FRAME_MASK) | flags | PTE_USER | PTE_PRESENT;
    if (pml4 == current_pml4) {
        __asm__ volatile("invlpg (%0)" :: "r"(virtual_addr) : "memory");
    }
    /* PID 1 at 0x400000 exercises slot 0.  This makes the isolation rule
     * explicit: mapping a user leaf may not modify the master hierarchy. */
    if (kernel_pml4->entries[pml4_index] != kernel_pml4e_before ||
        !vmm_address_space_validate(pml4)) {
        panic("user mapping modified kernel page-table hierarchy");
    }
    return true;
}

bool vmm_unmap_user_page(page_table_t *pml4, void *virtual_addr,
                         phys_addr_t *out_physical_addr)
{
    u64 virtual_value = (u64)virtual_addr;
    page_table_t *pdpt;
    page_table_t *pd;
    page_table_t *pt;
    u64 entry;
    u64 *pte;
    vmm_address_space_metadata_t *metadata;

    if (!pml4 || pml4 == kernel_pml4 || !out_physical_addr ||
        (virtual_value & (VMM_PAGE_SIZE - 1)) ||
        virtual_value >= VMM_USER_ADDRESS_LIMIT ||
        ((virtual_value >> 39) & 0x1ffU) >= 256) {
        return false;
    }
    metadata = vmm_metadata_find(pml4);
    if (!metadata) return false;
    entry = pml4->entries[(virtual_value >> 39) & 0x1ff];
    if ((entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER) ||
        (entry & PTE_HUGE) ||
        !vmm_owned_contains(metadata->tables, entry & PTE_FRAME_MASK)) {
        return false;
    }
    pdpt = vmm_table_from_phys(entry);
    entry = pdpt->entries[(virtual_value >> 30) & 0x1ff];
    if ((entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER) ||
        (entry & PTE_HUGE) ||
        !vmm_owned_contains(metadata->tables, entry & PTE_FRAME_MASK)) {
        return false;
    }
    pd = vmm_table_from_phys(entry);
    entry = pd->entries[(virtual_value >> 21) & 0x1ff];
    if ((entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER) ||
        (entry & PTE_HUGE) ||
        !vmm_owned_contains(metadata->tables, entry & PTE_FRAME_MASK)) {
        return false;
    }
    pt = vmm_table_from_phys(entry);
    pte = &pt->entries[(virtual_value >> 12) & 0x1ff];
    if ((*pte & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER)) {
        return false;
    }
    *out_physical_addr = *pte & PTE_FRAME_MASK;
    if (!vmm_owned_remove(&metadata->leaves, *out_physical_addr)) {
        return false;
    }
    *pte = 0;
    if (pml4 == current_pml4) {
        __asm__ volatile("invlpg (%0)" :: "r"(virtual_addr) : "memory");
    }
    return true;
}

bool vmm_map(page_table_t *pml4, void *virtual_addr, phys_addr_t physical_addr,
             u64 flags)
{
    /* All process roots share this fixed high-half PML4 branch by physical
     * reference.  Updating the master hierarchy is therefore immediately
     * visible under every process CR3 without process-root publication. */
    return vmm_map_kernel_one(pml4, virtual_addr, physical_addr, flags);
}

void *vmm_map_mmio(phys_addr_t physical_addr, u64 size) {
    phys_addr_t start;
    u64 offset;
    u64 mapped_size;
    virt_addr_t virtual_start;

    if (!kernel_pml4 || !size ||
        physical_addr > ~(phys_addr_t)0 - (size - 1)) {
        return 0;
    }
    start = physical_addr & ~(phys_addr_t)(VMM_PAGE_SIZE - 1);
    offset = physical_addr - start;
    if (size > ~(u64)0 - offset) return 0;
    mapped_size = (size + offset + VMM_PAGE_SIZE - 1) &
                  ~(u64)(VMM_PAGE_SIZE - 1);
    if (!mapped_size || ioremap_next > IOREMAP_LIMIT - mapped_size) {
        return 0;
    }
    virtual_start = ioremap_next;
    ioremap_next += mapped_size;

    for (u64 page = 0; page < mapped_size; page += VMM_PAGE_SIZE) {
        if (!vmm_map(kernel_pml4,
                     (void *)(uintptr_t)(virtual_start + page),
                     start + page,
                     PTE_READWRITE | PTE_WRITETHROUGH |
                     PTE_CACHEDISABLE | PTE_NX)) {
            return 0;
        }
    }
    return (void *)(uintptr_t)(virtual_start + offset);
}

bool vmm_ioremap_contains(const void *address)
{
    virt_addr_t value = (virt_addr_t)(uintptr_t)address;
    return value >= IOREMAP_BASE && value < IOREMAP_LIMIT;
}

static u64 vmm_leaf_entry(page_table_t *root, u64 virtual_address)
{
    u64 entry;
    page_table_t *table;

    if (!root) return 0;
    entry = root->entries[(virtual_address >> 39) & 0x1ff];
    if (!(entry & PTE_PRESENT) || (entry & PTE_HUGE)) return 0;
    table = vmm_table_from_phys(entry);
    entry = table->entries[(virtual_address >> 30) & 0x1ff];
    if (!(entry & PTE_PRESENT) || (entry & PTE_HUGE)) return 0;
    table = vmm_table_from_phys(entry);
    entry = table->entries[(virtual_address >> 21) & 0x1ff];
    if (!(entry & PTE_PRESENT) || (entry & PTE_HUGE)) return 0;
    table = vmm_table_from_phys(entry);
    return table->entries[(virtual_address >> 12) & 0x1ff];
}

bool vmm_kernel_mapping_present(const void *address)
{
    return (vmm_leaf_entry(kernel_pml4, (u64)(uintptr_t)address) &
            PTE_PRESENT) != 0;
}

bool vmm_kernel_mapping_supervisor(const void *address)
{
    u64 virtual_address = (u64)(uintptr_t)address;
    u64 entry;
    page_table_t *table;

    if (!kernel_pml4) return false;
    entry = kernel_pml4->entries[(virtual_address >> 39) & 0x1ff];
    if ((entry & (PTE_PRESENT | PTE_USER | PTE_HUGE)) != PTE_PRESENT) return false;
    table = vmm_table_from_phys(entry);
    entry = table->entries[(virtual_address >> 30) & 0x1ff];
    if ((entry & (PTE_PRESENT | PTE_USER | PTE_HUGE)) != PTE_PRESENT) return false;
    table = vmm_table_from_phys(entry);
    entry = table->entries[(virtual_address >> 21) & 0x1ff];
    if ((entry & (PTE_PRESENT | PTE_USER | PTE_HUGE)) != PTE_PRESENT) return false;
    entry = vmm_leaf_entry(kernel_pml4, virtual_address);
    return (entry & (PTE_PRESENT | PTE_USER)) == PTE_PRESENT;
}

phys_addr_t vmm_virtual_to_physical(void *virtual_addr)
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
    
    page_table_t *pdpt = vmm_table_from_phys(current_pml4->entries[pml4_idx]);

    if (!(pdpt->entries[pdpt_idx] & PTE_PRESENT)) {
        return 0;
    }
    
    page_table_t *pd = vmm_table_from_phys(pdpt->entries[pdpt_idx]);

    if (!(pd->entries[pd_idx] & PTE_PRESENT))
    {
        return 0;
    }
    
    page_table_t *pt = vmm_table_from_phys(pd->entries[pd_idx]);
    
    if (!(pt->entries[pt_idx] & PTE_PRESENT))
    {
        return 0;
    }
    
    u64 frame = pt->entries[pt_idx] & PTE_FRAME_MASK;
    
    return frame + offset;

}

void vmm_set_kernel_pml4(phys_addr_t pml4_phys) {
    kernel_pml4 = vmm_table_from_phys(pml4_phys);
    current_pml4 = kernel_pml4;
}

page_table_t *vmm_get_kernel_pml4(void) {
    return kernel_pml4;
}

phys_addr_t vmm_get_kernel_pml4_phys(void)
{
    return vmm_table_phys(kernel_pml4);
}

bool vmm_map_physical_ram(phys_addr_t start, u64 page_count)
{
    if (!kernel_pml4 || start >= PHYS_MAP_LIMIT ||
        page_count > (PHYS_MAP_LIMIT - start) / VMM_PAGE_SIZE) {
        return false;
    }
    for (u64 i = 0; i < page_count; i++) {
        phys_addr_t phys = start + i * VMM_PAGE_SIZE;
        if (!vmm_map_kernel_one(kernel_pml4,
                         (void *)(uintptr_t)(PHYS_MAP_BASE + phys), phys,
                         PTE_READWRITE | PTE_NX)) {
            return false;
        }
    }
    if (kernel_pml4->entries[(PHYS_MAP_BASE >> 39) & 0x1ff] & PTE_USER) {
        return false;
    }
    return true;
}

void vmm_enable_direct_map(void)
{
    phys_addr_t kernel_phys;
    phys_addr_t current_phys;

    if (phys_map_is_ready()) return;
    kernel_phys = vmm_table_phys(kernel_pml4);
    current_phys = vmm_table_phys(current_pml4);
    phys_map_activate();
    kernel_pml4 = (page_table_t *)phys_to_virt(kernel_phys);
    current_pml4 = (page_table_t *)phys_to_virt(current_phys);
    for (u32 i = 0; i < VMM_MAX_PROCESS_METADATA; i++) {
        if (address_space_metadata[i].root) {
            address_space_metadata[i].root = (page_table_t *)phys_to_virt(
                vmm_table_phys(address_space_metadata[i].root));
        }
    }
}

bool vmm_direct_map_valid(phys_addr_t physical_addr)
{
    u64 vaddr;
    u64 entry;
    page_table_t *pdpt;
    page_table_t *pd;
    page_table_t *pt;

    if (!kernel_pml4 || !phys_map_is_ready() ||
        !phys_map_contains(physical_addr)) {
        return false;
    }
    vaddr = PHYS_MAP_BASE + physical_addr;
    entry = kernel_pml4->entries[(vaddr >> 39) & 0x1ff];
    if ((entry & (PTE_PRESENT | PTE_USER | PTE_HUGE)) != PTE_PRESENT) {
        return false;
    }
    pdpt = vmm_table_from_phys(entry);
    entry = pdpt->entries[(vaddr >> 30) & 0x1ff];
    if ((entry & (PTE_PRESENT | PTE_USER | PTE_HUGE)) != PTE_PRESENT) {
        return false;
    }
    pd = vmm_table_from_phys(entry);
    entry = pd->entries[(vaddr >> 21) & 0x1ff];
    if ((entry & (PTE_PRESENT | PTE_USER | PTE_HUGE)) != PTE_PRESENT) {
        return false;
    }
    pt = vmm_table_from_phys(entry);
    entry = pt->entries[(vaddr >> 12) & 0x1ff];
    return (entry & (PTE_PRESENT | PTE_USER)) == PTE_PRESENT &&
           (entry & PTE_FRAME_MASK) == (physical_addr & PTE_FRAME_MASK);
}

page_table_t *vmm_get_current_pml4(void)
{
    return current_pml4;
}

page_table_t *vmm_create_address_space(void)
{
    page_table_t *pml4;
    vmm_address_space_metadata_t *metadata;

    if (!kernel_pml4) return 0;
    /* Fresh lower half; fixed high-half branches are physical references to
     * the master kernel hierarchy.  No lower table can alias kernel state. */
    pml4 = vmm_alloc_table();
    if (!pml4) return 0;
    for (u32 i = 256; i < 512; i++) {
        if (vmm_kernel_shared_pml4_index(i)) {
            u64 entry = kernel_pml4->entries[i];
            if (entry & PTE_USER) {
                pmm_free_frame(vmm_table_phys(pml4));
                return 0;
            }
            pml4->entries[i] = entry;
        }
    }
    metadata = vmm_metadata_create(pml4);
    if (!metadata) {
        pmm_free_frame(vmm_table_phys(pml4));
        return 0;
    }
    if (!vmm_address_space_validate(pml4)) {
        metadata->root = 0;
        pmm_free_frame(vmm_table_phys(pml4));
        return 0;
    }
    return pml4;
}

static bool vmm_kernel_reaches_table(phys_addr_t physical)
{
    page_table_t *pdpt;
    page_table_t *pd;
    phys_addr_t root_phys;

    if (!kernel_pml4) return false;
    physical &= PTE_FRAME_MASK;
    root_phys = vmm_table_phys(kernel_pml4);
    if (physical == root_phys) return true;
    for (u32 i = 256; i < 512; i++) {
        u64 pml4e = kernel_pml4->entries[i];
        if (!(pml4e & PTE_PRESENT) || (pml4e & PTE_HUGE)) continue;
        if ((pml4e & PTE_FRAME_MASK) == physical) return true;
        pdpt = vmm_table_from_phys(pml4e);
        for (u32 j = 0; j < 512; j++) {
            u64 pdpte = pdpt->entries[j];
            if (!(pdpte & PTE_PRESENT) || (pdpte & PTE_HUGE)) continue;
            if ((pdpte & PTE_FRAME_MASK) == physical) return true;
            pd = vmm_table_from_phys(pdpte);
            for (u32 k = 0; k < 512; k++) {
                u64 pde = pd->entries[k];
                if (!(pde & PTE_PRESENT) || (pde & PTE_HUGE)) continue;
                if ((pde & PTE_FRAME_MASK) == physical) return true;
            }
        }
    }
    return false;
}

bool vmm_address_space_validate(const page_table_t *pml4)
{
    vmm_address_space_metadata_t *metadata;

    if (!pml4 || pml4 == kernel_pml4) return false;
    metadata = vmm_metadata_find(pml4);
    if (!metadata) return false;

    for (u32 i = 0; i < 256; i++) {
        u64 entry = pml4->entries[i];
        if (!entry) continue;
        if ((entry & (PTE_PRESENT | PTE_USER | PTE_HUGE)) !=
                (PTE_PRESENT | PTE_USER) ||
            !vmm_owned_contains(metadata->tables, entry & PTE_FRAME_MASK) ||
            (kernel_pml4->entries[i] & PTE_PRESENT)) {
            return false;
        }
    }
    for (u32 i = 256; i < 512; i++) {
        u64 entry = pml4->entries[i];
        if (vmm_kernel_shared_pml4_index(i)) {
            if (entry != kernel_pml4->entries[i] || (entry & PTE_USER)) {
                return false;
            }
        } else if (entry) {
            return false;
        }
    }
    for (vmm_owned_frame_t *node = metadata->tables; node;
         node = node->next) {
        if (vmm_kernel_reaches_table(node->physical)) return false;
    }
    return true;
}

static bool vmm_table_supervisor_only(const page_table_t *table, u32 level)
{
    for (u32 i = 0; i < 512; i++) {
        u64 entry = table->entries[i];
        if (!(entry & PTE_PRESENT)) continue;
        if (entry & PTE_USER) return false;
        if (level > 1 && !(entry & PTE_HUGE) &&
            !vmm_table_supervisor_only(vmm_table_from_phys(entry), level - 1)) {
            return false;
        }
    }
    return true;
}

bool vmm_kernel_mappings_supervisor_only(void)
{
    return kernel_pml4 && vmm_table_supervisor_only(kernel_pml4, 4);
}

void vmm_destroy_address_space(page_table_t *pml4)
{
    vmm_address_space_metadata_t *metadata;
    vmm_owned_frame_t *node;

    if (!pml4 || pml4 == kernel_pml4) return;
    metadata = vmm_metadata_find(pml4);
    if (!metadata) return;
    if (current_pml4 == pml4) {
        vmm_switch_address_space(kernel_pml4);
    }
    while ((node = metadata->leaves) != 0) {
        metadata->leaves = node->next;
        pmm_free_frame(node->physical);
        kfree(node);
    }
    while ((node = metadata->tables) != 0) {
        metadata->tables = node->next;
        if (vmm_kernel_reaches_table(node->physical)) {
            panic("process owns shared kernel page table");
        }
        pmm_free_frame(node->physical);
        kfree(node);
    }
    metadata->root = 0;
    pmm_free_frame(vmm_table_phys(pml4));
}

void vmm_switch_address_space(page_table_t *pml4)
{
    if (!pml4 || pml4 == current_pml4) return;
    current_pml4 = pml4;
    __asm__ volatile("mov %0, %%cr3" :: "r"(vmm_table_phys(pml4)) : "memory");
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
        pdpt = vmm_table_from_phys(pml4e);
        pdpte = pdpt->entries[(page >> 30) & 0x1ff];
        if (!(pdpte & PTE_PRESENT) || !(pdpte & PTE_USER) ||
            (pdpte & PTE_HUGE)) return false;
        pd = vmm_table_from_phys(pdpte);
        pde = pd->entries[(page >> 21) & 0x1ff];
        if (!(pde & PTE_PRESENT) || !(pde & PTE_USER) ||
            (pde & PTE_HUGE)) return false;
        pt = vmm_table_from_phys(pde);
        pte = pt->entries[(page >> 12) & 0x1ff];
        if (!(pte & PTE_PRESENT) || !(pte & PTE_USER)) return false;
        if (page > ~(uintptr_t)0 - 0x1000) break;
        page += 0x1000;
    }
    return true;
}
