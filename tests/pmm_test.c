/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <memory_types.h>
#include <pmm.h>
#include <spinlock.h>

#define TEST_PHYSICAL_BYTES (5U * 1024U * 1024U)
static u8 physical_memory[TEST_PHYSICAL_BYTES];

void spinlock_init(spinlock_t *lock)
{
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELAXED);
}
u64 spin_lock_irqsave(spinlock_t *lock)
{
    while (__atomic_exchange_n(&lock->value, 1U, __ATOMIC_ACQUIRE) != 0U)
        __asm__ volatile("pause");
    return 0;
}
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    (void)flags;
    assert(__atomic_load_n(&lock->value, __ATOMIC_RELAXED) == 1U);
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
}
bool phys_map_is_ready(void) { return true; }
bool phys_map_contains(phys_addr_t physical) { return physical < TEST_PHYSICAL_BYTES; }
void *phys_to_virt(phys_addr_t physical)
{
    return phys_map_contains(physical) ? physical_memory + physical : NULL;
}
phys_addr_t virt_to_phys(const void *virtual_address)
{
    uintptr_t address = (uintptr_t)virtual_address;
    uintptr_t base = (uintptr_t)physical_memory;
    return address >= base && address - base < sizeof(physical_memory)
        ? address - base : 0;
}
void kprint(const char *format, ...)
{
    (void)format;
}

#include "../kernel/src/pmm.c"

typedef struct {
    pmm_owned_frame_t handle;
    bool result;
} release_attempt_t;

static void *release_owned_thread(void *argument)
{
    release_attempt_t *attempt = argument;
    attempt->result = pmm_owned_frame_release(&attempt->handle);
    return NULL;
}

