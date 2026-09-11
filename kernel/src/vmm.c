/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <vmm.h>
#include <cpu.h>
#include <pmm.h>
#include <heap.h>
#include <panic.h>
#include <spinlock.h>
#include <lapic.h>
#include <irq.h>
#include <timer.h>
#include <scheduler.h>
#include <cpu_relax.h>
#include <mutex.h>

#define VMM_MAX_PROCESS_METADATA 64U
#define VMM_CPU_MASK_WORDS ((CPU_MAX_COUNT + 63U) / 64U)
#define VMM_TLB_SHOOTDOWN_TIMEOUT_US 1000000ULL
#define VMM_TLB_SHOOTDOWN_FALLBACK_SPINS 50000000U

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
    mutex_t operation_lock;
    volatile u64 active_cpu_mask[VMM_CPU_MASK_WORDS];
    bool destroy_pending;
    bool shootdown_pending;
} vmm_address_space_metadata_t;

typedef struct {
    volatile u32 active;
    volatile page_table_t *address_space;
    volatile u64 pending[VMM_CPU_MASK_WORDS];
    volatile u64 acknowledged[VMM_CPU_MASK_WORDS];
} vmm_tlb_shootdown_request_t;

/* Bump allocator for permanent supervisor-only device mappings.  MMIO is
 * never part of PHYS_MAP_BASE: hardware ranges retain their own cache policy
 * and cannot be mistaken for PMM-owned RAM. */
static virt_addr_t ioremap_next = IOREMAP_BASE;

static page_table_t *kernel_pml4;
/* A temporary low identity mapping is kept only while an AP may still be
 * leaving the SIPI trampoline.  It is explicitly tolerated by address-space
 * validation until the mapping is removed and its empty lower hierarchy is
 * pruned. */
static bool bootstrap_mapping_active;

/* Kernel mappings and process page-table ownership have different callers.
 * Separate locks avoid making heap growth invert the user metadata path. */
static spinlock_t vmm_kernel_lock;
static spinlock_t vmm_metadata_lock;
static spinlock_t vmm_shootdown_lock;
static vmm_tlb_shootdown_request_t vmm_shootdown_request;

/* The active address space belongs to the executing CPU.  GS-backed CPU
 * state is initialized before VMM setup, including during the bootstrap
 * window before the heap-backed CPU topology is published. */
#define active_pml4 (cpu_current()->current_pml4)

/* Metadata records exactly what a process may reclaim; page-table permission
 * bits are never consulted for ownership or freeing. */
static vmm_address_space_metadata_t address_space_metadata[
    VMM_MAX_PROCESS_METADATA];

static bool vmm_address_space_validate_locked(const page_table_t *pml4);
static phys_addr_t vmm_table_phys(const page_table_t *table);

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
        if (!address_space_metadata[i].root &&
            !address_space_metadata[i].destroy_pending) {
            address_space_metadata[i].root = root;
            address_space_metadata[i].tables = 0;
            address_space_metadata[i].leaves = 0;
            mutex_init(&address_space_metadata[i].operation_lock);
            for (u32 word = 0; word < VMM_CPU_MASK_WORDS; word++)
                address_space_metadata[i].active_cpu_mask[word] = 0;
            address_space_metadata[i].destroy_pending = false;
            address_space_metadata[i].shootdown_pending = false;
            return &address_space_metadata[i];
        }
    }
    return 0;
}

static bool vmm_cpu_mask_empty(const volatile u64 *mask)
{
    for (u32 word = 0; word < VMM_CPU_MASK_WORDS; word++) {
        if (mask[word]) return false;
    }
    return true;
}

static void vmm_cpu_mask_copy(u64 *destination, const volatile u64 *source)
{
    for (u32 word = 0; word < VMM_CPU_MASK_WORDS; word++)
        destination[word] = __atomic_load_n(&source[word], __ATOMIC_ACQUIRE);
}

static void vmm_reload_current_cr3(void)
{
    if (active_pml4) {
        __asm__ volatile("mov %0, %%cr3" ::
                         "r"(vmm_table_phys(active_pml4)) : "memory");
    }
}

