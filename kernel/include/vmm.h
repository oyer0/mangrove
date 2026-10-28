/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>
#include <address_space.h>
#include <pmm.h>

#define PTE_PRESENT       (1ULL << 0)
#define PTE_READWRITE     (1ULL << 1)
#define PTE_USER          (1ULL << 2)
#define PTE_WRITETHROUGH  (1ULL << 3)
#define PTE_CACHEDISABLE  (1ULL << 4)
#define PTE_ACCESSED      (1ULL << 5)
#define PTE_DIRTY         (1ULL << 6)
#define PTE_HUGE          (1ULL << 7)
/* Bit 7 selects PAT only in a 4 KiB leaf; it means PS in a PDE/PDPTE. */
#define PTE_PAT_4K        (1ULL << 7)
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

/* Keeps a userspace mapping present with its requested access until release.
 * A lease does not hold a VMM lock across the operation; unmap refuses to
 * remove any leased page, and address-space teardown waits for all leases. */
typedef struct vmm_user_range_lease {
    page_table_t *address_space;
    uintptr_t first_page;
    usize page_count;
    bool active;
} vmm_user_range_lease_t;

/* Exclusive lease on an unmapped subrange of either the dedicated kernel VMM
 * arena or an explicitly leased heap-tail range. Reserving space allocates
 * neither page tables nor physical frames. The generation rejects stale
 * copied handles after address reuse. */
typedef struct vmm_kernel_range {
    virt_addr_t base;
    u64 page_count;
    u64 generation;
} vmm_kernel_range_t;

void vmm_init(void);
/* First-fit page-aligned leases from the dedicated kernel VMM range.  The
 * output handle must be zero-initialized and release consumes it.  A lease
 * with present kernel mappings cannot be released. */
bool vmm_kernel_range_reserve(u64 page_count,
                              vmm_kernel_range_t *out_range);
/* Reserve an exact, currently unmapped range in the kernel heap region. This
 * is only for backing the heap's contiguous tail; ordinary heap mappings do
 * not acquire leases. The caller must serialize this with heap growth. */
bool vmm_kernel_heap_range_reserve(virt_addr_t base, u64 page_count,
                                  vmm_kernel_range_t *out_range);
/* Removes a bounded page-aligned subrange. first_page is relative to the
 * lease; at most 256 pages are removed per call. On SMP, callers need enabled
 * interrupts and must hold no spinlocks. Success means local and all online
 * remote CPUs have invalidated the old translations. The caller keeps
 * ownership of the physical frames and must quiesce software users first. */
bool vmm_kernel_range_unmap(vmm_kernel_range_t *range,
                            u64 first_page, u64 page_count);
/* Maps one generation-owned PMM frame into a page of the exact live lease.
 * The caller retains the frame handle; release is rejected until the page is
 * unmapped and the SMP shootdown has completed. */
bool vmm_kernel_range_map_owned(const vmm_kernel_range_t *range,
                                u64 page_index,
                                const pmm_owned_frame_t *frame,
                                u64 flags);
bool vmm_kernel_range_release(vmm_kernel_range_t *range);
/* Maps one kernel page in the master root. Addresses in the leased range
 * require a live reservation covering the complete virtual page. */
bool vmm_map(page_table_t *pml4, void *virtual_addr, phys_addr_t physical_addr, u64 flags);
/* Maps a supervisor page during single-BSP startup.  The caller must have
 * interrupts disabled; this path refuses once more than one CPU is online. */
bool vmm_map_kernel_bootstrap(page_table_t *pml4, void *virtual_addr,
                              phys_addr_t physical_addr, u64 flags);
/* Temporary low identity mapping used only while an AP leaves the SIPI
 * trampoline.  The caller removes it after AP startup completes. */
bool vmm_map_bootstrap_page(phys_addr_t physical_addr);
bool vmm_unmap_bootstrap_page(phys_addr_t physical_addr);
bool vmm_map_user_page(page_table_t *pml4, void *virtual_addr,
                       phys_addr_t physical_addr, u64 flags);
bool vmm_unmap_user_page(page_table_t *pml4, void *virtual_addr,
                         phys_addr_t *out_physical_addr);
/* Atomically unmaps a contiguous user range after confirming every mapped
 * page is owned by the address space and has no active user-memory lease.
 * Owned frames are released after the required address-space TLB flush. */
bool vmm_unmap_user_range(page_table_t *pml4, void *virtual_addr,
                          usize page_count);
/* CPU online-state transitions share the kernel TLB-shootdown gate. These
 * hooks are used by cpu_mark_online/offline to keep shootdown target sets
 * stable and flush a CPU before it joins the online set. */
bool vmm_cpu_online_transition_begin(void);
bool vmm_cpu_offline_transition_begin(void);
void vmm_cpu_transition_end(bool locked);
#if defined(SLAB_CORE_GUEST_TEST) || defined(VMM_KERNEL_RANGE_GUEST_TEST)
void vmm_kernel_range_guest_test_copy_shootdown_masks(
    u64 *ack_mask, u64 *other_cr3_mask, u32 word_count);
bool vmm_kernel_range_guest_test_run(void);
#endif
void *vmm_map_mmio(phys_addr_t physical_addr, u64 size);
/* One-shot boot constructor for the inactive permanent root. Rejects aliases
 * and retains its physical exclusion after activation. Generic MMIO stays UC. */
void *vmm_map_boot_framebuffer(const BOOT_INFO *info, bool *write_combining);
bool vmm_ioremap_contains(const void *address);
bool vmm_kernel_mapping_present(const void *address);
bool vmm_kernel_mapping_supervisor(const void *address);

void vmm_set_kernel_pml4(phys_addr_t pml4_phys);
page_table_t *vmm_get_kernel_pml4(void);
phys_addr_t vmm_get_kernel_pml4_phys(void);
page_table_t *vmm_get_current_pml4(void);
/* Bytes occupied by user-mapped physical pages in this address space. */
u64 vmm_address_space_user_memory_bytes(const page_table_t *pml4);
/* Permanently map RAM-backed physical pages at PHYS_MAP_BASE + phys. */
bool vmm_map_physical_ram(phys_addr_t start, u64 page_count);
void vmm_enable_direct_map(void);
/* Verifies one direct-map leaf and all of its supervisor-only ancestors. */
bool vmm_direct_map_valid(phys_addr_t physical_addr);
page_table_t *vmm_create_address_space(void);
void vmm_destroy_address_space(page_table_t *pml4);
bool vmm_switch_address_space(page_table_t *pml4);
bool vmm_user_range_valid(const void *address, usize length);
bool vmm_user_range_writable(const void *address, usize length);
/* Checks an address-space range under VMM metadata synchronization. These
 * helpers are for trusted reservation/ELF validation, not user pointers. */
bool vmm_user_range_unmapped(page_table_t *pml4, uintptr_t address,
                             usize length);
bool vmm_user_range_executable(page_table_t *pml4, uintptr_t address,
                               usize length);
bool vmm_user_range_lease(page_table_t *pml4, const void *address,
                          usize length, bool writable,
                          vmm_user_range_lease_t *out_lease);
void vmm_user_range_release(vmm_user_range_lease_t *lease);
phys_addr_t vmm_virtual_to_physical(void *virtual_addr);

/* Stage-4 ownership checks.  These are intentionally independent from
 * PTE_USER: that bit is an x86 permission, not a lifetime annotation. */
bool vmm_address_space_validate(const page_table_t *pml4);
bool vmm_kernel_mappings_supervisor_only(void);
