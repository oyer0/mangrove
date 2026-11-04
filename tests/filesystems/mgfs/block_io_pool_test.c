/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdlib.h>
#include <block_io.h>

static void *allocated_buffer;
static void *destination_buffer;
static usize allocated_size;
static EFI_STATUS read_status;
static EFI_STATUS allocation_status;
static unsigned reads;
static unsigned frees;

static EFI_STATUS EFIAPI allocate_pool(EFI_MEMORY_TYPE type, usize size,
                                       void **buffer)
{
    assert(type == EFI_BOOT_SERVICES_DATA);
    allocated_size = size;
    if (EFI_ERROR(allocation_status)) return allocation_status;
    allocated_buffer = malloc(size + 1U);
    if (!allocated_buffer) return EFI_OUT_OF_RESOURCES;
    *buffer = (u8 *)allocated_buffer + 1U;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI free_pool(void *buffer)
{
    assert(buffer == (u8 *)allocated_buffer + 1U);
    free(allocated_buffer);
    allocated_buffer = NULL;
    frees++;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI read_blocks(EFI_BLOCK_IO_PROTOCOL *protocol,
                                     u32 media_id, u64 lba, usize size,
                                     void *buffer)
{
    assert(protocol && media_id == 7U && lba == 3U && size == 512U);
    assert(protocol->Media->IoAlign == 0U ||
           (uintptr_t)buffer % protocol->Media->IoAlign == 0U);
    assert(buffer != destination_buffer);
    reads++;
    if (read_status == EFI_SUCCESS) {
        u8 *bytes = buffer;
        for (usize i = 0; i < size; i++) bytes[i] = (u8)i;
    }
    return read_status;
}

int main(void)
{
    EFI_BLOCK_IO_MEDIA media = {
        .MediaId = 7U,
        .BlockSize = 512U,
        .IoAlign = 64U,
        .LastBlock = 8U
    };
    EFI_BLOCK_IO_PROTOCOL block_io = {
        .Media = &media,
        .ReadBlocks = read_blocks
    };
    EFI_BOOT_SERVICES boot_services = {
        .AllocatePool = allocate_pool,
        .FreePool = free_pool
    };
    u8 destination[512];

    destination_buffer = destination;
    read_status = EFI_SUCCESS;
    for (usize i = 0; i < sizeof(destination); i++) destination[i] = 0xA5U;
    assert(block_io_read_blocks(&boot_services, &block_io, media.MediaId,
                                3U, sizeof(destination), destination) ==
           EFI_SUCCESS);
    assert(allocated_size == sizeof(destination) + media.IoAlign - 1U);
    assert(reads == 1U && frees == 1U && allocated_buffer == NULL);
    for (usize i = 0; i < sizeof(destination); i++) {
        assert(destination[i] == (u8)i);
    }

    read_status = EFI_DEVICE_ERROR;
    for (usize i = 0; i < sizeof(destination); i++) destination[i] = 0xA5U;
    assert(block_io_read_blocks(&boot_services, &block_io, media.MediaId,
                                3U, sizeof(destination), destination) ==
           EFI_DEVICE_ERROR);
    assert(reads == 2U && frees == 2U && allocated_buffer == NULL);
    for (usize i = 0; i < sizeof(destination); i++) {
        assert(destination[i] == 0xA5U);
    }

    read_status = EFI_SUCCESS;
    allocation_status = EFI_OUT_OF_RESOURCES;
    assert(block_io_read_blocks(&boot_services, &block_io, media.MediaId,
                                3U, sizeof(destination), destination) ==
           EFI_OUT_OF_RESOURCES);
    assert(reads == 2U && frees == 2U);

    allocation_status = EFI_SUCCESS;
    media.IoAlign = 0U;
    assert(block_io_read_blocks(&boot_services, &block_io, media.MediaId,
                                3U, sizeof(destination), destination) ==
           EFI_SUCCESS);
    assert(allocated_size == sizeof(destination));
    assert(reads == 3U && frees == 3U && allocated_buffer == NULL);
    return 0;
}
