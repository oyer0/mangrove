/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>
#include <bootinfo.h>
#include <address_space.h>

#define PAGE_SIZE 4096

/* Explicitly owned frames can only be returned with the matching generation
 * handle. The fixed table keeps ownership metadata out of the allocator and
 * gives reclamation a bounded resource limit. */
#define PMM_OWNED_FRAME_CAPACITY 1024U
typedef struct pmm_owned_frame {
    phys_addr_t physical;
    u64 generation;
    u32 slot;
} pmm_owned_frame_t;

bool pmm_init(BOOT_INFO *boot_info);
/* PMM returns page-aligned physical frame addresses, never CPU pointers. */
phys_addr_t pmm_alloc_frame(void);
void pmm_free_frame(phys_addr_t frame);
/* Requires a zero-initialized output handle. Acquisition returns one zeroed
 * frame and fails when the bounded owned-frame table or PMM is exhausted. */
bool pmm_owned_frame_acquire(pmm_owned_frame_t *out_frame);
bool pmm_owned_frame_release(pmm_owned_frame_t *frame);
/* VMM mapping lifetime hooks. An owned frame can have one dynamic mapping,
 * and it must be within a leased kernel virtual range. mapping_end is called
 * only after the range's SMP TLB invalidation has completed. */
bool pmm_owned_frame_mapping_begin(const pmm_owned_frame_t *frame);
bool pmm_owned_frame_mapping_begin_physical(phys_addr_t frame,
                                             bool leased_kernel_range);
bool pmm_owned_frame_mapping_end(phys_addr_t frame);
/* Called after the permanent PHYS_MAP_BASE mapping has been installed. */
void pmm_enable_direct_map(void);

u64 pmm_get_free_memory(void);
u64 pmm_get_used_memory(void);
u64 pmm_get_total_memory(void);
u64 pmm_get_total_frames(void);
u64 pmm_get_boot_services_memory(void);

/* Returns a page selected from the boot map's conventional memory below the
 * VGA aperture, suitable for an AP startup trampoline. */
phys_addr_t pmm_smp_trampoline_candidate(void);
bool pmm_reserve_range(phys_addr_t start, u64 page_count);