static u64 vmm_irq_save(void)
{
    u64 flags;

    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void vmm_irq_restore(u64 flags)
{
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
}

static bool vmm_interrupts_enabled(void)
{
    u64 flags;

    __asm__ volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    return (flags & (1ULL << 9)) != 0;
}

static bool vmm_tlb_acknowledged(const u64 *targets)
{
    for (u32 word = 0; word < VMM_CPU_MASK_WORDS; word++) {
        u64 acknowledged = __atomic_load_n(
            &vmm_shootdown_request.acknowledged[word], __ATOMIC_ACQUIRE);
        if ((acknowledged & targets[word]) != targets[word])
            return false;
    }
    return true;
}

static void vmm_tlb_shootdown(u64 *targets, page_table_t *address_space)
{
    timer_monotonic_deadline_t deadline;
    u64 saved_flags;
    bool complete = false;

    if (!targets || vmm_cpu_mask_empty(targets))
        return;
    if (!lapic_enabled()) {
        panic("vmm: TLB shootdown requested without LAPIC");
    }

    if (!vmm_interrupts_enabled())
        panic("vmm: TLB shootdown requested with interrupts disabled");

    spin_lock(&vmm_shootdown_lock);
    saved_flags = vmm_irq_save();
    for (u32 word = 0; word < VMM_CPU_MASK_WORDS; word++) {
        __atomic_store_n(&vmm_shootdown_request.pending[word], targets[word],
                          __ATOMIC_RELAXED);
        __atomic_store_n(&vmm_shootdown_request.acknowledged[word], 0,
                          __ATOMIC_RELAXED);
    }
    __atomic_store_n(&vmm_shootdown_request.address_space, address_space,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&vmm_shootdown_request.active, 1U, __ATOMIC_RELEASE);

    for (u32 word = 0; word < VMM_CPU_MASK_WORDS; word++) {
        u64 pending = targets[word];
        while (pending) {
            u32 bit = (u32)__builtin_ctzll(pending);
            u32 index = word * 64U + bit;
            cpu_local_t *cpu = cpu_by_index(index);
            if (!cpu || !cpu->online ||
                !lapic_send_fixed_ipi(cpu->apic_id,
                                      IRQ_VECTOR_TLB_SHOOTDOWN)) {
                __atomic_store_n(&vmm_shootdown_request.active, 0U,
                                 __ATOMIC_RELEASE);
                spin_unlock_irqrestore(&vmm_shootdown_lock, saved_flags);
                panic("vmm: TLB shootdown IPI delivery failed");
            }
            pending &= pending - 1U;
        }
    }

    if (timer_monotonic_deadline_start(&deadline,
                                       VMM_TLB_SHOOTDOWN_TIMEOUT_US)) {
        while (!timer_monotonic_deadline_expired(&deadline)) {
            if (vmm_tlb_acknowledged(targets)) {
                complete = true;
                break;
            }
            cpu_relax();
        }
    } else {
        for (u32 i = 0; i < VMM_TLB_SHOOTDOWN_FALLBACK_SPINS; i++) {
            if (vmm_tlb_acknowledged(targets)) {
                complete = true;
                break;
            }
            cpu_relax();
        }
    }
    if (!complete) {
        __atomic_store_n(&vmm_shootdown_request.active, 0U,
                         __ATOMIC_RELEASE);
        spin_unlock_irqrestore(&vmm_shootdown_lock, saved_flags);
        panic("vmm: TLB shootdown acknowledgement timeout");
    }
    __atomic_store_n(&vmm_shootdown_request.active, 0U, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&vmm_shootdown_lock, saved_flags);
}

static void vmm_tlb_shootdown_handler(struct cpu_registers *regs)
{
    cpu_local_t *cpu = cpu_current();
    u32 index;
    u32 word;
    u64 bit;
    u64 pending;
    page_table_t *address_space;

    (void)regs;
    if (!cpu) return;
    if (!__atomic_load_n(&vmm_shootdown_request.active, __ATOMIC_ACQUIRE))
        return;
    index = cpu->index;
    if (index >= CPU_MAX_COUNT) return;
    word = index / 64U;
    bit = 1ULL << (index % 64U);
    pending = __atomic_load_n(&vmm_shootdown_request.pending[word],
                              __ATOMIC_ACQUIRE);
    if (!(pending & bit)) return;
    address_space = (page_table_t *)(uintptr_t)__atomic_load_n(
        &vmm_shootdown_request.address_space, __ATOMIC_ACQUIRE);
    if (!address_space || active_pml4 == address_space)
        vmm_reload_current_cr3();
    __atomic_fetch_or(&vmm_shootdown_request.acknowledged[word], bit,
                      __ATOMIC_RELEASE);
}

static void vmm_capture_active_targets_locked(
    const vmm_address_space_metadata_t *metadata, u64 *targets)
{
    u32 index = cpu_current_index();

    vmm_cpu_mask_copy(targets, metadata->active_cpu_mask);
    if (index < CPU_MAX_COUNT)
        targets[index / 64U] &= ~(1ULL << (index % 64U));
}

static void vmm_publish_kernel_mappings(void)
{
    u64 metadata_flags = spin_lock_irqsave(&vmm_metadata_lock);
    u64 kernel_flags = spin_lock_irqsave(&vmm_kernel_lock);

    for (u32 i = 0; i < VMM_MAX_PROCESS_METADATA; i++) {
        page_table_t *root = address_space_metadata[i].root;
        if (!root) continue;
        for (u32 index = 256; index < 512; index++) {
            if (vmm_kernel_shared_pml4_index(index))
                root->entries[index] = kernel_pml4->entries[index];
        }
    }
    spin_unlock_irqrestore(&vmm_kernel_lock, kernel_flags);
    spin_unlock_irqrestore(&vmm_metadata_lock, metadata_flags);

    /* vmm_map_kernel_one_internal() already invalidates the local mapping.
     * Avoid reloading CR3 here: during early bootstrap the new kernel root is
     * intentionally incomplete until all kernel sections and RAM mappings
     * have been installed. */
    u64 targets[VMM_CPU_MASK_WORDS] = {0};
    u32 current = cpu_current_index();
    for (u32 index = 0; index < cpu_count(); index++) {
        cpu_local_t *cpu = cpu_by_index(index);
        if (cpu && cpu->online && index != current)
            targets[index / 64U] |= 1ULL << (index % 64U);
    }
    vmm_tlb_shootdown(targets, 0);
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

static void vmm_owned_insert(vmm_owned_frame_t **frames,
                             vmm_owned_frame_t *node,
                             phys_addr_t physical)
{
    node->physical = physical & PTE_FRAME_MASK;
    node->next = *frames;
    *frames = node;
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

static bool vmm_table_empty(const page_table_t *table)
{
    if (!table)
        return false;
    for (u32 i = 0; i < 512; i++) {
        if (table->entries[i] & PTE_PRESENT)
            return false;
    }
    return true;
}

void vmm_init(void) {
    spinlock_init(&vmm_kernel_lock);
    spinlock_init(&vmm_metadata_lock);
    spinlock_init(&vmm_shootdown_lock);
    __atomic_store_n(&vmm_shootdown_request.active, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&vmm_shootdown_request.address_space, 0,
                     __ATOMIC_RELAXED);
    for (u32 word = 0; word < VMM_CPU_MASK_WORDS; word++) {
        __atomic_store_n(&vmm_shootdown_request.pending[word], 0,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&vmm_shootdown_request.acknowledged[word], 0,
                         __ATOMIC_RELAXED);
    }
    if (!irq_register_vector(IRQ_VECTOR_TLB_SHOOTDOWN,
                             vmm_tlb_shootdown_handler))
        panic("vmm: failed to register TLB shootdown vector");
    active_pml4 = 0;
    kernel_pml4 = 0;
    bootstrap_mapping_active = false;
    for (u32 i = 0; i < VMM_MAX_PROCESS_METADATA; i++) {
        address_space_metadata[i].root = 0;
        address_space_metadata[i].tables = 0;
        address_space_metadata[i].leaves = 0;
        mutex_init(&address_space_metadata[i].operation_lock);
        for (u32 word = 0; word < VMM_CPU_MASK_WORDS; word++)
            address_space_metadata[i].active_cpu_mask[word] = 0;
        address_space_metadata[i].destroy_pending = false;
        address_space_metadata[i].shootdown_pending = false;
    }
    ioremap_next = IOREMAP_BASE;
}

/* Kernel mappings are restricted to the master root.  The lower-half user
 * mapper below is deliberately separate: it cannot clone or promote any
 * existing kernel table. */
static bool vmm_map_kernel_one_internal(page_table_t *pml4,
                                        void *virtual_addr,
                                        phys_addr_t physical_addr,
                                        u64 flags, bool allow_bootstrap_low)
{
    u64 vaddr = (u64)virtual_addr;
    phys_addr_t paddr = physical_addr;
    bool low_identity = vaddr < VMM_USER_ADDRESS_LIMIT;

    if (!pml4 || pml4 != kernel_pml4 || !phys_map_contains(paddr) ||
        (flags & PTE_USER) ||
        (low_identity && (!allow_bootstrap_low ||
                          vaddr >= 0x00100000ULL || paddr != vaddr)) ||
        (!low_identity &&
         !vmm_kernel_shared_pml4_index((u32)((vaddr >> 39) & 0x1ff)))) {
        return false;
    }

    u64 pml4_idx = (vaddr >> 39) & 0x1FF;
    u64 pdpt_idx = (vaddr >> 30) & 0x1FF;
    u64 pd_idx   = (vaddr >> 21) & 0x1FF;
    u64 pt_idx   = (vaddr >> 12) & 0x1FF;

    if (!low_identity && !vmm_kernel_shared_pml4_index((u32)pml4_idx))
        return false;

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
    if (pml4 == active_pml4) {
        __asm__ volatile("invlpg (%0)" :: "r"(virtual_addr) : "memory");
    }
    return true;
}

static bool vmm_map_kernel_one(page_table_t *pml4, void *virtual_addr,
                               phys_addr_t physical_addr, u64 flags)
{
    return vmm_map_kernel_one_internal(pml4, virtual_addr, physical_addr,
                                       flags, false);
}

static page_table_t *vmm_user_child_reserved(
    vmm_address_space_metadata_t *metadata, page_table_t *parent, u32 index,
    page_table_t **reserved_table, vmm_owned_frame_t **reserved_node)
{
    u64 entry = parent->entries[index];
    page_table_t *child;

    if (entry & PTE_PRESENT) {
        if ((entry & (PTE_USER | PTE_HUGE)) != PTE_USER) return 0;
        child = vmm_table_from_phys(entry);
        return vmm_owned_contains(metadata->tables, vmm_table_phys(child))
            ? child : 0;
    }
    if (!reserved_table || !*reserved_table ||
        !reserved_node || !*reserved_node) return 0;
    child = *reserved_table;
    vmm_owned_insert(&metadata->tables, *reserved_node,
                     vmm_table_phys(child));
    *reserved_table = 0;
    *reserved_node = 0;
    parent->entries[index] = vmm_table_phys(child) |
        PTE_PRESENT | PTE_READWRITE | PTE_USER;
    return child;
}

/* Validate the lower-half path before changing it.  Resource allocation is
 * deliberately performed before taking vmm_metadata_lock: ownership nodes
 * come from the heap, and heap growth maps kernel pages through VMM. */
static bool vmm_user_path_available(vmm_address_space_metadata_t *metadata,
                                    page_table_t *pml4, u64 virtual_addr)
{
    page_table_t *table = pml4;
    u32 indexes[] = {
        (u32)((virtual_addr >> 39) & 0x1ffU),
        (u32)((virtual_addr >> 30) & 0x1ffU),
        (u32)((virtual_addr >> 21) & 0x1ffU),
    };

    for (u32 level = 0; level < 3; level++) {
        u64 entry = table->entries[indexes[level]];
        if (!(entry & PTE_PRESENT)) return true;
        if ((entry & (PTE_USER | PTE_HUGE)) != PTE_USER ||
            !vmm_owned_contains(metadata->tables, entry & PTE_FRAME_MASK))
            return false;
        table = vmm_table_from_phys(entry);
    }

    return !(table->entries[(virtual_addr >> 12) & 0x1ffU] & PTE_PRESENT);
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
    page_table_t *reserved_tables[3] = {0};
    vmm_owned_frame_t *reserved_table_nodes[3] = {0};
    vmm_owned_frame_t *reserved_leaf_node = 0;
    u32 pml4_index;
    u64 kernel_pml4e_before;
    u64 shootdown_targets[VMM_CPU_MASK_WORDS] = {0};
    vmm_address_space_metadata_t *operation_metadata;
    bool operation_locked = false;

    if (!pml4 || pml4 == kernel_pml4 ||
        (virtual_value & (VMM_PAGE_SIZE - 1)) ||
        (physical_value & (VMM_PAGE_SIZE - 1)) ||
        !phys_map_contains(physical_value) ||
        virtual_value >= VMM_USER_ADDRESS_LIMIT ||
        ((virtual_value >> 39) & 0x1ffU) >= 256 ||
        !(flags & PTE_USER)) {
        return false;
    }
    u64 saved_flags = spin_lock_irqsave(&vmm_metadata_lock);
    metadata = vmm_metadata_find(pml4);
    if (!metadata || metadata->destroy_pending ||
        !vmm_user_path_available(metadata, pml4, virtual_value)) {
        spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
        return false;
    }
    operation_metadata = metadata;
    pml4_index = (virtual_value >> 39) & 0x1ffU;
    u64 kernel_flags = spin_lock_irqsave(&vmm_kernel_lock);
    if (!kernel_pml4) {
        spin_unlock_irqrestore(&vmm_kernel_lock, kernel_flags);
        spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
        return false;
    }
    kernel_pml4e_before = kernel_pml4->entries[pml4_index];
    spin_unlock_irqrestore(&vmm_kernel_lock, kernel_flags);
    spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);

    if (!mutex_lock(&operation_metadata->operation_lock))
        return false;
    operation_locked = true;

    reserved_leaf_node = (vmm_owned_frame_t *)kmalloc(
        sizeof(*reserved_leaf_node));
    if (!reserved_leaf_node) goto reserve_failure;
    for (u32 index = 0; index < 3; index++) {
        reserved_tables[index] = vmm_alloc_table();
        if (!reserved_tables[index]) goto reserve_failure;
        reserved_table_nodes[index] = (vmm_owned_frame_t *)kmalloc(
            sizeof(*reserved_table_nodes[index]));
        if (!reserved_table_nodes[index]) goto reserve_failure;
    }

    saved_flags = spin_lock_irqsave(&vmm_metadata_lock);
    metadata = vmm_metadata_find(pml4);
    if (!metadata || metadata->destroy_pending ||
        !vmm_user_path_available(metadata, pml4, virtual_value) ||
        vmm_owned_contains(metadata->leaves, physical_value)) {
        spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
        goto reserve_failure;
    }
    pdpt = vmm_user_child_reserved(
        metadata, pml4, pml4_index, &reserved_tables[0],
        &reserved_table_nodes[0]);
    pd = pdpt ? vmm_user_child_reserved(
                    metadata, pdpt, (virtual_value >> 30) & 0x1ffU,
                    &reserved_tables[1], &reserved_table_nodes[1]) : 0;
    pt = pd ? vmm_user_child_reserved(
                  metadata, pd, (virtual_value >> 21) & 0x1ffU,
                  &reserved_tables[2], &reserved_table_nodes[2]) : 0;
    if (!pt) {
        spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
        goto reserve_failure;
    }
    metadata->shootdown_pending = true;
    vmm_owned_insert(&metadata->leaves, reserved_leaf_node, physical_value);
    reserved_leaf_node = 0;
    pt->entries[(virtual_value >> 12) & 0x1ffU] =
        (physical_value & PTE_FRAME_MASK) | flags | PTE_USER | PTE_PRESENT;
    if (pml4 == active_pml4) {
        __asm__ volatile("invlpg (%0)" :: "r"(virtual_addr) : "memory");
    }
    /* PID 1 at 0x400000 exercises slot 0.  This makes the isolation rule
     * explicit: mapping a user leaf may not modify the master hierarchy. */
    kernel_flags = spin_lock_irqsave(&vmm_kernel_lock);
    bool kernel_changed = kernel_pml4->entries[pml4_index] !=
                          kernel_pml4e_before;
    spin_unlock_irqrestore(&vmm_kernel_lock, kernel_flags);
    if (kernel_changed ||
        !vmm_address_space_validate_locked(pml4)) {
        panic("user mapping modified kernel page-table hierarchy");
    }
    vmm_capture_active_targets_locked(metadata, shootdown_targets);
    spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
    /* The target CPUs must be able to take the shootdown interrupt.  The
     * metadata mutation is serialized by operation_lock, so it is safe to
     * release this short IRQ-disabled section before waiting for them. */
    vmm_tlb_shootdown(shootdown_targets, pml4);
    saved_flags = spin_lock_irqsave(&vmm_metadata_lock);
    metadata = vmm_metadata_find(pml4);
    if (metadata)
        metadata->shootdown_pending = false;
    spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
    mutex_unlock(&operation_metadata->operation_lock);
    operation_locked = false;
    for (u32 index = 0; index < 3; index++) {
        if (reserved_tables[index])
            pmm_free_frame(vmm_table_phys(reserved_tables[index]));
        if (reserved_table_nodes[index]) kfree(reserved_table_nodes[index]);
    }
    return true;

reserve_failure:
    if (operation_locked)
        mutex_unlock(&operation_metadata->operation_lock);
    if (reserved_leaf_node) kfree(reserved_leaf_node);
    for (u32 index = 0; index < 3; index++) {
        if (reserved_tables[index])
            pmm_free_frame(vmm_table_phys(reserved_tables[index]));
        if (reserved_table_nodes[index]) kfree(reserved_table_nodes[index]);
    }
    return false;
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
    u64 shootdown_targets[VMM_CPU_MASK_WORDS] = {0};

    if (!pml4 || pml4 == kernel_pml4 || !out_physical_addr ||
        (virtual_value & (VMM_PAGE_SIZE - 1)) ||
        virtual_value >= VMM_USER_ADDRESS_LIMIT ||
        ((virtual_value >> 39) & 0x1ffU) >= 256) {
        return false;
    }
    u64 saved_flags = spin_lock_irqsave(&vmm_metadata_lock);
    metadata = vmm_metadata_find(pml4);
    if (!metadata || metadata->destroy_pending) {
        spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
        return false;
    }
    vmm_address_space_metadata_t *operation_metadata = metadata;
    spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
    if (!mutex_lock(&operation_metadata->operation_lock))
        return false;

    saved_flags = spin_lock_irqsave(&vmm_metadata_lock);
    metadata = vmm_metadata_find(pml4);
    if (!metadata || metadata->destroy_pending) {
        spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
        mutex_unlock(&operation_metadata->operation_lock);
        return false;
    }
    entry = pml4->entries[(virtual_value >> 39) & 0x1ff];
    if ((entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER) ||
        (entry & PTE_HUGE) ||
        !vmm_owned_contains(metadata->tables, entry & PTE_FRAME_MASK)) {
        spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
        mutex_unlock(&operation_metadata->operation_lock);
        return false;
    }
    pdpt = vmm_table_from_phys(entry);
    entry = pdpt->entries[(virtual_value >> 30) & 0x1ff];
    if ((entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER) ||
        (entry & PTE_HUGE) ||
        !vmm_owned_contains(metadata->tables, entry & PTE_FRAME_MASK)) {
        spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
        mutex_unlock(&operation_metadata->operation_lock);
        return false;
    }
    pd = vmm_table_from_phys(entry);
    entry = pd->entries[(virtual_value >> 21) & 0x1ff];
    if ((entry & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER) ||
        (entry & PTE_HUGE) ||
        !vmm_owned_contains(metadata->tables, entry & PTE_FRAME_MASK)) {
        spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
        mutex_unlock(&operation_metadata->operation_lock);
        return false;
    }
    pt = vmm_table_from_phys(entry);
    pte = &pt->entries[(virtual_value >> 12) & 0x1ff];
    if ((*pte & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER)) {
        spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
        mutex_unlock(&operation_metadata->operation_lock);
        return false;
    }
    *out_physical_addr = *pte & PTE_FRAME_MASK;
    if (!vmm_owned_remove(&metadata->leaves, *out_physical_addr)) {
        spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
        mutex_unlock(&metadata->operation_lock);
        return false;
    }
    metadata->shootdown_pending = true;
    *pte = 0;
    if (pml4 == active_pml4) {
        __asm__ volatile("invlpg (%0)" :: "r"(virtual_addr) : "memory");
    }
    vmm_capture_active_targets_locked(metadata, shootdown_targets);
    spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
    vmm_tlb_shootdown(shootdown_targets, pml4);
    saved_flags = spin_lock_irqsave(&vmm_metadata_lock);
    metadata = vmm_metadata_find(pml4);
    if (metadata)
        metadata->shootdown_pending = false;
    spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
    mutex_unlock(&operation_metadata->operation_lock);
    return true;
}

bool vmm_map(page_table_t *pml4, void *virtual_addr, phys_addr_t physical_addr,
             u64 flags)
{
    /* All process roots share this fixed high-half PML4 branch by physical
     * reference.  Updating the master hierarchy is therefore immediately
     * visible under every process CR3 without process-root publication. */
    u64 saved_flags = spin_lock_irqsave(&vmm_kernel_lock);
    bool result = vmm_map_kernel_one(pml4, virtual_addr, physical_addr, flags);
    spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
    if (result)
        vmm_publish_kernel_mappings();
    return result;
}

bool vmm_map_bootstrap_page(phys_addr_t physical_addr)
{
    u64 saved_flags;
    bool result;

    if ((physical_addr & (VMM_PAGE_SIZE - 1)) ||
        physical_addr >= 0x00100000ULL)
        return false;
    saved_flags = spin_lock_irqsave(&vmm_kernel_lock);
    if (bootstrap_mapping_active) {
        result = false;
    } else {
        result = vmm_map_kernel_one_internal(
            kernel_pml4, (void *)(uintptr_t)physical_addr, physical_addr,
            PTE_READWRITE, true);
        if (result)
            bootstrap_mapping_active = true;
    }
    spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
    return result;
}

bool vmm_unmap_bootstrap_page(phys_addr_t physical_addr)
{
    u64 saved_flags;
    u64 vaddr = physical_addr;
    u64 entry;
    phys_addr_t free_pdpt = 0;
    phys_addr_t free_pd = 0;
    phys_addr_t free_pt = 0;
    u32 pml4_idx = (u32)((vaddr >> 39) & 0x1ff);
    u32 pdpt_idx = (u32)((vaddr >> 30) & 0x1ff);
    u32 pd_idx = (u32)((vaddr >> 21) & 0x1ff);
    u32 pt_idx = (u32)((vaddr >> 12) & 0x1ff);
    page_table_t *pdpt;
    page_table_t *pd;
    page_table_t *pt;

    if ((physical_addr & (VMM_PAGE_SIZE - 1)) ||
        physical_addr >= 0x00100000ULL)
        return false;

    saved_flags = spin_lock_irqsave(&vmm_kernel_lock);
    if (!kernel_pml4 || !bootstrap_mapping_active) {
        spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
        return false;
    }
    entry = kernel_pml4->entries[pml4_idx];
    if (!(entry & PTE_PRESENT) || (entry & (PTE_HUGE | PTE_USER))) {
        spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
        return false;
    }
    pdpt = vmm_table_from_phys(entry);
    entry = pdpt->entries[pdpt_idx];
    if (!(entry & PTE_PRESENT) || (entry & (PTE_HUGE | PTE_USER))) {
        spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
        return false;
    }
    pd = vmm_table_from_phys(entry);
    entry = pd->entries[pd_idx];
    if (!(entry & PTE_PRESENT) || (entry & (PTE_HUGE | PTE_USER))) {
        spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
        return false;
    }
    pt = vmm_table_from_phys(entry);
    entry = pt->entries[pt_idx];
    if (!(entry & PTE_PRESENT) ||
        (entry & PTE_FRAME_MASK) != physical_addr) {
        spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
        return false;
    }
    pt->entries[pt_idx] = 0;
    if (vmm_table_empty(pt)) {
        free_pt = entry & PTE_FRAME_MASK;
        pd->entries[pd_idx] = 0;
        if (vmm_table_empty(pd)) {
            free_pd = pdpt->entries[pdpt_idx] & PTE_FRAME_MASK;
            pdpt->entries[pdpt_idx] = 0;
            if (vmm_table_empty(pdpt)) {
                free_pdpt = kernel_pml4->entries[pml4_idx] & PTE_FRAME_MASK;
                kernel_pml4->entries[pml4_idx] = 0;
            }
        }
    }
    bootstrap_mapping_active = false;
    __asm__ volatile("invlpg (%0)" :: "r"((void *)(uintptr_t)vaddr)
                     : "memory");
    spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
    if (free_pdpt)
        pmm_free_frame(free_pdpt);
    if (free_pd)
        pmm_free_frame(free_pd);
    if (free_pt)
        pmm_free_frame(free_pt);
    return true;
}

void *vmm_map_mmio(phys_addr_t physical_addr, u64 size) {
    phys_addr_t start;
    u64 offset;
    u64 mapped_size;
    virt_addr_t virtual_start;
    bool changed = false;

    u64 saved_flags = spin_lock_irqsave(&vmm_kernel_lock);

    if (!kernel_pml4 || !size ||
        physical_addr > ~(phys_addr_t)0 - (size - 1)) {
        spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
        return 0;
    }
    start = physical_addr & ~(phys_addr_t)(VMM_PAGE_SIZE - 1);
    offset = physical_addr - start;
    if (size > ~(u64)0 - offset) {
        spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
        return 0;
    }
    mapped_size = (size + offset + VMM_PAGE_SIZE - 1) &
                  ~(u64)(VMM_PAGE_SIZE - 1);
    if (!mapped_size || ioremap_next > IOREMAP_LIMIT - mapped_size) {
        spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
        return 0;
    }
    virtual_start = ioremap_next;
    ioremap_next += mapped_size;

    for (u64 page = 0; page < mapped_size; page += VMM_PAGE_SIZE) {
        if (!vmm_map_kernel_one(kernel_pml4,
                     (void *)(uintptr_t)(virtual_start + page),
                     start + page,
                     PTE_READWRITE | PTE_WRITETHROUGH |
                     PTE_CACHEDISABLE | PTE_NX)) {
            spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
            if (changed)
                vmm_publish_kernel_mappings();
            return 0;
        }
        changed = true;
    }
    void *result = (void *)(uintptr_t)(virtual_start + offset);
    spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
    vmm_publish_kernel_mappings();
    return result;
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
    if (!active_pml4)
    {
        return 0;
    }

    u64 vaddr = (u64)virtual_addr;

    u64 pml4_idx = (vaddr >> 39) & 0x1FF;
    u64 pdpt_idx = (vaddr >> 30) & 0x1FF;
    u64 pd_idx   = (vaddr >> 21) & 0x1FF;
    u64 pt_idx   = (vaddr >> 12) & 0x1FF;

    u64 offset = vaddr & 0xFFF;

    if (!(active_pml4->entries[pml4_idx] & PTE_PRESENT)) {
        return 0;
    }
    
    page_table_t *pdpt = vmm_table_from_phys(active_pml4->entries[pml4_idx]);

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
    u64 saved_flags = spin_lock_irqsave(&vmm_kernel_lock);
    kernel_pml4 = vmm_table_from_phys(pml4_phys);
    active_pml4 = kernel_pml4;
    spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
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
    bool changed = false;
    u64 saved_flags = spin_lock_irqsave(&vmm_kernel_lock);
    if (!kernel_pml4 || start >= PHYS_MAP_LIMIT ||
        page_count > (PHYS_MAP_LIMIT - start) / VMM_PAGE_SIZE) {
        spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
        return false;
    }
    for (u64 i = 0; i < page_count; i++) {
        phys_addr_t phys = start + i * VMM_PAGE_SIZE;
        if (!vmm_map_kernel_one(kernel_pml4,
                         (void *)(uintptr_t)(PHYS_MAP_BASE + phys), phys,
                         PTE_READWRITE | PTE_NX)) {
            spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
            if (changed)
                vmm_publish_kernel_mappings();
            return false;
        }
        changed = true;
    }
    if (kernel_pml4->entries[(PHYS_MAP_BASE >> 39) & 0x1ff] & PTE_USER) {
        spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
        return false;
    }
    spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
    if (changed)
        vmm_publish_kernel_mappings();
    return true;
}

void vmm_enable_direct_map(void)
{
    phys_addr_t kernel_phys;
    phys_addr_t current_phys;
    u64 metadata_flags = spin_lock_irqsave(&vmm_metadata_lock);

    u64 saved_flags = spin_lock_irqsave(&vmm_kernel_lock);
    if (phys_map_is_ready()) {
        spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
        spin_unlock_irqrestore(&vmm_metadata_lock, metadata_flags);
        return;
    }
    kernel_phys = vmm_table_phys(kernel_pml4);
    current_phys = vmm_table_phys(active_pml4);
    phys_map_activate();
    kernel_pml4 = (page_table_t *)phys_to_virt(kernel_phys);
    active_pml4 = (page_table_t *)phys_to_virt(current_phys);
    for (u32 i = 0; i < VMM_MAX_PROCESS_METADATA; i++) {
        if (address_space_metadata[i].root) {
            address_space_metadata[i].root = (page_table_t *)phys_to_virt(
                vmm_table_phys(address_space_metadata[i].root));
        }
    }
    spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
    spin_unlock_irqrestore(&vmm_metadata_lock, metadata_flags);
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
    return active_pml4;
}

u64 vmm_address_space_user_memory_bytes(const page_table_t *pml4)
{
    vmm_address_space_metadata_t *metadata;
    u64 pages = 0;
    u64 flags;

    if (!pml4 || pml4 == kernel_pml4)
        return 0;
    flags = spin_lock_irqsave(&vmm_metadata_lock);
    metadata = vmm_metadata_find(pml4);
    if (metadata) {
        for (vmm_owned_frame_t *node = metadata->leaves; node;
             node = node->next) {
            if (pages != ~(u64)0)
                pages++;
        }
    }
    spin_unlock_irqrestore(&vmm_metadata_lock, flags);
    return pages > ~(u64)0 / VMM_PAGE_SIZE
        ? ~(u64)0 : pages * VMM_PAGE_SIZE;
}

page_table_t *vmm_create_address_space(void)
{
    page_table_t *pml4;
    vmm_address_space_metadata_t *metadata;
    u64 saved_flags = spin_lock_irqsave(&vmm_metadata_lock);

    if (!kernel_pml4) {
        spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
        return 0;
    }
    /* Fresh lower half; fixed high-half branches are physical references to
     * the master kernel hierarchy.  No lower table can alias kernel state. */
    pml4 = vmm_alloc_table();
    if (!pml4) {
        spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
        return 0;
    }
    u64 kernel_flags = spin_lock_irqsave(&vmm_kernel_lock);
    for (u32 i = 256; i < 512; i++) {
        if (vmm_kernel_shared_pml4_index(i)) {
            u64 entry = kernel_pml4->entries[i];
            if (entry & PTE_USER) {
                spin_unlock_irqrestore(&vmm_kernel_lock, kernel_flags);
                pmm_free_frame(vmm_table_phys(pml4));
                spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
                return 0;
            }
            pml4->entries[i] = entry;
        }
    }
    spin_unlock_irqrestore(&vmm_kernel_lock, kernel_flags);
    metadata = vmm_metadata_create(pml4);
    if (!metadata) {
        pmm_free_frame(vmm_table_phys(pml4));
        spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
        return 0;
    }
    if (!vmm_address_space_validate_locked(pml4)) {
        metadata->root = 0;
        pmm_free_frame(vmm_table_phys(pml4));
        spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
        return 0;
    }
    spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
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

static bool vmm_address_space_validate_unlocked(const page_table_t *pml4)
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
            ((kernel_pml4->entries[i] & PTE_PRESENT) &&
             !(bootstrap_mapping_active && i == 0U))) {
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

static bool vmm_address_space_validate_locked(const page_table_t *pml4)
{
    u64 saved_flags = spin_lock_irqsave(&vmm_kernel_lock);
    bool result = vmm_address_space_validate_unlocked(pml4);
    spin_unlock_irqrestore(&vmm_kernel_lock, saved_flags);
    return result;
}

bool vmm_address_space_validate(const page_table_t *pml4)
{
    u64 saved_flags = spin_lock_irqsave(&vmm_metadata_lock);
    bool result = vmm_address_space_validate_locked(pml4);
    spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
    return result;
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
    vmm_address_space_metadata_t *operation_metadata;
    vmm_owned_frame_t *node;
    vmm_owned_frame_t *leaves;
    vmm_owned_frame_t *tables;
    phys_addr_t root_physical;
    u32 wait_ticks = 0;

    if (!pml4 || pml4 == kernel_pml4)
        return;

    {
        u64 flags = spin_lock_irqsave(&vmm_metadata_lock);
        metadata = vmm_metadata_find(pml4);
        if (!metadata) {
            spin_unlock_irqrestore(&vmm_metadata_lock, flags);
            return;
        }
        operation_metadata = metadata;
        spin_unlock_irqrestore(&vmm_metadata_lock, flags);
    }
    if (!mutex_lock(&operation_metadata->operation_lock))
        return;

    for (;;) {
        u64 saved_flags = spin_lock_irqsave(&vmm_metadata_lock);
        u32 current = cpu_current_index();

        metadata = vmm_metadata_find(pml4);
        if (!metadata) {
            spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
            mutex_unlock(&operation_metadata->operation_lock);
            return;
        }
        metadata->destroy_pending = true;
        if ((current < CPU_MAX_COUNT) &&
            (metadata->active_cpu_mask[current / 64U] &
             (1ULL << (current % 64U)))) {
            spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
            panic("vmm: attempted to destroy the active address space");
        }
        if (!vmm_cpu_mask_empty(metadata->active_cpu_mask)) {
            spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
            if (wait_ticks++ >= 5000U || !scheduler_sleep(1))
                panic("vmm: address-space teardown did not quiesce");
            continue;
        }

        /* Detach ownership under the metadata lock, then release the physical
         * frames and list nodes without holding it.  In particular this
         * prevents metadata -> PMM -> heap from inverting with heap growth's
         * heap -> VMM -> PMM path. */
        leaves = metadata->leaves;
        tables = metadata->tables;
        root_physical = vmm_table_phys(pml4);
        metadata->leaves = 0;
        metadata->tables = 0;
        metadata->root = 0;
        spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
        break;
    }

    mutex_unlock(&operation_metadata->operation_lock);

    pmm_free_frame(root_physical);

    while ((node = leaves) != 0) {
        leaves = node->next;
        pmm_free_frame(node->physical);
        kfree(node);
    }
    while ((node = tables) != 0) {
        tables = node->next;
        u64 kernel_flags = spin_lock_irqsave(&vmm_kernel_lock);
        bool shared = vmm_kernel_reaches_table(node->physical);
        spin_unlock_irqrestore(&vmm_kernel_lock, kernel_flags);
        if (shared) {
            panic("process owns shared kernel page table");
        }
        pmm_free_frame(node->physical);
        kfree(node);
    }

    {
        u64 flags = spin_lock_irqsave(&vmm_metadata_lock);
        operation_metadata->destroy_pending = false;
        operation_metadata->shootdown_pending = false;
        spin_unlock_irqrestore(&vmm_metadata_lock, flags);
    }
}

bool vmm_switch_address_space(page_table_t *pml4)
{
    cpu_local_t *cpu = cpu_current();
    page_table_t *old_pml4;
    vmm_address_space_metadata_t *metadata;
    u32 index;
    u64 saved_flags;

    if (!cpu || !pml4)
        return false;
    old_pml4 = active_pml4;
    if (pml4 == old_pml4)
        return true;
    index = cpu->index;
    if (index >= CPU_MAX_COUNT)
        return false;

    saved_flags = spin_lock_irqsave(&vmm_metadata_lock);
    metadata = pml4 == kernel_pml4 ? 0 : vmm_metadata_find(pml4);
    if (pml4 != kernel_pml4 &&
        (!metadata || metadata->destroy_pending)) {
        spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
        return false;
    }
    if (metadata) {
        metadata->active_cpu_mask[index / 64U] |= 1ULL << (index % 64U);
    }

    /* Keep interrupts masked through the CR3 write and bookkeeping so an
     * interrupt handler can never observe a CPU-local CR3 pointer that does
     * not describe the hardware CR3 currently in use. */
    __asm__ volatile("mov %0, %%cr3" ::
                     "r"(vmm_table_phys(pml4)) : "memory");
    cpu->current_pml4 = pml4;
    if (old_pml4 && old_pml4 != kernel_pml4) {
        vmm_address_space_metadata_t *old = vmm_metadata_find(old_pml4);
        if (old)
            old->active_cpu_mask[index / 64U] &=
                ~(1ULL << (index % 64U));
    }
    spin_unlock_irqrestore(&vmm_metadata_lock, saved_flags);
    return true;
}

bool vmm_user_range_valid(const void *address, usize length)
{
    uintptr_t start = (uintptr_t)address;
    uintptr_t end;
    uintptr_t page;

    if (!active_pml4 || (!address && length) ||
        length > ~(uintptr_t)0 - start) return false;
    end = start + length;
    if (length == 0) return true;

    for (page = start & ~(uintptr_t)0xfff; page < end; ) {
        u64 pml4e = active_pml4->entries[(page >> 39) & 0x1ff];
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
