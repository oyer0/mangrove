/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <pmm.h>
#include <bootinfo.h>
#include <memory_types.h>
#include <kprint.h>
#include <spinlock.h>
#include <limits.h>

static u8   *bitmap = 0;
static u8   *allocation_bitmap = 0;
static phys_addr_t bitmap_phys = 0;
static u64   bitmap_size = 0;
static u64   total_frames = 0;
static u64   free_frames = 0;
static u64   used_ram_frames = 0;
static u64  boot_services_frames = 0;
static spinlock_t pmm_lock;
static phys_addr_t smp_trampoline_candidate;
static bool bitmap_ready;

typedef struct {
    phys_addr_t physical;
    /* Low bit records a live VMM mapping; upper bits are the generation. */
    u64 generation_state;
} pmm_owned_frame_record_t;

static pmm_owned_frame_record_t pmm_owned_frames[PMM_OWNED_FRAME_CAPACITY];
static u64 pmm_owned_frame_next_generation = 1U;

static inline void bitmap_set(u64 frame) {
    bitmap[frame / 8] |= (u8)(1U << (frame % 8));
}

static inline void bitmap_clear(u64 frame) {
    bitmap[frame / 8] &= (u8)~(1U << (frame % 8));
}

static inline bool bitmap_test(u64 frame) {
    return (bitmap[frame / 8] & (u8)(1U << (frame % 8))) != 0;
}

static inline void allocation_bitmap_set(u64 frame)
{
    allocation_bitmap[frame / 8] |= (u8)(1U << (frame % 8));
}

static inline void allocation_bitmap_clear(u64 frame)
{
    allocation_bitmap[frame / 8] &= (u8)~(1U << (frame % 8));
}

static inline bool allocation_bitmap_test(u64 frame)
{
    return (allocation_bitmap[frame / 8] &
            (u8)(1U << (frame % 8))) != 0;
}

static u32 pmm_owned_frame_find_physical_locked(phys_addr_t physical)
{
    for (u32 slot = 0; slot < PMM_OWNED_FRAME_CAPACITY; slot++)
        if (pmm_owned_frames[slot].physical == physical && physical)
            return slot;
    return PMM_OWNED_FRAME_CAPACITY;
}

static bool pmm_owned_frame_record_matches_locked(
    const pmm_owned_frame_t *frame)
{
    const pmm_owned_frame_record_t *record;

    if (!frame || !frame->physical || !frame->generation ||
        frame->slot >= PMM_OWNED_FRAME_CAPACITY)
        return false;
    record = &pmm_owned_frames[frame->slot];
    return record->physical == frame->physical &&
           (record->generation_state >> 1) == frame->generation;
}

static inline bool pmm_is_usable_memory(u32 type)
{
    return type == EFI_CONVENTIONAL_MEMORY ||
           type == EFI_BOOT_SERVICES_CODE ||
           type == EFI_BOOT_SERVICES_DATA ;
}

static MANGROVE_MEMORY_DESCRIPTOR *pmm_descriptor_at(
    const BOOT_INFO *boot_info, u64 index)
{
    return (MANGROVE_MEMORY_DESCRIPTOR *)(boot_info->MemoryMap +
        index * boot_info->DescriptorSize);
}