int main(void)
{
    enum { LOW_RESERVED_FRAMES = 1024, LAST_MANAGED_FRAME = 1032 };
    MANGROVE_MEMORY_DESCRIPTOR map[] = {
        {.Type = EFI_RESERVED_MEMORY_TYPE,
         .PhysicalStart = 0,
         .NumberOfPages = LOW_RESERVED_FRAMES},
        {.Type = EFI_BOOT_SERVICES_DATA,
         .PhysicalStart = LOW_RESERVED_FRAMES * PAGE_SIZE,
         .NumberOfPages = 2},
        {.Type = EFI_CONVENTIONAL_MEMORY,
         .PhysicalStart = (LOW_RESERVED_FRAMES + 2U) * PAGE_SIZE,
         .NumberOfPages = 7},
        /* A large reserved device range must not inflate the managed bitmap. */
        {.Type = EFI_MEMORY_MAPPED_IO,
         .PhysicalStart = 0x10000000000ULL,
         .NumberOfPages = 1},
    };
    BOOT_INFO boot_info = {
        .MemoryMap = (u8 *)map,
        .MemoryMapSize = sizeof(map),
        .DescriptorSize = sizeof(map[0]),
    };
    phys_addr_t allocated[8];
    pmm_owned_frame_t owned = {0};
    pmm_owned_frame_t stale_owned = {0};
    pmm_owned_frame_t forged_owned = {0};
    pmm_owned_frame_t concurrent_owner = {0};

    memset(physical_memory, 0xa5, sizeof(physical_memory));
    assert(pmm_init(&boot_info));
    assert(total_frames == LAST_MANAGED_FRAME + 1U);
    assert(bitmap_size == (total_frames / 8U + 1U));
    assert(bitmap_ready);
    assert(pmm_get_boot_services_memory() == 2U * PAGE_SIZE);
    assert(pmm_get_free_memory() == 8U * PAGE_SIZE);
    for (u64 frame = total_frames; frame < bitmap_size * 8U; frame++)
        assert(bitmap_test(frame));

    for (u32 index = 0; index < sizeof(allocated) / sizeof(allocated[0]); index++) {
        allocated[index] = pmm_alloc_frame();
        assert(allocated[index] == (LOW_RESERVED_FRAMES + 1U + index) * PAGE_SIZE);
        const u8 *bytes = (const u8 *)phys_to_virt(allocated[index]);
        for (u32 byte = 0; byte < PAGE_SIZE; byte++)
            assert(bytes[byte] == 0);
    }
    assert(allocated[7] == LAST_MANAGED_FRAME * PAGE_SIZE);
    assert(pmm_alloc_frame() == 0);
    pmm_owned_frame_t exhausted_owner = {0};
    assert(!pmm_owned_frame_acquire(&exhausted_owner));
    assert(pmm_get_free_memory() == 0);
    assert(!pmm_reserve_range(allocated[0], 1));
    assert(pmm_get_free_memory() == 0);

    for (u32 index = 0; index < sizeof(allocated) / sizeof(allocated[0]); index++)
        pmm_free_frame(allocated[index]);
    assert(pmm_get_free_memory() == 8U * PAGE_SIZE);

    u64 free_before = pmm_get_free_memory();
    u64 used_before = pmm_get_used_memory();
    pmm_free_frame(allocated[0]);                 /* already free */
    pmm_free_frame(allocated[1] + 1U);             /* unaligned */
    pmm_free_frame(0);                             /* reserved frame zero */
    pmm_free_frame(PAGE_SIZE);                      /* reserved low memory */
    pmm_free_frame(LOW_RESERVED_FRAMES * PAGE_SIZE); /* bitmap storage */
    pmm_free_frame((total_frames + 1U) * PAGE_SIZE); /* out of range */
    assert(pmm_get_free_memory() == free_before);
    assert(pmm_get_used_memory() == used_before);

    /* Generation-owned pages cannot escape through the raw free API, and a
     * live leased mapping pins the frame until VMM reports shootdown completion. */
    assert(pmm_owned_frame_acquire(&owned));
    assert(owned.physical && owned.generation &&
           owned.slot < PMM_OWNED_FRAME_CAPACITY);
    stale_owned = owned;
    forged_owned = owned;
    forged_owned.generation++;
    free_before = pmm_get_free_memory();
    pmm_free_frame(owned.physical);
    assert(pmm_get_free_memory() == free_before);
    assert(!pmm_reserve_range(owned.physical, 1U));
    assert(!pmm_owned_frame_release(&forged_owned));
    assert(stale_owned.physical == owned.physical);
    assert(pmm_owned_frame_mapping_begin(&owned));
    assert(!pmm_owned_frame_release(&owned));
    assert(owned.physical == stale_owned.physical);
    assert(!pmm_owned_frame_mapping_begin(&owned)); /* no alias mapping */
    assert(!pmm_owned_frame_mapping_begin_physical(
        owned.physical, false)); /* only leased kernel mappings */
    assert(pmm_owned_frame_mapping_end(owned.physical));
    assert(!pmm_owned_frame_mapping_end(owned.physical)); /* duplicate end */
    assert(pmm_owned_frame_release(&owned));
    assert(!owned.physical && !owned.generation && !owned.slot);
    assert(!pmm_owned_frame_release(&stale_owned)); /* stale copied handle */
    phys_addr_t reused = pmm_alloc_frame();
    assert(reused == stale_owned.physical);
    assert(!pmm_owned_frame_release(&stale_owned));
    pmm_free_frame(reused);

    /* Reserved boot memory and ordinary allocations have no owned token. */
    assert(pmm_owned_frame_mapping_begin_physical(0, true));
    assert(pmm_owned_frame_mapping_end(0));
    assert(pmm_owned_frame_mapping_begin_physical(PAGE_SIZE, true));
    pmm_owned_frame_t forged = {
        .physical = allocated[2], .generation = 1U, .slot = 0U
    };
    assert(!pmm_owned_frame_release(&forged));
    forged.physical = PAGE_SIZE; /* reserved firmware memory */
    assert(!pmm_owned_frame_release(&forged));

    /* Two copied handles raced on different host threads must release one
     * frame exactly once; the generation record serializes ownership. */
    release_attempt_t attempts[2] = {
        {.handle = {0}, .result = false},
        {.handle = {0}, .result = false}
    };
    pthread_t releasers[2];
    free_before = pmm_get_free_memory();
    assert(pmm_owned_frame_acquire(&concurrent_owner));
    phys_addr_t concurrent_physical = concurrent_owner.physical;
    attempts[0].handle = concurrent_owner;
    attempts[1].handle = concurrent_owner;
    assert(pthread_create(&releasers[0], NULL, release_owned_thread,
                          &attempts[0]) == 0);
    assert(pthread_create(&releasers[1], NULL, release_owned_thread,
                          &attempts[1]) == 0);
    assert(pthread_join(releasers[0], NULL) == 0);
    assert(pthread_join(releasers[1], NULL) == 0);
    assert(attempts[0].result != attempts[1].result);
    assert(pmm_get_free_memory() == free_before);
    phys_addr_t concurrently_reused = pmm_alloc_frame();
    assert(concurrently_reused == concurrent_physical);
    pmm_free_frame(concurrently_reused);

    assert(pmm_reserve_range(LAST_MANAGED_FRAME * PAGE_SIZE, 1));
    assert(pmm_get_free_memory() == 7U * PAGE_SIZE);
    assert(!pmm_reserve_range((total_frames - 1U) * PAGE_SIZE, 2));

    boot_info.DescriptorSize = sizeof(map[0]) - 1U;
    assert(!pmm_init(&boot_info));
    puts("PMM boundary and accounting tests passed");
    return 0;
}
