/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <block_io.h>

static void block_io_copy(void *destination, const void *source, usize size)
{
    u8 *out = (u8 *)destination;
    const u8 *in = (const u8 *)source;
    for (usize i = 0; i < size; i++) out[i] = in[i];
}

EFI_STATUS block_io_read_blocks(EFI_BOOT_SERVICES *boot_services,
                                EFI_BLOCK_IO_PROTOCOL *block_io,
                                u32 media_id, u64 lba,
                                usize buffer_size, void *buffer)
{
    EFI_BLOCK_IO_MEDIA *media;
    u32 alignment;
    usize allocation_size;
    void *allocation = NULL;
    uintptr_t aligned_address;
    EFI_STATUS status;
    EFI_STATUS free_status;

    if (!block_io || !block_io->Media || !block_io->ReadBlocks ||
        !buffer || buffer_size == 0U) return EFI_INVALID_PARAMETER;
    media = block_io->Media;
    alignment = media->IoAlign;
    if (media->BlockSize == 0U || buffer_size < media->BlockSize ||
        buffer_size % media->BlockSize != 0U ||
        (alignment != 0U && (alignment & (alignment - 1U)) != 0U) ||
        lba > media->LastBlock ||
        buffer_size / media->BlockSize - 1U > media->LastBlock - lba) {
        return EFI_INVALID_PARAMETER;
    }
    if (!boot_services || !boot_services->AllocatePool ||
        !boot_services->FreePool) return EFI_UNSUPPORTED;

    if (alignment == 0U) alignment = 1U;
    if (buffer_size > (usize)-1 - (alignment - 1U)) {
        return EFI_OUT_OF_RESOURCES;
    }
    allocation_size = buffer_size + alignment - 1U;
    status = boot_services->AllocatePool(EFI_BOOT_SERVICES_DATA,
                                          allocation_size, &allocation);
    if (EFI_ERROR(status)) return status;
    if (!allocation) return EFI_OUT_OF_RESOURCES;
    if ((uintptr_t)allocation > (uintptr_t)-1 - (alignment - 1U)) {
        (void)boot_services->FreePool(allocation);
        return EFI_OUT_OF_RESOURCES;
    }
    aligned_address = (uintptr_t)
        (((uintptr_t)allocation + alignment - 1U) &
         ~((uintptr_t)alignment - 1U));
    status = block_io->ReadBlocks(block_io, media_id, lba, buffer_size,
                                  (void *)aligned_address);
    if (status == EFI_SUCCESS) {
        block_io_copy(buffer, (const void *)aligned_address, buffer_size);
    }
    free_status = boot_services->FreePool(allocation);
    return status == EFI_SUCCESS && EFI_ERROR(free_status)
        ? free_status : status;
}
