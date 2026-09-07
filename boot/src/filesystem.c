#include <filesystem.h>
#include <memory.h>

#define GPT_HEADER_BYTES 92U
#define GPT_ENTRY_BYTES_MIN 128U
#define GPT_ENTRY_BYTES_MAX 512U
#define GPT_ENTRY_LIMIT 128U
#define GPT_ROOT_NAME "MANGROVE_ROOT"
#define GPT_ESP_NAME "MANGROVE_ESP"

static EFI_BOOT_SERVICES *BootServices;
static MGFS_BOOT_FS RootFs;

static u32 filesystem_le32(const u8 *data)
{
    return (u32)data[0] | ((u32)data[1] << 8U) |
           ((u32)data[2] << 16U) | ((u32)data[3] << 24U);
}

static u64 filesystem_le64(const u8 *data)
{
    u64 value = 0;
    for (u32 i = 0; i < 8U; i++) value |= (u64)data[i] << (i * 8U);
    return value;
}

static bool filesystem_equal(const u8 *left, const u8 *right, usize size)
{
    for (usize i = 0; i < size; i++) if (left[i] != right[i]) return false;
    return true;
}

static bool filesystem_read_blocks(EFI_BLOCK_IO_PROTOCOL *block_io,
                                   u32 media_id, u64 lba, u32 count,
                                   void *buffer)
{
    EFI_BLOCK_IO_MEDIA *media;
    usize bytes;

    if (!block_io || !block_io->Media || !block_io->ReadBlocks ||
        !buffer || count == 0U) return false;
    media = block_io->Media;
    if (media->BlockSize != 512U || lba > media->LastBlock ||
        count - 1U > media->LastBlock - lba) return false;
    bytes = (usize)count * media->BlockSize;
    return block_io->ReadBlocks(block_io, media_id, lba, bytes, buffer) == EFI_SUCCESS;
}

static bool filesystem_gpt_name(const u8 *entry, const char *name)
{
    usize name_length = 0;
    while (name[name_length] != '\0') {
        if (++name_length > 36U) return false;
    }
    for (usize i = 0; i < 36U; i++) {
        u16 value = (u16)entry[56U + i * 2U] |
                    (u16)entry[57U + i * 2U] << 8U;
        if (i < name_length) {
            if (value != (u16)(u8)name[i]) return false;
        } else if (value != 0U) {
            return false;
        }
    }
    return true;
}

static bool filesystem_gpt_esp_type(const u8 *entry)
{
    static const u8 esp_guid[16] = {
        0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
        0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
    };
    return filesystem_equal(entry, esp_guid, sizeof(esp_guid));
}

static EFI_STATUS filesystem_find_root(u64 loaded_esp_start,
                                        u64 loaded_esp_end,
                                        bool loaded_partition,
                                        EFI_BLOCK_IO_PROTOCOL **root_disk,
                                        u64 *root_start,
                                        u64 *root_blocks)
{
    EFI_HANDLE *handles = NULL;
    usize handle_count = 0;
    EFI_LOCATE_HANDLE_BUFFER locate_handles;
    bool found = false;
    EFI_STATUS status;

    locate_handles = (EFI_LOCATE_HANDLE_BUFFER)BootServices->LocateHandleBuffer;
    if (!locate_handles) return EFI_UNSUPPORTED;
    status = locate_handles(EFI_LOCATE_BY_PROTOCOL,
                            &EFI_BLOCK_IO_PROTOCOL_GUID, NULL,
                            &handle_count, &handles);
    if (EFI_ERROR(status)) return status;
    for (usize handle_index = 0; handle_index < handle_count; handle_index++) {
        EFI_BLOCK_IO_PROTOCOL *disk = NULL;
        EFI_BLOCK_IO_MEDIA *media;
        u8 header[512];
        u64 entries_lba;
        u32 entry_count;
        u32 entry_size;
        bool disk_has_root = false;
        bool root_ambiguous = false;
        bool disk_has_boot = false;
        bool boot_ambiguous = false;
        u64 candidate_root_start = 0;
        u64 candidate_root_end = 0;
        status = BootServices->HandleProtocol(handles[handle_index],
                                              &EFI_BLOCK_IO_PROTOCOL_GUID,
                                              (void **)&disk);
        if (EFI_ERROR(status) || !disk || !disk->Media ||
            !disk->ReadBlocks) continue;
        media = disk->Media;
        bool header_read = filesystem_read_blocks(disk, media->MediaId, 1ULL,
                                                   1U, header);
        if (!media->MediaPresent || media->LogicalPartition ||
            media->BlockSize != 512U || media->LastBlock < 2ULL ||
            !header_read ||
            !filesystem_equal(header, (const u8 *)"EFI PART", 8U)) continue;

        if (filesystem_le32(header + 8U) == 0U ||
            filesystem_le32(header + 12U) < GPT_HEADER_BYTES ||
            filesystem_le32(header + 12U) > 512U) continue;
        entries_lba = filesystem_le64(header + 72U);
        entry_count = filesystem_le32(header + 80U);
        entry_size = filesystem_le32(header + 84U);
        if (entry_count == 0U || entry_count > GPT_ENTRY_LIMIT ||
            entry_size < GPT_ENTRY_BYTES_MIN || entry_size > GPT_ENTRY_BYTES_MAX ||
            entries_lba > media->LastBlock) continue;

        for (u32 entry_index = 0; entry_index < entry_count; entry_index++) {
            u8 entry_sectors[1024];
            u64 byte_offset = (u64)entry_index * entry_size;
            u64 entry_lba = entries_lba + byte_offset / 512ULL;
            u32 within = (u32)(byte_offset % 512ULL);
            u32 sectors = (within + entry_size + 511U) / 512U;
            const u8 *entry;
            u64 first;
            u64 last;
            bool nonzero_type = false;

            if (sectors > 2U || entry_lba > media->LastBlock ||
                sectors - 1U > media->LastBlock - entry_lba ||
                !filesystem_read_blocks(disk, media->MediaId, entry_lba,
                                         sectors, entry_sectors)) continue;
            entry = entry_sectors + within;
            first = filesystem_le64(entry + 32U);
            last = filesystem_le64(entry + 40U);
            for (u32 byte = 0; byte < 16U; byte++) {
                if (entry[byte] != 0U) nonzero_type = true;
            }
            if (!nonzero_type || first > last || last > media->LastBlock) continue;
            if (filesystem_gpt_name(entry, GPT_ROOT_NAME)) {
                if (disk_has_root) {
                    root_ambiguous = true;
                } else {
                    disk_has_root = true;
                    candidate_root_start = first;
                    candidate_root_end = last;
                }
            }
            if (filesystem_gpt_name(entry, GPT_ESP_NAME) &&
                filesystem_gpt_esp_type(entry) &&
                (!loaded_partition ||
                 (first == loaded_esp_start && last == loaded_esp_end))) {
                if (disk_has_boot) boot_ambiguous = true;
                else disk_has_boot = true;
            }
        }

        if (disk_has_root && !root_ambiguous && disk_has_boot && !boot_ambiguous) {
            if (found) {
                status = EFI_COMPROMISED_DATA;
                goto done;
            }
            *root_disk = disk;
            *root_start = candidate_root_start;
            *root_blocks = candidate_root_end - candidate_root_start + 1ULL;
            found = true;
        }
    }

    status = found ? EFI_SUCCESS : EFI_NOT_FOUND;
done:
    if (handles) BootServices->FreePool(handles);
    return status;
}