bool pmm_init(BOOT_INFO *boot_info) {
    u64 mmap_entries;

    spinlock_init(&pmm_lock);

    u64 highest_address = 0;
    bitmap = 0;
    allocation_bitmap = 0;
    bitmap_phys = 0;
    bitmap_size = 0;
    total_frames = 0;
    free_frames = 0;
    used_ram_frames = 0;
    boot_services_frames = 0;
    smp_trampoline_candidate = 0;
    bitmap_ready = false;
    for (u32 slot = 0; slot < PMM_OWNED_FRAME_CAPACITY; slot++)
        pmm_owned_frames[slot] = (pmm_owned_frame_record_t){0};

    if (!boot_info || !boot_info->MemoryMap || !boot_info->DescriptorSize ||
        boot_info->DescriptorSize < sizeof(MANGROVE_MEMORY_DESCRIPTOR) ||
        boot_info->MemoryMapSize % boot_info->DescriptorSize ||
        !phys_map_is_ready())
        return false;
    mmap_entries = boot_info->MemoryMapSize / boot_info->DescriptorSize;
    if (!mmap_entries)
        return false;

    /* Keep the startup page in ordinary conventional RAM below the VGA
     * aperture.  The candidate is taken from the firmware map rather than
     * assuming that a traditional low-memory layout exists. */
    for (u64 i = 0; i < mmap_entries; i++) {
        MANGROVE_MEMORY_DESCRIPTOR *desc = pmm_descriptor_at(boot_info, i);
        u64 bytes;
        u64 end;
        u64 candidate_end;
        phys_addr_t candidate;

        if (!pmm_is_usable_memory(desc->Type) ||
            desc->NumberOfPages > ~(u64)0 / PAGE_SIZE)
            continue;
        bytes = desc->NumberOfPages * PAGE_SIZE;
        if (desc->PhysicalStart > ~(u64)0 - bytes)
            continue;
        end = desc->PhysicalStart + bytes;
        candidate_end = end < 0x000A0000ULL ? end : 0x000A0000ULL;
        if (candidate_end <= desc->PhysicalStart || candidate_end < PAGE_SIZE)
            continue;
        candidate = (phys_addr_t)((candidate_end - 1) &
                                  ~(u64)(PAGE_SIZE - 1));
        if (candidate < desc->PhysicalStart || candidate < PAGE_SIZE)
            continue;
        if (candidate > smp_trampoline_candidate)
            smp_trampoline_candidate = candidate;
    }

    for (u64 i = 0; i < mmap_entries; i++) {
        MANGROVE_MEMORY_DESCRIPTOR *desc = pmm_descriptor_at(boot_info, i);
        u64 bytes;
        u64 top;

        /* The allocator indexes usable RAM only. Reserved device ranges above
         * the direct-map limit must not inflate its bitmap or scan range. */
        if (!pmm_is_usable_memory(desc->Type))
            continue;
        if ((desc->PhysicalStart & (PAGE_SIZE - 1U)) ||
            desc->NumberOfPages > ULLONG_MAX / PAGE_SIZE)
            return false;
        bytes = desc->NumberOfPages * PAGE_SIZE;
        if (desc->PhysicalStart > ULLONG_MAX - bytes)
            return false;
        top = desc->PhysicalStart + bytes;
        if (top > PHYS_MAP_LIMIT)
            return false;
        if (top > highest_address)
            highest_address = top;
    }

    total_frames = highest_address / PAGE_SIZE;
    if (!total_frames)
        return false;
    bitmap_size = total_frames / 8U + (total_frames % 8U != 0U);
    if (bitmap_size > ULLONG_MAX / 2U)
        return false;
    u64 bitmap_storage_size = bitmap_size * 2U;
    u64 bitmap_frames = bitmap_storage_size / PAGE_SIZE +
        (bitmap_storage_size % PAGE_SIZE != 0U);
    if (!bitmap_frames)
        return false;

    for (u64 i = 0; i < mmap_entries; i++) {
        MANGROVE_MEMORY_DESCRIPTOR *desc = pmm_descriptor_at(boot_info, i);
        if (pmm_is_usable_memory(desc->Type)){
            if (desc->NumberOfPages >= bitmap_frames){
                bitmap_phys = desc->PhysicalStart;
                bitmap = (u8 *)phys_to_virt(bitmap_phys);
                if (!bitmap)
                    return false;
                allocation_bitmap = bitmap + bitmap_size;
                bitmap_ready = true;
                break;
            }
        }
    }
    if (!bitmap_ready)
        return false;

    for (u64 i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0xFF;
        allocation_bitmap[i] = 0;
    }

    for (u64 i = 0; i < mmap_entries; i++) {
        MANGROVE_MEMORY_DESCRIPTOR *desc = pmm_descriptor_at(boot_info, i);
        
        if (pmm_is_usable_memory(desc->Type)){
            u64 start_frame = desc->PhysicalStart / PAGE_SIZE;
        
            for (u64 f = 0; f < desc->NumberOfPages; f++){
                bitmap_clear(start_frame + f);
                free_frames++;
            }
            if (desc->Type == EFI_BOOT_SERVICES_CODE ||
                desc->Type == EFI_BOOT_SERVICES_DATA)
                boot_services_frames += desc->NumberOfPages;
        }
    }

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
    return true;
}
void pmm_enable_direct_map(void)
{
    u64 flags = spin_lock_irqsave(&pmm_lock);
    if (bitmap_phys) bitmap = (u8 *)phys_to_virt(bitmap_phys);
    spin_unlock_irqrestore(&pmm_lock, flags);
}

phys_addr_t pmm_alloc_frame(void) {
    u64 flags = spin_lock_irqsave(&pmm_lock);
    phys_addr_t result = 0;

    if (!bitmap_ready) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return 0;
    }
    for (u64 i = 0; i < total_frames; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            allocation_bitmap_set(i);
            free_frames--;
            used_ram_frames++;

            phys_addr_t frame_addr = i * PAGE_SIZE;
            u64 *ptr = (u64 *)phys_to_virt(frame_addr);
            for (int j = 0; j < 512; j++) {
                ptr[j] = 0;
            }
            result = frame_addr;
            break;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return result;
}

