/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production UEFI MGFS reader with host-backed Block I/O and pool allocation. */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <mgfs.h>
#include <block_io.h>
#include <memory.h>

static FILE *image;
static unsigned reads, allocations;
static unsigned pool_allocations, pool_frees;

static EFI_STATUS EFIAPI read_blocks(EFI_BLOCK_IO_PROTOCOL *protocol,
                                     u32 media_id, u64 lba,
                                     usize size, void *buffer)
{
    assert(protocol);
    if (media_id != 7) return EFI_INVALID_PARAMETER;
    assert(size != 0 && size % 512 == 0);
    assert((uintptr_t)buffer % protocol->Media->IoAlign == 0);
    assert(fseek(image, (long)(lba * 512), SEEK_SET) == 0);
    reads++;
    return fread(buffer, 1, size, image) == size ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

static EFI_STATUS EFIAPI allocate_pool(EFI_MEMORY_TYPE memory_type,
                                       usize size, void **buffer)
{
    assert(memory_type == EFI_BOOT_SERVICES_DATA);
    pool_allocations++;
    *buffer = malloc(size);
    return *buffer ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

static EFI_STATUS EFIAPI free_pool(void *buffer)
{
    pool_frees++;
    free(buffer);
    return EFI_SUCCESS;
}

EFI_STATUS memory_allocate(EFI_MEMORY_TYPE type, usize size, void **buffer)
{
    assert(type == EFI_LOADER_DATA);
    allocations++;
    *buffer = malloc(size);
    return *buffer ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

EFI_STATUS memory_free(void *buffer)
{
    free(buffer);
    return EFI_SUCCESS;
}

static void check(const char *path, bool current)
{
    MGFS_BOOT_FS fs;
    EFI_BLOCK_IO_MEDIA media = {0};
    EFI_BLOCK_IO_PROTOCOL io = {0};
    EFI_BOOT_SERVICES boot_services = {0};
    image = fopen(path, "rb");
    assert(image && fseek(image, 0, SEEK_END) == 0);
    long bytes = ftell(image);
    assert(bytes > 0 && bytes % 512 == 0);
    media.BlockSize = 512;
    media.IoAlign = 8192;
    media.LastBlock = (u64)bytes / 512 - 1;
    io.Media = &media;
    io.ReadBlocks = read_blocks;
    boot_services.AllocatePool = allocate_pool;
    boot_services.FreePool = free_pool;
    reads = allocations = 0;
    pool_allocations = pool_frees = 0;
    {
        u8 unaligned[513];
        assert(block_io_read_blocks(&boot_services, &io, 7, 0, 512,
                                    unaligned + 1) == EFI_SUCCESS);
        assert(pool_allocations == 1 && pool_frees == 1);
        media.IoAlign = 3;
        assert(block_io_read_blocks(&boot_services, &io, 7, 0, 512,
                                    unaligned + 1) == EFI_INVALID_PARAMETER);
        media.IoAlign = 8192;
        assert(block_io_read_blocks(&boot_services, &io, 0, 0, 512,
                                    unaligned + 1) == EFI_INVALID_PARAMETER);
    }
    reads = allocations = 0;
    pool_allocations = pool_frees = 0;
    EFI_STATUS status = mgfs_boot_init(&fs, &boot_services, &io, 7, 0,
                                       (u64)bytes / 512);
    assert(pool_allocations == pool_frees && pool_allocations == reads);
    if (current) {
        assert(status == EFI_SUCCESS);
        assert(reads > 1 && allocations == 1);
        memory_free(fs.record_bitmap);
    } else {
        assert(status == EFI_COMPROMISED_DATA);
        assert(reads == 1 && allocations == 0);
    }
    fclose(image);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    check(argv[1], true);
    check(argv[2], false);
    puts("UEFI MGFS 1.2 admission and 1.1 rejection: production paths passed");
    return 0;
}
