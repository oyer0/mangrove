/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory_types.h>
#include <pmm.h>
#include <spinlock.h>

#define TEST_PHYSICAL_BYTES (5U * 1024U * 1024U)
static u8 physical_memory[TEST_PHYSICAL_BYTES];

void spinlock_init(spinlock_t *lock) { lock->value = 0; }
u64 spin_lock_irqsave(spinlock_t *lock)
{
    assert(__atomic_exchange_n(&lock->value, 1U, __ATOMIC_ACQUIRE) == 0U);
    return 0;
}
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    (void)flags;
    assert(__atomic_exchange_n(&lock->value, 0U, __ATOMIC_RELEASE) == 1U);
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

    assert(pmm_reserve_range(LAST_MANAGED_FRAME * PAGE_SIZE, 1));
    assert(pmm_get_free_memory() == 7U * PAGE_SIZE);
    assert(!pmm_reserve_range((total_frames - 1U) * PAGE_SIZE, 2));

    boot_info.DescriptorSize = sizeof(map[0]) - 1U;
    assert(!pmm_init(&boot_info));
    puts("PMM boundary and accounting tests passed");
    return 0;
}
