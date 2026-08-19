#pragma once

#include <types.h>
#include <bootinfo.h>
#include <address_space.h>

#define PAGE_SIZE 4096

void pmm_init(BOOT_INFO *boot_info);
/* PMM returns page-aligned physical frame addresses, never CPU pointers. */
phys_addr_t pmm_alloc_frame(void);
void pmm_free_frame(phys_addr_t frame);
/* Called after the permanent PHYS_MAP_BASE mapping has been installed. */
void pmm_enable_direct_map(void);

u64 pmm_get_free_memory(void);
u64 pmm_get_used_memory(void);
u64 pmm_get_total_memory(void);
u64 pmm_get_total_frames(void);
u64 pmm_get_boot_services_memory(void);
