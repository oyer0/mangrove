/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <kloader.h>
#include <memory.h>
#include <address_layout.h>

static unsigned allocation_calls;
static unsigned seek_calls;
static unsigned read_calls;
static unsigned char file_bytes[0x300];
static unsigned char *allocated_image;
static usize allocated_pages;
static bool fail_allocation;

EFI_STATUS memory_allocate(EFI_MEMORY_TYPE type, usize size, void **buffer)
{
    (void)type;
    (void)size;
    (void)buffer;
    assert(0);
    return EFI_OUT_OF_RESOURCES;
}

EFI_STATUS memory_allocate_pages(EFI_ALLOCATE_TYPE type,
                                 EFI_MEMORY_TYPE memory_type,
                                 usize pages,
                                 EFI_PHYSICAL_ADDRESS *address)
{
    allocation_calls++;
    assert(type == AllocateAnyPages);
    assert(memory_type == EFI_LOADER_DATA);
    assert(pages == 3U);
    allocated_pages = pages;
    if (fail_allocation) return EFI_NOT_FOUND;
    allocated_image = aligned_alloc(EFI_PAGE_SIZE,
                                    pages * EFI_PAGE_SIZE);
    assert(allocated_image);
    memset(allocated_image, 0xCC, pages * EFI_PAGE_SIZE);
    *address = (EFI_PHYSICAL_ADDRESS)(uintptr_t)allocated_image;
    return EFI_SUCCESS;
}

EFI_STATUS filesystem_seek(BOOT_FILE *file, u64 position)
{
    (void)file;
    seek_calls++;
    assert(position <= sizeof(file_bytes));
    ((BOOT_FILE *)file)->position = position;
    return EFI_SUCCESS;
}

EFI_STATUS filesystem_read(BOOT_FILE *file, void *buffer, usize *size)
{
    read_calls++;
    assert(file->position <= sizeof(file_bytes));
    assert(*size <= sizeof(file_bytes) - file->position);
    memcpy(buffer, file_bytes + file->position, *size);
    file->position += *size;
    return EFI_SUCCESS;
}

int main(void)
{
    BOOT_FILE kernel = {.size = sizeof(file_bytes)};
    ELF_HEADER header = {
        .Entry = KERNEL_IMAGE_VIRT_BASE + 4U,
        .ProgramHeaderCount = 3U
    };
    ELF_PROGRAM_HEADER program_headers[3] = {
        {
            .Type = PT_LOAD,
            .Flags = 1U,
            .Offset = 0x100U,
            .VirtualAddress = KERNEL_IMAGE_VIRT_BASE,
            .PhysicalAddress = 0x00100000ULL,
            .FileSize = 4U,
            .MemorySize = 8U,
            .Alignment = EFI_PAGE_SIZE
        },
        {.Type = 0U},
        {
            .Type = PT_LOAD,
            .Flags = 2U,
            .Offset = 0x200U,
            .VirtualAddress = KERNEL_IMAGE_VIRT_BASE + 0x2000U,
            .PhysicalAddress = 0x00102000ULL,
            .FileSize = 3U,
            .MemorySize = EFI_PAGE_SIZE,
            .Alignment = EFI_PAGE_SIZE
        }
    };
    ELF_LOAD_FAILURE failure;
    void *entry = NULL;
    u64 image_base = 0;

    for (usize i = 0; i < sizeof(file_bytes); i++)
        file_bytes[i] = (u8)i;

    assert(elf_load_segments(&kernel, &header, program_headers, &entry,
                             &image_base, &failure) == EFI_SUCCESS);
    assert(allocation_calls == 1U && allocated_pages == 3U);
    assert(image_base == (u64)(uintptr_t)allocated_image);
    assert((u64)(uintptr_t)entry == header.Entry);
    assert(seek_calls == 2U && read_calls == 2U);
    assert(memcmp(allocated_image, file_bytes + 0x100U, 4U) == 0);
    for (usize i = 4U; i < 8U; i++) assert(allocated_image[i] == 0U);
    assert(memcmp(allocated_image + 0x2000U, file_bytes + 0x200U, 3U) == 0);
    assert(allocated_image[0x2003U] == 0U);
    assert(kernel_image_relocate_phys(0x00102000U, image_base) ==
           image_base + 0x2000U);
    assert(kernel_image_virt_to_phys(
               KERNEL_IMAGE_VIRT_BASE + 0x2000U, image_base) ==
           image_base + 0x2000U);
    free(allocated_image);
    allocated_image = NULL;

    allocation_calls = seek_calls = read_calls = 0;
    fail_allocation = true;
    assert(elf_load_segments(&kernel, &header, program_headers, &entry,
                             &image_base, &failure) == EFI_NOT_FOUND);
    assert(failure.Stage == ELF_LOAD_FAILURE_ALLOCATION);
    assert(failure.ProgramHeaderIndex == 0U);
    assert(failure.PhysicalAddress == 0x00100000ULL);
    assert(failure.Pages == 3U);
    assert(failure.FileOffset == 0U);
    assert(allocation_calls == 1U);
    assert(seek_calls == 0U && read_calls == 0U);

    fail_allocation = false;
    program_headers[0].PhysicalAddress = 0x80000U;
    assert(elf_load_segments(&kernel, &header, program_headers, &entry,
                             &image_base, &failure) == EFI_COMPROMISED_DATA);
    assert(failure.Stage == ELF_LOAD_FAILURE_SEGMENT);
    assert(allocation_calls == 1U);
    return 0;
}