void pmm_free_frame(phys_addr_t frame) {
    u64 flags = spin_lock_irqsave(&pmm_lock);
    u64 frame_idx;
    if (!bitmap_ready || !frame || (frame & (PAGE_SIZE - 1U)) ||
        frame / PAGE_SIZE >= total_frames ||
        !allocation_bitmap_test(frame / PAGE_SIZE)) {
        kprint("[PMM INVALID FREE] frame %p is outside the managed frame set\n",
               (void *)(uintptr_t)frame);
        spin_unlock_irqrestore(&pmm_lock, flags);
        return;
    }
    frame_idx = frame / PAGE_SIZE;
    if (bitmap_test(frame_idx)) {
        allocation_bitmap_clear(frame_idx);
        bitmap_clear(frame_idx);
        free_frames++;
        used_ram_frames--;
    } else {
        kprint("[PMM DOUBLE FREE BUG!] frame %p was ALREADY FREE!\n",
               (void *)(uintptr_t)frame);
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
}

bool pmm_owned_frame_acquire(pmm_owned_frame_t *out_frame)
{
    u64 flags;
    u32 owner_slot = PMM_OWNED_FRAME_CAPACITY;
    phys_addr_t physical = 0;
    u64 generation;

    if (!out_frame || out_frame->physical || out_frame->generation ||
        out_frame->slot)
        return false;
    flags = spin_lock_irqsave(&pmm_lock);
    if (!bitmap_ready || !pmm_owned_frame_next_generation ||
        pmm_owned_frame_next_generation > (ULLONG_MAX >> 1)) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return false;
    }
    for (u32 slot = 0; slot < PMM_OWNED_FRAME_CAPACITY; slot++) {
        if (!pmm_owned_frames[slot].physical) {
            owner_slot = slot;
            break;
        }
    }
    if (owner_slot == PMM_OWNED_FRAME_CAPACITY) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return false;
    }
    for (u64 index = 0; index < total_frames; index++) {
        if (bitmap_test(index))
            continue;
        bitmap_set(index);
        /* Owned frames deliberately do not enter the raw pmm_free_frame()
         * path. Only the generation-checked release below may return them. */
        allocation_bitmap_clear(index);
        free_frames--;
        used_ram_frames++;
        physical = index * PAGE_SIZE;
        u64 *words = (u64 *)phys_to_virt(physical);
        if (!words) {
            bitmap_clear(index);
            free_frames++;
            used_ram_frames--;
            physical = 0;
            break;
        }
        for (u32 word = 0; word < PAGE_SIZE / sizeof(u64); word++)
            words[word] = 0;
        break;
    }
    if (!physical) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return false;
    }

    generation = pmm_owned_frame_next_generation++;
    pmm_owned_frames[owner_slot] = (pmm_owned_frame_record_t){
        .physical = physical,
        .generation_state = generation << 1
    };
    *out_frame = (pmm_owned_frame_t){
        .physical = physical,
        .generation = generation,
        .slot = owner_slot
    };
    spin_unlock_irqrestore(&pmm_lock, flags);
    return true;
}

bool pmm_owned_frame_release(pmm_owned_frame_t *frame)
{
    u64 flags;
    pmm_owned_frame_record_t *record;
    u64 index;

    if (!frame || !frame->physical || !frame->generation ||
        frame->slot >= PMM_OWNED_FRAME_CAPACITY)
        return false;
    flags = spin_lock_irqsave(&pmm_lock);
    if (!bitmap_ready || !pmm_owned_frame_record_matches_locked(frame)) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return false;
    }
    record = &pmm_owned_frames[frame->slot];
    index = frame->physical / PAGE_SIZE;
    if ((record->generation_state & 1U) || index >= total_frames ||
        !bitmap_test(index) || allocation_bitmap_test(index)) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return false;
    }
    bitmap_clear(index);
    free_frames++;
    used_ram_frames--;
    *record = (pmm_owned_frame_record_t){0};
    *frame = (pmm_owned_frame_t){0};
    spin_unlock_irqrestore(&pmm_lock, flags);
    return true;
}

