#include <pmm.h>
#include <font.h>
#include <stdbool.h>
#include <bootinfo.h>

static u8   *bitmap = 0;
static u64   bitmap_size = 0;
static u64   total_frames = 0;
static u64   free_frames = 0;
static u64   used_ram_frames = 0;

static inline void bitmap_set(u64 frame) {
    bitmap[frame / 8] |= (1 << (frame % 8));
}

static inline void bitmap_clear(u64 frame) {
    bitmap[frame / 8] &= ~(1 << (frame % 8));
}

static inline bool bitmap_test(u64 frame) {
    return (bitmap[frame / 8] & (1 << (frame % 8))) != 0;
}

void pmm_init(BOOT_INFO *boot_info) {
    MANGROVE_MEMORY_DESCRIPTOR *mmap = (MANGROVE_MEMORY_DESCRIPTOR *)boot_info->MemoryMap;
    u64 mmap_entries = boot_info->MemoryMapSize / boot_info->DescriptorSize;

    u64 highest_address = 0;
    free_frames = 0;
    used_ram_frames = 0;

    for (u64 i = 0; i < mmap_entries; i++) {
        MANGROVE_MEMORY_DESCRIPTOR *desc = (MANGROVE_MEMORY_DESCRIPTOR *)((u64)mmap + (i * boot_info->DescriptorSize));

        u64 top = desc->PhysicalStart + (desc->NumberOfPages * PAGE_SIZE);

        if (top > highest_address) {
            highest_address = top;
        }
    }

    total_frames = highest_address / PAGE_SIZE;

    bitmap_size = total_frames / 8;

    for (u64 i = 0; i < mmap_entries; i++) {
        MANGROVE_MEMORY_DESCRIPTOR *desc = (MANGROVE_MEMORY_DESCRIPTOR *)((u64)mmap + (i * boot_info->DescriptorSize));
        if (desc->Type == 7) { 
            if ((desc->NumberOfPages * PAGE_SIZE) >= bitmap_size) {
                bitmap = (u8 *)desc->PhysicalStart;
                break;
            }
        }
    }

    for (u64 i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0xFF;
    }

    for (u64 i = 0; i < mmap_entries; i++) {
        MANGROVE_MEMORY_DESCRIPTOR *desc = (MANGROVE_MEMORY_DESCRIPTOR *)((u64)mmap + (i * boot_info->DescriptorSize));
        
        if (desc->Type == 7) {
            u64 start_frame = desc->PhysicalStart / PAGE_SIZE;
            for (u64 f = 0; f < desc->NumberOfPages; f++) {
                bitmap_clear(start_frame + f);
                free_frames++;
            }
        } else if (desc->Type == 3 || desc->Type == 4) {
            used_ram_frames += desc->NumberOfPages;
        }
    }

    u64 bitmap_frames = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 bitmap_start_frame = (u64)bitmap / PAGE_SIZE;
    for (u64 f = 0; f < bitmap_frames; f++) {
        if (!bitmap_test(bitmap_start_frame + f)) {
            bitmap_set(bitmap_start_frame + f);
            free_frames--;
            used_ram_frames++;
        }
    }

    if (!bitmap_test(0)) {
        bitmap_set(0);
        free_frames--;
        used_ram_frames++;
    }
}

void *pmm_alloc_frame(void) {
    for (u64 i = 0; i < total_frames; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_frames--;
            used_ram_frames++;

            void *frame_addr = (void *)(i * PAGE_SIZE);

            u64 *ptr = (u64 *)frame_addr;
            for (int j = 0; j < 512; j++) {
                ptr[j] = 0;
            }

            return frame_addr;
        }
    }
    return 0;
}

void pmm_free_frame(void *frame) {
    u64 addr = (u64)frame;
    u64 frame_idx = addr / PAGE_SIZE;

    if (bitmap_test(frame_idx)) {
        bitmap_clear(frame_idx);
        free_frames++;
        used_ram_frames--;
    }
}

u64 pmm_get_free_memory(void) {
    return free_frames * PAGE_SIZE;
}

u64 pmm_get_used_memory(void) {
    return used_ram_frames * PAGE_SIZE;
}

u64 pmm_get_total_frames(void) {
    return total_frames;
}
