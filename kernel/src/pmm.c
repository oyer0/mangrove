#include <pmm.h>
#include <bootinfo.h>
#include <memory_types.h>
#include <kprint.h>

static u8   *bitmap = 0;
static phys_addr_t bitmap_phys = 0;
static u64   bitmap_size = 0;
static u64   total_frames = 0;
static u64   free_frames = 0;
static u64   used_ram_frames = 0;
static u64  boot_services_frames = 0;

static inline void bitmap_set(u64 frame) {
    bitmap[frame / 8] |= (1 << (frame % 8));
}

static inline void bitmap_clear(u64 frame) {
    bitmap[frame / 8] &= ~(1 << (frame % 8));
}

static inline bool bitmap_test(u64 frame) {
    return (bitmap[frame / 8] & (1 << (frame % 8))) != 0;
}

static inline bool pmm_is_usable_memory(u32 type)
{
    return type == EFI_CONVENTIONAL_MEMORY ||
           type == EFI_BOOT_SERVICES_CODE ||
           type == EFI_BOOT_SERVICES_DATA ;
}

void pmm_init(BOOT_INFO *boot_info) {
    MANGROVE_MEMORY_DESCRIPTOR *mmap = (MANGROVE_MEMORY_DESCRIPTOR *)boot_info->MemoryMap;
    u64 mmap_entries = boot_info->MemoryMapSize / boot_info->DescriptorSize;

    u64 highest_address = 0;
    free_frames = 0;
    used_ram_frames = 0;
    boot_services_frames = 0;

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
        if (pmm_is_usable_memory(desc->Type)){
            if ((desc->NumberOfPages * PAGE_SIZE) >= bitmap_size){
                bitmap_phys = desc->PhysicalStart;
                bitmap = phys_map_is_ready()
                    ? (u8 *)phys_to_virt(bitmap_phys)
                    : (u8 *)(uintptr_t)bitmap_phys;
                break;
            }
        }
    }

    for (u64 i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0xFF;
    }

    for (u64 i = 0; i < mmap_entries; i++) {
        MANGROVE_MEMORY_DESCRIPTOR *desc = (MANGROVE_MEMORY_DESCRIPTOR *)((u64)mmap + (i * boot_info->DescriptorSize));
        
        if (pmm_is_usable_memory(desc->Type)){
            u64 start_frame = desc->PhysicalStart / PAGE_SIZE;
        
            for (u64 f = 0; f < desc->NumberOfPages; f++){
                bitmap_clear(start_frame + f);
                free_frames++;
            }
        } else if (desc->Type == EFI_BOOT_SERVICES_CODE ||
                 desc->Type == EFI_BOOT_SERVICES_DATA){
            boot_services_frames += desc->NumberOfPages;
            used_ram_frames += desc->NumberOfPages;
        }
    }

    u64 bitmap_frames = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 bitmap_start_frame = bitmap_phys / PAGE_SIZE;
    for (u64 f = 0; f < bitmap_frames; f++) {
        if (!bitmap_test(bitmap_start_frame + f)) {
            bitmap_set(bitmap_start_frame + f);
            free_frames--;
            used_ram_frames++;
        }
    }

    /* Reserve low memory & kernel image region (0 to 4MB) */
    for (u64 f = 0; f < 0x400; f++) {
        if (f < total_frames && !bitmap_test(f)) {
            bitmap_set(f);
            free_frames--;
            used_ram_frames++;
        }
    }

    if (boot_info) {
        u64 bootinfo_frame = (u64)boot_info / PAGE_SIZE;
        if (bootinfo_frame < total_frames && !bitmap_test(bootinfo_frame)) {
            bitmap_set(bootinfo_frame);
            free_frames--;
            used_ram_frames++;
        }

        u64 mmap_frame_start = (u64)boot_info->MemoryMap / PAGE_SIZE;
        u64 mmap_frame_end = ((u64)boot_info->MemoryMap + boot_info->MemoryMapSize + PAGE_SIZE - 1) / PAGE_SIZE;
        for (u64 f = mmap_frame_start; f < mmap_frame_end; f++) {
            if (f < total_frames && !bitmap_test(f)) {
                bitmap_set(f);
                free_frames--;
                used_ram_frames++;
            }
        }
    }
}
void pmm_enable_direct_map(void)
{
    if (bitmap_phys) bitmap = (u8 *)phys_to_virt(bitmap_phys);
}

phys_addr_t pmm_alloc_frame(void) {
    for (u64 i = 0; i < total_frames; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_frames--;
            used_ram_frames++;

            phys_addr_t frame_addr = i * PAGE_SIZE;
            u64 *ptr = (u64 *)phys_to_virt(frame_addr);
            for (int j = 0; j < 512; j++) {
                ptr[j] = 0;
            }
            return frame_addr;
        }
    }
    return 0;
}

void pmm_free_frame(phys_addr_t frame) {
    u64 addr = frame;
    u64 frame_idx = addr / PAGE_SIZE;
    if (bitmap_test(frame_idx)) {
        bitmap_clear(frame_idx);
        free_frames++;
        used_ram_frames--;
    } else {
        kprint("[PMM DOUBLE FREE BUG!] frame %p was ALREADY FREE!\n",
               (void *)(uintptr_t)frame);
    }
}

u64 pmm_get_free_memory(void) {
    return free_frames * PAGE_SIZE;
}

u64 pmm_get_used_memory(void) {
    return used_ram_frames * PAGE_SIZE;
}

u64 pmm_get_total_memory(void)
{
    return pmm_get_free_memory() + pmm_get_used_memory();
}

u64 pmm_get_total_frames(void) {
    return total_frames;
}

u64 pmm_get_boot_services_memory(void)
{
    return boot_services_frames * PAGE_SIZE;
}