EFI_STATUS filesystem_init(EFI_HANDLE ImageHandle,
                            EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    EFI_BLOCK_IO_PROTOCOL *loaded_esp = NULL;
    EFI_PARTITION_INFO_PROTOCOL *partition_info = NULL;
    EFI_BLOCK_IO_PROTOCOL *root_disk = NULL;
    u64 esp_start = 0;
    u64 esp_end = 0;
    bool loaded_partition = false;
    EFI_STATUS status;

    if (!ImageHandle || !SystemTable || !SystemTable->BootServices) {
        return EFI_INVALID_PARAMETER;
    }
    BootServices = SystemTable->BootServices;
    status = BootServices->HandleProtocol(
        ImageHandle, &EFI_LOADED_IMAGE_PROTOCOL_GUID,
        (void **)&loaded_image);
    if (EFI_ERROR(status) || !loaded_image) return status;
    status = BootServices->HandleProtocol(
        loaded_image->DeviceHandle, &EFI_BLOCK_IO_PROTOCOL_GUID,
        (void **)&loaded_esp);
    if (EFI_ERROR(status) || !loaded_esp || !loaded_esp->Media) return status;

    status = BootServices->HandleProtocol(
        loaded_image->DeviceHandle, &EFI_PARTITION_INFO_PROTOCOL_GUID,
        (void **)&partition_info);
    if (!EFI_ERROR(status) && partition_info &&
        partition_info->Type == EFI_PARTITION_TYPE_GPT) {
        esp_start = partition_info->Info.Gpt.StartingLBA;
        esp_end = partition_info->Info.Gpt.EndingLBA;
        loaded_partition = true;
    }
    status = filesystem_find_root(esp_start, esp_end,
                                  loaded_partition, &root_disk,
                                  &esp_start, &esp_end);
    if (EFI_ERROR(status)) return status;
    return mgfs_boot_init(&RootFs, root_disk, root_disk->Media->MediaId,
                          esp_start, esp_end);
}

EFI_STATUS filesystem_open(const char *Path, BOOT_FILE **File)
{
    BOOT_FILE *file;
    EFI_STATUS status;

    if (!Path || !File) return EFI_INVALID_PARAMETER;
    status = memory_allocate(EFI_LOADER_DATA, sizeof(*file), (void **)&file);
    if (EFI_ERROR(status)) return status;
    status = mgfs_boot_open(&RootFs, Path, file);
    if (EFI_ERROR(status)) {
        memory_free(file);
        return status;
    }
    *File = file;
    return EFI_SUCCESS;
}

EFI_STATUS filesystem_read(BOOT_FILE *File, void *Buffer, usize *BufferSize)
{
    return mgfs_boot_read(File, Buffer, BufferSize);
}

EFI_STATUS filesystem_seek(BOOT_FILE *File, u64 Position)
{
    return mgfs_boot_seek(File, Position);
}