bool pmm_owned_frame_mapping_begin(const pmm_owned_frame_t *frame)
{
    u64 flags;
    pmm_owned_frame_record_t *record;
    bool result = false;

    if (!frame || !frame->physical || (frame->physical & (PAGE_SIZE - 1U)) ||
        !frame->generation || frame->slot >= PMM_OWNED_FRAME_CAPACITY)
        return false;
    flags = spin_lock_irqsave(&pmm_lock);
    if (bitmap_ready && pmm_owned_frame_record_matches_locked(frame)) {
        record = &pmm_owned_frames[frame->slot];
        if (!(record->generation_state & 1U) &&
            bitmap_test(frame->physical / PAGE_SIZE) &&
            !allocation_bitmap_test(frame->physical / PAGE_SIZE)) {
            record->generation_state |= 1U;
            result = true;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return result;
}

bool pmm_owned_frame_mapping_begin_physical(phys_addr_t frame,
                                             bool leased_kernel_range)
{
    u64 flags;
    u32 slot = PMM_OWNED_FRAME_CAPACITY;
    bool result = true;

    if (frame & (PAGE_SIZE - 1U))
        return false;
    flags = spin_lock_irqsave(&pmm_lock);
    if (!bitmap_ready || frame / PAGE_SIZE >= total_frames) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return true;
    }
    /* Most ordinary PMM allocations take this constant-time path. Reserved
     * pages and leased-owned pages are the only frames that need a registry
     * lookup. */
    if (bitmap_test(frame / PAGE_SIZE) &&
        !allocation_bitmap_test(frame / PAGE_SIZE))
        slot = pmm_owned_frame_find_physical_locked(frame);
    if (slot < PMM_OWNED_FRAME_CAPACITY) {
        pmm_owned_frame_record_t *record = &pmm_owned_frames[slot];
        if (!leased_kernel_range || (record->generation_state & 1U)) {
            result = false;
        } else {
            record->generation_state |= 1U;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return result;
}

bool pmm_owned_frame_mapping_end(phys_addr_t frame)
{
    u64 flags;
    u32 slot = PMM_OWNED_FRAME_CAPACITY;
    bool result = true;

    if (frame & (PAGE_SIZE - 1U))
        return false;
    flags = spin_lock_irqsave(&pmm_lock);
    if (bitmap_ready && frame / PAGE_SIZE < total_frames &&
        bitmap_test(frame / PAGE_SIZE) &&
        !allocation_bitmap_test(frame / PAGE_SIZE))
        slot = pmm_owned_frame_find_physical_locked(frame);
    if (slot < PMM_OWNED_FRAME_CAPACITY) {
        pmm_owned_frame_record_t *record = &pmm_owned_frames[slot];
        if (!(record->generation_state & 1U)) {
            result = false;
        } else {
            record->generation_state &= ~1ULL;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return result;
}

u64 pmm_get_free_memory(void) {
    u64 flags = spin_lock_irqsave(&pmm_lock);
    u64 result = free_frames * PAGE_SIZE;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return result;
}

u64 pmm_get_used_memory(void) {
    u64 flags = spin_lock_irqsave(&pmm_lock);
    u64 result = used_ram_frames * PAGE_SIZE;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return result;
}

u64 pmm_get_total_memory(void)
{
    u64 flags = spin_lock_irqsave(&pmm_lock);
    u64 result = (free_frames + used_ram_frames) * PAGE_SIZE;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return result;
}

u64 pmm_get_total_frames(void) {
    u64 flags = spin_lock_irqsave(&pmm_lock);
    u64 result = total_frames;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return result;
}

u64 pmm_get_boot_services_memory(void)
{
    u64 flags = spin_lock_irqsave(&pmm_lock);
    u64 result = boot_services_frames * PAGE_SIZE;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return result;
}

phys_addr_t pmm_smp_trampoline_candidate(void)
{
    u64 flags = spin_lock_irqsave(&pmm_lock);
    phys_addr_t result = smp_trampoline_candidate;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return result;
}

bool pmm_reserve_range(phys_addr_t start, u64 page_count)
{
    u64 flags;
    u64 first;
    u64 end;

    if (!page_count || (start & (PAGE_SIZE - 1)) ||
        page_count > ~(u64)0 / PAGE_SIZE ||
        start > ~(u64)0 - page_count * PAGE_SIZE)
        return false;

    first = start / PAGE_SIZE;
    end = first + page_count;
    flags = spin_lock_irqsave(&pmm_lock);
    if (!bitmap_ready || !bitmap || end > total_frames ||
        end > bitmap_size * 8ULL) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return false;
    }
    for (u64 frame = first; frame < end; frame++) {
        if (allocation_bitmap_test(frame) ||
            (bitmap_test(frame) &&
             pmm_owned_frame_find_physical_locked(frame * PAGE_SIZE) <
                 PMM_OWNED_FRAME_CAPACITY)) {
            spin_unlock_irqrestore(&pmm_lock, flags);
            return false;
        }
    }
    for (u64 frame = first; frame < end; frame++) {
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            free_frames--;
            used_ram_frames++;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return true;
}
