/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production UEFI MGFS reader with host-backed Block I/O and pool allocation. */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <mgfs.h>
#include <memory.h>

static FILE *image;
static unsigned reads, allocations;

static EFI_STATUS EFIAPI read_blocks(EFI_BLOCK_IO_PROTOCOL *protocol,
                                     u32 media_id, u64 lba,
                                     usize size, void *buffer)
{
    assert(protocol && media_id == 7 && size == MGFS_BOOT_BLOCK_BYTES);
    assert(fseek(image, (long)(lba * 512), SEEK_SET) == 0);
    reads++;
    return fread(buffer, 1, size, image) == size ? EFI_SUCCESS : EFI_DEVICE_ERROR;
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
    image = fopen(path, "rb");
    assert(image && fseek(image, 0, SEEK_END) == 0);
    long bytes = ftell(image);
    assert(bytes > 0 && bytes % 512 == 0);
    media.BlockSize = 512;
    media.LastBlock = (u64)bytes / 512 - 1;
    io.Media = &media;
    io.ReadBlocks = read_blocks;
    reads = allocations = 0;
    EFI_STATUS status = mgfs_boot_init(&fs, &io, 7, 0, (u64)bytes / 512);
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
