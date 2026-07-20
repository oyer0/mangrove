#ifndef PMM_H
#define PMM_H

#include <types.h>
#include <bootinfo.h>

#define PAGE_SIZE 4096

void pmm_init(BOOT_INFO *boot_info);
void *pmm_alloc_frame(void);
void pmm_free_frame(void *frame);

u64 pmm_get_free_memory(void);
u64 pmm_get_used_memory(void);
u64 pmm_get_total_frames(void);

#endif
