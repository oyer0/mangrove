/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <filesystem.h>
#include <block_io.h>
#if X250_BOOT_TRACE
#include <console.h>
#endif
#include <memory.h>

#ifndef X250_BOOT_TRACE
#define X250_BOOT_TRACE 0
#endif

#define GPT_HEADER_BYTES 92U
#define GPT_ENTRY_BYTES_MIN 128U
#define GPT_ENTRY_BYTES_MAX 512U
#define GPT_ENTRY_LIMIT 128U
#if X250_BOOT_TRACE
#define FILESYSTEM_DIAGNOSTIC_CANDIDATES 4U
#define FILESYSTEM_DIAGNOSTIC_PARTITIONS 4U
#endif
#define GPT_ROOT_NAME "MANGROVE_ROOT"
#define GPT_ESP_NAME "MANGROVE_ESP"

_Static_assert(sizeof(void *) == 8U, "UEFI x86-64 pointers must be 64-bit");
_Static_assert(__builtin_offsetof(EFI_BLOCK_IO_MEDIA, BlockSize) == 12U,
               "UEFI Block I/O media ABI mismatch");
_Static_assert(__builtin_offsetof(EFI_BLOCK_IO_MEDIA, IoAlign) == 16U,
               "UEFI Block I/O media ABI mismatch");
_Static_assert(__builtin_offsetof(EFI_BLOCK_IO_MEDIA, LastBlock) == 24U,
               "UEFI Block I/O media ABI mismatch");
_Static_assert(sizeof(EFI_BLOCK_IO_MEDIA) == 48U,
               "UEFI Block I/O media ABI mismatch");
_Static_assert(__builtin_offsetof(EFI_BLOCK_IO_PROTOCOL, Media) == 8U,
               "UEFI Block I/O protocol ABI mismatch");
_Static_assert(__builtin_offsetof(EFI_BLOCK_IO_PROTOCOL, Reset) == 16U,
               "UEFI Block I/O protocol ABI mismatch");
_Static_assert(__builtin_offsetof(EFI_BLOCK_IO_PROTOCOL, ReadBlocks) == 24U,
               "UEFI Block I/O protocol ABI mismatch");
_Static_assert(__builtin_offsetof(EFI_BLOCK_IO_PROTOCOL, WriteBlocks) == 32U,
               "UEFI Block I/O protocol ABI mismatch");
_Static_assert(__builtin_offsetof(EFI_BLOCK_IO_PROTOCOL, FlushBlocks) == 40U,
               "UEFI Block I/O protocol ABI mismatch");
_Static_assert(sizeof(EFI_BLOCK_IO_PROTOCOL) == 48U,
               "UEFI Block I/O protocol ABI mismatch");
#if X250_BOOT_TRACE
_Static_assert(__builtin_offsetof(EFI_DISK_IO_PROTOCOL, ReadDisk) == 8U,
               "UEFI Disk I/O protocol ABI mismatch");
_Static_assert(__builtin_offsetof(EFI_DISK_IO_PROTOCOL, WriteDisk) == 16U,
               "UEFI Disk I/O protocol ABI mismatch");
_Static_assert(sizeof(EFI_DISK_IO_PROTOCOL) == 24U,
               "UEFI Disk I/O protocol ABI mismatch");
#endif
_Static_assert(__builtin_offsetof(EFI_LOADED_IMAGE_PROTOCOL, DeviceHandle) ==
                   24U,
               "UEFI Loaded Image protocol ABI mismatch");
_Static_assert(__builtin_offsetof(EFI_BOOT_SERVICES, HandleProtocol) == 152U,
               "UEFI Boot Services protocol ABI mismatch");
_Static_assert(__builtin_offsetof(EFI_BOOT_SERVICES, LocateHandleBuffer) ==
                   312U,
               "UEFI Boot Services locate ABI mismatch");
_Static_assert(__builtin_offsetof(EFI_SYSTEM_TABLE, BootServices) == 96U,
               "UEFI System Table ABI mismatch");
_Static_assert(__builtin_offsetof(EFI_SYSTEM_TABLE, ConfigurationTable) ==
                   112U,
               "UEFI System Table ABI mismatch");

#if X250_BOOT_TRACE
typedef enum
{
    FILESYSTEM_CANDIDATE_NOT_CHECKED,
    FILESYSTEM_CANDIDATE_NO_MEDIA,
    FILESYSTEM_CANDIDATE_INVALID_GEOMETRY,
    FILESYSTEM_CANDIDATE_HEADER_READ_FAILED,
    FILESYSTEM_CANDIDATE_NO_GPT_SIGNATURE,
    FILESYSTEM_CANDIDATE_INVALID_HEADER,
    FILESYSTEM_CANDIDATE_INVALID_ENTRY_GEOMETRY,
    FILESYSTEM_CANDIDATE_ENTRY_READ_FAILED,
    FILESYSTEM_CANDIDATE_INVALID_PARTITION,
    FILESYSTEM_CANDIDATE_ROOT_MISSING,
    FILESYSTEM_CANDIDATE_ESP_MISSING,
    FILESYSTEM_CANDIDATE_ESP_TYPE_MISMATCH,
    FILESYSTEM_CANDIDATE_ESP_RANGE_MISMATCH,
    FILESYSTEM_CANDIDATE_DUPLICATE_ROLE,
    FILESYSTEM_CANDIDATE_MATCHED
} FILESYSTEM_CANDIDATE_RESULT;

typedef struct
{
    usize handle_index;
    FILESYSTEM_CANDIDATE_RESULT result;
    EFI_STATUS header_status;
    EFI_STATUS entries_status;
    EFI_STATUS lba0_status;
    EFI_STATUS lba1_status;
    EFI_STATUS direct_lba0_status;
    EFI_STATUS direct_lba1_status;
    EFI_STATUS disk_io_lba1_status;
    EFI_STATUS esp_start_status;
    bool device_path_prefix;
    bool device_path_known;
    bool device_path_summary_valid;
    bool lba0_read;
    bool lba1_read;
    bool direct_lba0_read;
    bool direct_lba1_read;
    bool direct_lba0_attempted;
    bool direct_lba1_attempted;
    bool lba0_signature_unchanged;
    bool lba1_prefix_unchanged;
    bool direct_lba0_signature_unchanged;
    bool direct_lba1_prefix_unchanged;
    bool disk_io_attempted;
    bool disk_io_read;
    bool disk_io_prefix_unchanged;
    bool esp_start_attempted;
    bool esp_start_read;
    bool esp_start_unchanged;
    bool root_partition_found;
    bool esp_partition_found;
    bool esp_type_found;
    u64 root_start;
    u64 root_end;
    u64 last_block;
    u64 path_prefix_hash;
    usize path_prefix_nodes;
    u64 esp_partition_start;
    u64 esp_partition_end;
    EFI_GUID root_guid;
    EFI_GUID esp_guid;
    u32 block_size;
    u32 io_align;
    u32 media_id;
    u32 media_id_after_read;
    u64 block_io_revision;
    u64 disk_io_revision;
    u8 logical_partition;
    bool media_present;
    bool media_present_after_read;
    bool removable_media;
    u8 lba1_prefix[8];
    u8 direct_lba1_prefix[8];
    u8 disk_io_lba1_prefix[8];
    u8 esp_start_prefix[8];
    u8 lba0_signature[2];
    u8 direct_lba0_signature[2];
} FILESYSTEM_CANDIDATE_DIAGNOSTIC;

typedef struct
{
    usize handle_index;
    bool loaded_handle;
    bool path_valid;
    bool matches_loaded_path;
    bool parent_path_match;
    bool gpt_info;
    bool root_named;
    bool esp_named;
    bool read;
    bool unchanged;
    u32 block_size;
    u32 media_id;
    EFI_STATUS partition_info_status;
    EFI_STATUS read_status;
    u32 path_partition_number;
    u64 path_partition_start;
    u64 path_partition_size;
    u64 start;
    u64 end;
    EFI_GUID guid;
    u8 prefix[8];
    u8 signature[2];
} FILESYSTEM_PARTITION_DIAGNOSTIC;

typedef struct
{
    usize handle_count;
    usize block_io_count;
    usize whole_disk_count;
    usize partition_count;
    usize partitions_recorded;
    EFI_STATUS locate_status;
    EFI_STATUS loaded_block_status;
    EFI_STATUS loaded_partition_status;
    EFI_STATUS loaded_path_status;
    EFI_STATUS loaded_esp_read_status;
    EFI_HANDLE loaded_device_handle;
    bool loaded_block_checked;
    bool loaded_partition_checked;
    bool loaded_path_checked;
    bool locate_attempted;
    bool loaded_esp_present;
    bool loaded_gpt_partition;
    bool loaded_path_present;
    bool loaded_path_summary_valid;
    bool loaded_esp_read_attempted;
    bool loaded_esp_read;
    bool loaded_esp_read_unchanged;
    bool loaded_esp_path_partition;
    bool root_ambiguous;
    const u8 *loaded_device_path;
    u64 loaded_path_partition_start;
    u64 loaded_path_partition_size;
    u32 loaded_path_partition_number;
    u64 loaded_media_last_block;
    u32 loaded_media_block_size;
    u32 loaded_media_io_align;
    u32 loaded_media_id;
    u32 loaded_media_id_after_read;
    u64 loaded_block_io_revision;
    bool loaded_media_present;
    bool loaded_media_present_after_read;
    bool loaded_media_logical_partition;
    bool loaded_media_removable;
    EFI_GUID loaded_path_partition_guid;
    u8 loaded_esp_prefix[8];
    u8 loaded_esp_signature[2];
    u64 loaded_path_prefix_hash;
    usize loaded_path_prefix_nodes;
    u64 esp_start;
    u64 esp_end;
    EFI_GUID esp_guid;
    FILESYSTEM_CANDIDATE_DIAGNOSTIC candidates[FILESYSTEM_DIAGNOSTIC_CANDIDATES];
    FILESYSTEM_PARTITION_DIAGNOSTIC partitions[
        FILESYSTEM_DIAGNOSTIC_PARTITIONS];
    usize candidates_recorded;
} FILESYSTEM_DIAGNOSTICS;
#endif

static EFI_BOOT_SERVICES *BootServices;
static MGFS_BOOT_FS RootFs;
static _Alignas(EFI_PAGE_SIZE) u8 FilesystemGptHeader[512];
static _Alignas(EFI_PAGE_SIZE) u8 FilesystemGptEntry[1024];
#if X250_BOOT_TRACE
static _Alignas(EFI_PAGE_SIZE) u8 FilesystemDiagnosticBlock[512];
static FILESYSTEM_DIAGNOSTICS FilesystemDiagnostics;

static EFI_GUID FilesystemDevicePathProtocolGuid = {
    0x09576E91U, 0x6D3FU, 0x11D2U,
    {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}
};
#endif

#if X250_BOOT_TRACE
static const CHAR16 *filesystem_candidate_result_name(
    FILESYSTEM_CANDIDATE_RESULT result)
{
    switch (result) {
        case FILESYSTEM_CANDIDATE_NO_MEDIA: return L"no-media";
        case FILESYSTEM_CANDIDATE_INVALID_GEOMETRY: return L"geometry";
        case FILESYSTEM_CANDIDATE_HEADER_READ_FAILED: return L"header-read";
        case FILESYSTEM_CANDIDATE_NO_GPT_SIGNATURE: return L"no-signature";
        case FILESYSTEM_CANDIDATE_INVALID_HEADER: return L"header-invalid";
        case FILESYSTEM_CANDIDATE_INVALID_ENTRY_GEOMETRY: return L"entries-invalid";
        case FILESYSTEM_CANDIDATE_ENTRY_READ_FAILED: return L"entry-read";
        case FILESYSTEM_CANDIDATE_INVALID_PARTITION: return L"partition-invalid";
        case FILESYSTEM_CANDIDATE_ROOT_MISSING: return L"root-missing";
        case FILESYSTEM_CANDIDATE_ESP_MISSING: return L"esp-missing";
        case FILESYSTEM_CANDIDATE_ESP_TYPE_MISMATCH: return L"esp-type";
        case FILESYSTEM_CANDIDATE_ESP_RANGE_MISMATCH: return L"esp-range";
        case FILESYSTEM_CANDIDATE_DUPLICATE_ROLE: return L"duplicate-role";
        case FILESYSTEM_CANDIDATE_MATCHED: return L"matched";
        default: return L"not-checked";
    }
}

static void filesystem_console_decimal(usize value)
{
    CHAR16 digits[21];
    usize count = 0;
    do {
        digits[count++] = (CHAR16)(L'0' + value % 10U);
        value /= 10U;
    } while (value != 0U && count < sizeof(digits) / sizeof(digits[0]));
    while (count != 0U) {
        CHAR16 character[2] = {digits[--count], 0};
        console_write(character);
    }
}

static void filesystem_console_hex(u64 value, usize digits)
{
    CHAR16 text[17];
    for (usize i = 0; i < digits; i++) {
        u8 nibble = (u8)(value & 0xFU);
        text[digits - i - 1U] = nibble < 10U
            ? (CHAR16)(L'0' + nibble)
            : (CHAR16)(L'A' + nibble - 10U);
        value >>= 4U;
    }
    text[digits] = 0;
    console_write(text);
}
static bool filesystem_device_path_prefix(const u8 *parent, const u8 *child)
{
    usize offset = 0;

    if (!parent || !child) return false;
    for (usize nodes = 0; nodes < 256U; nodes++) {
        if (offset > 4092U) return false;
        u16 parent_length = (u16)parent[offset + 2U] |
                            (u16)parent[offset + 3U] << 8U;
        if (parent[offset] == 0x7FU && parent[offset + 1U] == 0xFFU) {
            return parent_length == 4U;
        }
        u16 child_length = (u16)child[offset + 2U] |
                           (u16)child[offset + 3U] << 8U;
        if (parent_length < 4U || child_length < 4U ||
            parent_length != child_length || offset + parent_length > 4096U) {
            return false;
        }
        for (u16 byte = 0; byte < parent_length; byte++) {
            if (parent[offset + byte] != child[offset + byte]) return false;
        }
        offset += parent_length;
    }
    return false;
}

static u32 filesystem_le32(const u8 *data);
static u64 filesystem_le64(const u8 *data);
static EFI_GUID filesystem_gpt_unique_guid(const u8 *entry);
static bool filesystem_gpt_name(const u8 *entry, const char *name);

static bool filesystem_device_path_summary(
    const u8 *path, u64 *prefix_hash, usize *prefix_nodes,
    bool *has_partition, u32 *partition_number, u64 *partition_start,
    u64 *partition_size, EFI_GUID *partition_guid)
{
    usize offset = 0;
    u64 hash = 14695981039346656037ULL;
    usize nodes = 0;

    if (!path) return false;
    if (has_partition) *has_partition = false;
    for (usize count = 0; count < 256U; count++) {
        if (offset > 4092U) return false;
        u16 length = (u16)path[offset + 2U] |
                     (u16)path[offset + 3U] << 8U;
        if (length < 4U || offset + length > 4096U) return false;
        if (path[offset] == 0x7FU) {
            if (length != 4U ||
                (path[offset + 1U] != 0xFFU &&
                 path[offset + 1U] != 0x01U)) return false;
            if (path[offset + 1U] == 0x01U) {
                for (u16 byte = 0; byte < length; byte++) {
                    hash ^= path[offset + byte];
                    hash *= 1099511628211ULL;
                }
                offset += length;
                nodes++;
                continue;
            }
            if (prefix_hash) *prefix_hash = hash;
            if (prefix_nodes) *prefix_nodes = nodes;
            return path[offset + 1U] == 0xFFU;
        }
        if (path[offset] == 0x04U && path[offset + 1U] == 0x01U) {
            if (length != 42U) return false;
            if (has_partition) *has_partition = true;
            if (partition_number) {
                *partition_number = filesystem_le32(path + offset + 4U);
            }
            if (partition_start) {
                *partition_start = filesystem_le64(path + offset + 8U);
            }
            if (partition_size) {
                *partition_size = filesystem_le64(path + offset + 16U);
            }
            if (partition_guid) {
                *partition_guid = filesystem_gpt_unique_guid(path + offset + 8U);
            }
            if (prefix_hash) *prefix_hash = hash;
            if (prefix_nodes) *prefix_nodes = nodes;
            return true;
        }
        for (u16 byte = 0; byte < length; byte++) {
            hash ^= path[offset + byte];
            hash *= 1099511628211ULL;
        }
        offset += length;
        nodes++;
    }
    return false;
}

static bool filesystem_candidate_path_match(EFI_HANDLE handle,
                                             FILESYSTEM_CANDIDATE_DIAGNOSTIC *diag)
{
    void *path = NULL;
    EFI_STATUS status = BootServices->HandleProtocol(
        handle, &FilesystemDevicePathProtocolGuid, &path);
    if (EFI_ERROR(status) || !path || !FilesystemDiagnostics.loaded_path_present) {
        return false;
    }
    diag->device_path_known = true;
    diag->device_path_prefix = filesystem_device_path_prefix(
        (const u8 *)path,
        FilesystemDiagnostics.loaded_device_path);
    diag->device_path_summary_valid = filesystem_device_path_summary(
        (const u8 *)path, &diag->path_prefix_hash,
        &diag->path_prefix_nodes, NULL, NULL, NULL, NULL, NULL);
    return diag->device_path_prefix;
}
#endif

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
                                   void *buffer, EFI_STATUS *read_status)
{
    EFI_BLOCK_IO_MEDIA *media;
    usize bytes;

    if (!block_io || !block_io->Media || !block_io->ReadBlocks ||
        !buffer || count == 0U) {
        if (read_status) *read_status = EFI_INVALID_PARAMETER;
        return false;
    }
    media = block_io->Media;
    if (media->BlockSize != 512U || lba > media->LastBlock ||
        count - 1U > media->LastBlock - lba) {
        if (read_status) *read_status = EFI_INVALID_PARAMETER;
        return false;
    }
    bytes = (usize)count * media->BlockSize;
    EFI_STATUS status = block_io_read_blocks(BootServices, block_io, media_id,
                                             lba, bytes, buffer);
    if (read_status) *read_status = status;
    return status == EFI_SUCCESS;
}

#if X250_BOOT_TRACE
static EFI_STATUS filesystem_diagnostic_direct_read(
    EFI_BLOCK_IO_PROTOCOL *block_io, u32 media_id, u64 lba, bool *attempted)
{
    EFI_BLOCK_IO_MEDIA *media = block_io->Media;
    u32 alignment = media->IoAlign;
    *attempted = false;
    if ((alignment != 0U &&
         ((alignment & (alignment - 1U)) != 0U ||
          (uintptr_t)FilesystemDiagnosticBlock % alignment != 0U)) ||
        media->BlockSize != sizeof(FilesystemDiagnosticBlock)) {
        return EFI_UNSUPPORTED;
    }
    *attempted = true;
    return block_io->ReadBlocks(block_io, media_id, lba,
                                sizeof(FilesystemDiagnosticBlock),
                                FilesystemDiagnosticBlock);
}

static void filesystem_diagnostic_capture_direct_reads(
    EFI_BLOCK_IO_PROTOCOL *block_io, u32 media_id,
    FILESYSTEM_CANDIDATE_DIAGNOSTIC *diagnostic)
{
    FilesystemDiagnosticBlock[510U] = 0xA5U;
    FilesystemDiagnosticBlock[511U] = 0xA5U;
    diagnostic->direct_lba0_status = filesystem_diagnostic_direct_read(
        block_io, media_id, 0ULL, &diagnostic->direct_lba0_attempted);
    if (diagnostic->direct_lba0_attempted &&
        diagnostic->direct_lba0_status == EFI_SUCCESS) {
        diagnostic->direct_lba0_read = true;
        diagnostic->direct_lba0_signature[0] =
            FilesystemDiagnosticBlock[510U];
        diagnostic->direct_lba0_signature[1] =
            FilesystemDiagnosticBlock[511U];
        diagnostic->direct_lba0_signature_unchanged =
            FilesystemDiagnosticBlock[510U] == 0xA5U &&
            FilesystemDiagnosticBlock[511U] == 0xA5U;
    }

    for (u32 byte = 0; byte < sizeof(diagnostic->direct_lba1_prefix); byte++) {
        FilesystemDiagnosticBlock[byte] = 0xA5U;
    }
    diagnostic->direct_lba1_status = filesystem_diagnostic_direct_read(
        block_io, media_id, 1ULL, &diagnostic->direct_lba1_attempted);
    if (diagnostic->direct_lba1_attempted &&
        diagnostic->direct_lba1_status == EFI_SUCCESS) {
        diagnostic->direct_lba1_read = true;
        diagnostic->direct_lba1_prefix_unchanged = true;
        for (u32 byte = 0; byte < sizeof(diagnostic->direct_lba1_prefix);
             byte++) {
            diagnostic->direct_lba1_prefix[byte] =
                FilesystemDiagnosticBlock[byte];
            if (FilesystemDiagnosticBlock[byte] != 0xA5U) {
                diagnostic->direct_lba1_prefix_unchanged = false;
            }
        }
    }
}

static void filesystem_diagnostic_capture_disk_io(
    EFI_HANDLE handle, u32 media_id,
    FILESYSTEM_CANDIDATE_DIAGNOSTIC *diagnostic)
{
    EFI_DISK_IO_PROTOCOL *disk_io = NULL;
    EFI_STATUS status = BootServices->HandleProtocol(
        handle, &EFI_DISK_IO_PROTOCOL_GUID, (void **)&disk_io);
    diagnostic->disk_io_lba1_status = status;
    if (EFI_ERROR(status)) return;
    if (!disk_io || !disk_io->ReadDisk) {
        diagnostic->disk_io_lba1_status = EFI_UNSUPPORTED;
        return;
    }
    diagnostic->disk_io_revision = disk_io->Revision;

    for (u32 byte = 0; byte < sizeof(diagnostic->disk_io_lba1_prefix); byte++) {
        FilesystemDiagnosticBlock[byte] = 0xA5U;
    }
    diagnostic->disk_io_attempted = true;
    status = disk_io->ReadDisk(disk_io, media_id, 512U,
                               sizeof(FilesystemDiagnosticBlock),
                               FilesystemDiagnosticBlock);
    diagnostic->disk_io_lba1_status = status;
    if (status != EFI_SUCCESS) return;
    diagnostic->disk_io_read = true;
    diagnostic->disk_io_prefix_unchanged = true;
    for (u32 byte = 0; byte < sizeof(diagnostic->disk_io_lba1_prefix); byte++) {
        diagnostic->disk_io_lba1_prefix[byte] = FilesystemDiagnosticBlock[byte];
        if (FilesystemDiagnosticBlock[byte] != 0xA5U) {
            diagnostic->disk_io_prefix_unchanged = false;
        }
    }
}

static void filesystem_diagnostic_capture_parent_esp(
    EFI_BLOCK_IO_PROTOCOL *block_io,
    FILESYSTEM_CANDIDATE_DIAGNOSTIC *diagnostic)
{
    if (!FilesystemDiagnostics.loaded_esp_path_partition ||
        !diagnostic->device_path_prefix ||
        FilesystemDiagnostics.loaded_path_partition_start >
            block_io->Media->LastBlock) {
        diagnostic->esp_start_status = EFI_UNSUPPORTED;
        return;
    }
    diagnostic->esp_start_attempted = true;
    for (u32 byte = 0; byte < sizeof(diagnostic->esp_start_prefix); byte++) {
        FilesystemDiagnosticBlock[byte] = 0xA5U;
    }
    diagnostic->esp_start_status = block_io_read_blocks(
        BootServices, block_io, block_io->Media->MediaId,
        FilesystemDiagnostics.loaded_path_partition_start,
        block_io->Media->BlockSize, FilesystemDiagnosticBlock);
    if (diagnostic->esp_start_status != EFI_SUCCESS) return;
    diagnostic->esp_start_read = true;
    diagnostic->esp_start_unchanged = true;
    for (u32 byte = 0; byte < sizeof(diagnostic->esp_start_prefix); byte++) {
        diagnostic->esp_start_prefix[byte] = FilesystemDiagnosticBlock[byte];
        if (FilesystemDiagnosticBlock[byte] != 0xA5U) {
            diagnostic->esp_start_unchanged = false;
        }
    }
}

static void filesystem_diagnostic_capture_media_state(
    EFI_BLOCK_IO_MEDIA *media,
    FILESYSTEM_CANDIDATE_DIAGNOSTIC *diagnostic)
{
    diagnostic->media_id_after_read = media->MediaId;
    diagnostic->media_present_after_read = media->MediaPresent;
}

static void filesystem_diagnostic_capture_loaded_esp(
    EFI_BLOCK_IO_PROTOCOL *block_io)
{
    if (!block_io || !block_io->Media || !block_io->ReadBlocks ||
        block_io->Media->BlockSize != sizeof(FilesystemDiagnosticBlock) ||
        block_io->Media->LastBlock < 1U) {
        FilesystemDiagnostics.loaded_esp_read_status = EFI_UNSUPPORTED;
        return;
    }
    FilesystemDiagnostics.loaded_esp_read_attempted = true;
    for (u32 byte = 0; byte < sizeof(FilesystemDiagnostics.loaded_esp_prefix);
         byte++) {
        FilesystemDiagnosticBlock[byte] = 0xA5U;
    }
    FilesystemDiagnostics.loaded_esp_read_status = block_io_read_blocks(
        BootServices, block_io, block_io->Media->MediaId, 0U,
        sizeof(FilesystemDiagnosticBlock), FilesystemDiagnosticBlock);
    FilesystemDiagnostics.loaded_media_id_after_read = block_io->Media->MediaId;
    FilesystemDiagnostics.loaded_media_present_after_read =
        block_io->Media->MediaPresent;
    if (FilesystemDiagnostics.loaded_esp_read_status != EFI_SUCCESS) return;
    FilesystemDiagnostics.loaded_esp_read = true;
    FilesystemDiagnostics.loaded_esp_read_unchanged = true;
    for (u32 byte = 0; byte < sizeof(FilesystemDiagnostics.loaded_esp_prefix);
         byte++) {
        FilesystemDiagnostics.loaded_esp_prefix[byte] =
            FilesystemDiagnosticBlock[byte];
        if (FilesystemDiagnosticBlock[byte] != 0xA5U) {
            FilesystemDiagnostics.loaded_esp_read_unchanged = false;
        }
    }
    FilesystemDiagnostics.loaded_esp_signature[0] =
        FilesystemDiagnosticBlock[510U];
    FilesystemDiagnostics.loaded_esp_signature[1] =
        FilesystemDiagnosticBlock[511U];
}

static bool filesystem_guid_equal(const EFI_GUID *left, const EFI_GUID *right)
{
    return left->Data1 == right->Data1 &&
           left->Data2 == right->Data2 &&
           left->Data3 == right->Data3 &&
           filesystem_equal(left->Data4, right->Data4, sizeof(left->Data4));
}

static void filesystem_diagnostic_capture_partition(
    EFI_HANDLE handle, usize handle_index, EFI_BLOCK_IO_PROTOCOL *block_io)
{
    FILESYSTEM_PARTITION_DIAGNOSTIC *diagnostic;
    EFI_PARTITION_INFO_PROTOCOL *partition_info = NULL;
    const u8 *path = NULL;
    u64 path_hash = 0;
    u64 path_start = 0;
    u64 path_size = 0;
    u32 path_number = 0;
    EFI_GUID path_guid = {0};
    bool has_path_partition = false;
    EFI_STATUS status;

    if (FilesystemDiagnostics.partitions_recorded >=
        FILESYSTEM_DIAGNOSTIC_PARTITIONS) return;
    diagnostic = &FilesystemDiagnostics.partitions[
        FilesystemDiagnostics.partitions_recorded++];
    diagnostic->handle_index = handle_index;
    diagnostic->loaded_handle =
        handle == FilesystemDiagnostics.loaded_device_handle;
    diagnostic->block_size = block_io->Media->BlockSize;
    diagnostic->media_id = block_io->Media->MediaId;

    status = BootServices->HandleProtocol(
        handle, &FilesystemDevicePathProtocolGuid, (void **)&path);
    if (!EFI_ERROR(status) && path) {
        diagnostic->path_valid = filesystem_device_path_summary(
            path, &path_hash, NULL, &has_path_partition,
            &path_number, &path_start, &path_size, &path_guid);
        diagnostic->parent_path_match =
            diagnostic->path_valid &&
            FilesystemDiagnostics.loaded_path_summary_valid &&
            path_hash == FilesystemDiagnostics.loaded_path_prefix_hash;
        if (has_path_partition) {
            diagnostic->path_partition_number = path_number;
            diagnostic->path_partition_start = path_start;
            diagnostic->path_partition_size = path_size;
        }
        diagnostic->matches_loaded_path =
            diagnostic->path_valid &&
            FilesystemDiagnostics.loaded_path_summary_valid &&
            has_path_partition &&
            path_hash == FilesystemDiagnostics.loaded_path_prefix_hash &&
            path_number == FilesystemDiagnostics.loaded_path_partition_number &&
            path_start == FilesystemDiagnostics.loaded_path_partition_start &&
            path_size == FilesystemDiagnostics.loaded_path_partition_size &&
            filesystem_guid_equal(
                &path_guid, &FilesystemDiagnostics.loaded_path_partition_guid);
    }
    status = BootServices->HandleProtocol(
        handle, &EFI_PARTITION_INFO_PROTOCOL_GUID,
        (void **)&partition_info);
    diagnostic->partition_info_status = status;
    if (!EFI_ERROR(status) && partition_info &&
        partition_info->Type == EFI_PARTITION_TYPE_GPT) {
        diagnostic->gpt_info = true;
        diagnostic->root_named = filesystem_gpt_name(
            (const u8 *)&partition_info->Info.Gpt, GPT_ROOT_NAME);
        diagnostic->esp_named = filesystem_gpt_name(
            (const u8 *)&partition_info->Info.Gpt, GPT_ESP_NAME);
        diagnostic->start = partition_info->Info.Gpt.StartingLBA;
        diagnostic->end = partition_info->Info.Gpt.EndingLBA;
        diagnostic->guid = partition_info->Info.Gpt.UniquePartitionGUID;
        if (FilesystemDiagnostics.loaded_gpt_partition &&
            diagnostic->start == FilesystemDiagnostics.esp_start &&
            diagnostic->end == FilesystemDiagnostics.esp_end &&
            filesystem_guid_equal(&diagnostic->guid,
                                  &FilesystemDiagnostics.esp_guid)) {
            diagnostic->matches_loaded_path = true;
        }
    }

    if (diagnostic->loaded_handle) {
        diagnostic->read_status = FilesystemDiagnostics.loaded_esp_read_status;
        diagnostic->read = FilesystemDiagnostics.loaded_esp_read;
        diagnostic->unchanged = FilesystemDiagnostics.loaded_esp_read_unchanged;
        for (u32 byte = 0; byte < sizeof(diagnostic->prefix); byte++) {
            diagnostic->prefix[byte] =
                FilesystemDiagnostics.loaded_esp_prefix[byte];
        }
        diagnostic->signature[0] =
            FilesystemDiagnostics.loaded_esp_signature[0];
        diagnostic->signature[1] =
            FilesystemDiagnostics.loaded_esp_signature[1];
    } else if (block_io->Media->BlockSize ==
               sizeof(FilesystemDiagnosticBlock)) {
        for (u32 byte = 0; byte < sizeof(diagnostic->prefix); byte++) {
            FilesystemDiagnosticBlock[byte] = 0xA5U;
        }
        FilesystemDiagnosticBlock[510U] = 0xA5U;
        FilesystemDiagnosticBlock[511U] = 0xA5U;
        diagnostic->read_status = block_io_read_blocks(
            BootServices, block_io, block_io->Media->MediaId, 0U,
            sizeof(FilesystemDiagnosticBlock), FilesystemDiagnosticBlock);
        if (diagnostic->read_status == EFI_SUCCESS) {
            diagnostic->read = true;
            diagnostic->unchanged = true;
            for (u32 byte = 0; byte < sizeof(diagnostic->prefix); byte++) {
                diagnostic->prefix[byte] = FilesystemDiagnosticBlock[byte];
                if (FilesystemDiagnosticBlock[byte] != 0xA5U) {
                    diagnostic->unchanged = false;
                }
            }
            diagnostic->signature[0] = FilesystemDiagnosticBlock[510U];
            diagnostic->signature[1] = FilesystemDiagnosticBlock[511U];
        }
    } else {
        diagnostic->read_status = EFI_UNSUPPORTED;
    }
}

#if X250_BOOT_TRACE
static void filesystem_console_status(EFI_STATUS status)
{
    if (status == EFI_SUCCESS) {
        console_write(L"OK");
    } else {
        console_write_hex(status);
    }
}

static void filesystem_console_bytes(const u8 *bytes, usize count)
{
    for (usize i = 0; i < count; i++) {
        filesystem_console_hex(bytes[i], 2U);
    }
}
#endif
#endif

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

#if X250_BOOT_TRACE
static EFI_GUID filesystem_gpt_unique_guid(const u8 *entry)
{
    EFI_GUID guid;
    guid.Data1 = filesystem_le32(entry + 16U);
    guid.Data2 = (u16)entry[20U] | (u16)entry[21U] << 8U;
    guid.Data3 = (u16)entry[22U] | (u16)entry[23U] << 8U;
    for (u32 i = 0; i < 8U; i++) guid.Data4[i] = entry[24U + i];
    return guid;
}
#endif

static EFI_STATUS filesystem_find_root(u64 loaded_esp_start,
                                        u64 loaded_esp_end,
                                        bool loaded_partition,
                                        EFI_BLOCK_IO_PROTOCOL **root_disk,
                                        u64 *root_start,
#if X250_BOOT_TRACE
                                        u64 *root_blocks,
                                        bool *ambiguous
#else
                                        u64 *root_blocks
#endif
)
{
    EFI_HANDLE *handles = NULL;
    usize handle_count = 0;
    bool found = false;
    EFI_STATUS status;

    if (!BootServices->LocateHandleBuffer) return EFI_UNSUPPORTED;
#if X250_BOOT_TRACE
    if (ambiguous) *ambiguous = false;
#endif
    status = BootServices->LocateHandleBuffer(
        EFI_LOCATE_BY_PROTOCOL, &EFI_BLOCK_IO_PROTOCOL_GUID, NULL,
        &handle_count, &handles);
#if X250_BOOT_TRACE
    FilesystemDiagnostics.locate_attempted = true;
    FilesystemDiagnostics.locate_status = status;
    FilesystemDiagnostics.handle_count = handle_count;
#endif
    if (EFI_ERROR(status)) return status;
    for (usize handle_index = 0; handle_index < handle_count; handle_index++) {
        EFI_BLOCK_IO_PROTOCOL *disk = NULL;
        EFI_BLOCK_IO_MEDIA *media;
#if X250_BOOT_TRACE
        FILESYSTEM_CANDIDATE_DIAGNOSTIC *diagnostic = NULL;
#endif
        u64 entries_lba;
        u32 entry_count;
        u32 entry_size;
        bool disk_has_root = false;
        bool root_ambiguous = false;
        bool disk_has_boot = false;
        bool boot_ambiguous = false;
#if X250_BOOT_TRACE
        bool esp_named = false;
        bool esp_type_found = false;
        bool esp_range_mismatch = false;
        bool invalid_partition = false;
#endif
        u64 candidate_root_start = 0;
        u64 candidate_root_end = 0;
        status = BootServices->HandleProtocol(handles[handle_index],
                                              &EFI_BLOCK_IO_PROTOCOL_GUID,
                                              (void **)&disk);
        if (EFI_ERROR(status) || !disk || !disk->Media ||
            !disk->ReadBlocks) continue;
#if X250_BOOT_TRACE
        FilesystemDiagnostics.block_io_count++;
#endif
        media = disk->Media;
        if (media->LogicalPartition) {
#if X250_BOOT_TRACE
            FilesystemDiagnostics.partition_count++;
            filesystem_diagnostic_capture_partition(
                handles[handle_index], handle_index, disk);
#endif
            continue;
        }
#if X250_BOOT_TRACE
        FilesystemDiagnostics.whole_disk_count++;
        if (FilesystemDiagnostics.candidates_recorded <
            FILESYSTEM_DIAGNOSTIC_CANDIDATES) {
            diagnostic = &FilesystemDiagnostics.candidates[
                FilesystemDiagnostics.candidates_recorded++];
            diagnostic->handle_index = handle_index;
            diagnostic->result = FILESYSTEM_CANDIDATE_NOT_CHECKED;
            diagnostic->header_status = EFI_SUCCESS;
            diagnostic->entries_status = EFI_SUCCESS;
            diagnostic->lba0_status = EFI_SUCCESS;
            diagnostic->lba1_status = EFI_SUCCESS;
            diagnostic->direct_lba0_status = EFI_UNSUPPORTED;
            diagnostic->direct_lba1_status = EFI_UNSUPPORTED;
            diagnostic->disk_io_lba1_status = EFI_UNSUPPORTED;
            diagnostic->esp_start_status = EFI_UNSUPPORTED;
            diagnostic->device_path_known = false;
            diagnostic->device_path_prefix = false;
            diagnostic->lba0_read = false;
            diagnostic->lba1_read = false;
            diagnostic->direct_lba0_read = false;
            diagnostic->direct_lba1_read = false;
            diagnostic->direct_lba0_attempted = false;
            diagnostic->direct_lba1_attempted = false;
            diagnostic->lba0_signature_unchanged = false;
            diagnostic->lba1_prefix_unchanged = false;
            diagnostic->direct_lba0_signature_unchanged = false;
            diagnostic->direct_lba1_prefix_unchanged = false;
            diagnostic->root_partition_found = false;
            diagnostic->esp_partition_found = false;
            diagnostic->esp_type_found = false;
            diagnostic->block_size = media->BlockSize;
            diagnostic->io_align = media->IoAlign;
            diagnostic->media_id = media->MediaId;
            diagnostic->block_io_revision = disk->Revision;
            diagnostic->last_block = media->LastBlock;
            diagnostic->logical_partition = media->LogicalPartition;
            diagnostic->media_present = media->MediaPresent;
            diagnostic->removable_media = media->RemovableMedia;
            (void)filesystem_candidate_path_match(handles[handle_index],
                                                  diagnostic);
        }
#endif
        if (!media->MediaPresent) {
#if X250_BOOT_TRACE
            if (diagnostic) diagnostic->result = FILESYSTEM_CANDIDATE_NO_MEDIA;
#endif
            continue;
        }
        if (media->BlockSize != 512U || media->LastBlock < 2ULL) {
#if X250_BOOT_TRACE
            if (diagnostic) {
                diagnostic->result = FILESYSTEM_CANDIDATE_INVALID_GEOMETRY;
            }
#endif
            continue;
        }
#if X250_BOOT_TRACE
        FilesystemGptEntry[510U] = 0xA5U;
        FilesystemGptEntry[511U] = 0xA5U;
        EFI_STATUS lba0_status = EFI_SUCCESS;
        if (filesystem_read_blocks(disk, media->MediaId, 0ULL, 1U,
                                   FilesystemGptEntry, &lba0_status) &&
            diagnostic) {
            diagnostic->lba0_read = true;
            diagnostic->lba0_signature[0] = FilesystemGptEntry[510U];
            diagnostic->lba0_signature[1] = FilesystemGptEntry[511U];
            diagnostic->lba0_signature_unchanged =
                FilesystemGptEntry[510U] == 0xA5U &&
                FilesystemGptEntry[511U] == 0xA5U;
        }
        if (diagnostic) diagnostic->lba0_status = lba0_status;
#endif
#if X250_BOOT_TRACE
        for (u32 byte = 0; byte < 8U; byte++) {
            FilesystemGptHeader[byte] = 0xA5U;
        }
        EFI_STATUS header_status = EFI_SUCCESS;
        if (!filesystem_read_blocks(disk, media->MediaId, 1ULL, 1U,
                                    FilesystemGptHeader, &header_status)) {
            if (diagnostic) {
                diagnostic->header_status = header_status;
                diagnostic->lba1_status = header_status;
                diagnostic->result = FILESYSTEM_CANDIDATE_HEADER_READ_FAILED;
                filesystem_diagnostic_capture_direct_reads(
                    disk, media->MediaId, diagnostic);
                filesystem_diagnostic_capture_disk_io(
                    handles[handle_index], media->MediaId, diagnostic);
                filesystem_diagnostic_capture_parent_esp(disk, diagnostic);
                filesystem_diagnostic_capture_media_state(media, diagnostic);
            }
            continue;
        }
        if (diagnostic) {
            diagnostic->header_status = header_status;
            diagnostic->lba1_status = header_status;
            diagnostic->lba1_read = true;
            diagnostic->lba1_prefix_unchanged = true;
            for (u32 byte = 0; byte < sizeof(diagnostic->lba1_prefix); byte++) {
                diagnostic->lba1_prefix[byte] = FilesystemGptHeader[byte];
                if (FilesystemGptHeader[byte] != 0xA5U) {
                    diagnostic->lba1_prefix_unchanged = false;
                }
            }
        }
#else
        if (!filesystem_read_blocks(disk, media->MediaId, 1ULL, 1U,
                                    FilesystemGptHeader, NULL)) {
            continue;
        }
#endif
        if (!filesystem_equal(FilesystemGptHeader,
                              (const u8 *)"EFI PART", 8U)) {
#if X250_BOOT_TRACE
            if (diagnostic) {
                diagnostic->result = FILESYSTEM_CANDIDATE_NO_GPT_SIGNATURE;
                filesystem_diagnostic_capture_direct_reads(
                    disk, media->MediaId, diagnostic);
                filesystem_diagnostic_capture_disk_io(
                    handles[handle_index], media->MediaId, diagnostic);
                filesystem_diagnostic_capture_parent_esp(disk, diagnostic);
                filesystem_diagnostic_capture_media_state(media, diagnostic);
            }
#endif
            continue;
        }

        if (filesystem_le32(FilesystemGptHeader + 8U) == 0U ||
            filesystem_le32(FilesystemGptHeader + 12U) < GPT_HEADER_BYTES ||
            filesystem_le32(FilesystemGptHeader + 12U) > 512U) {
#if X250_BOOT_TRACE
            if (diagnostic) {
                diagnostic->result = FILESYSTEM_CANDIDATE_INVALID_HEADER;
            }
#endif
            continue;
        }
        entries_lba = filesystem_le64(FilesystemGptHeader + 72U);
        entry_count = filesystem_le32(FilesystemGptHeader + 80U);
        entry_size = filesystem_le32(FilesystemGptHeader + 84U);
        if (entry_count == 0U || entry_count > GPT_ENTRY_LIMIT ||
            entry_size < GPT_ENTRY_BYTES_MIN || entry_size > GPT_ENTRY_BYTES_MAX ||
            entries_lba > media->LastBlock) {
#if X250_BOOT_TRACE
            if (diagnostic) {
                diagnostic->result =
                    FILESYSTEM_CANDIDATE_INVALID_ENTRY_GEOMETRY;
            }
#endif
            continue;
        }

        for (u32 entry_index = 0; entry_index < entry_count; entry_index++) {
            u64 byte_offset = (u64)entry_index * entry_size;
            u64 entry_lba = entries_lba + byte_offset / 512ULL;
            u32 within = (u32)(byte_offset % 512ULL);
            u32 sectors = (within + entry_size + 511U) / 512U;
            const u8 *entry;
            u64 first;
            u64 last;
            bool nonzero_type = false;

            if (sectors > 2U || entry_lba > media->LastBlock ||
                sectors - 1U > media->LastBlock - entry_lba) {
#if X250_BOOT_TRACE
                invalid_partition = true;
#endif
                continue;
            }
#if X250_BOOT_TRACE
            EFI_STATUS entries_status = EFI_SUCCESS;
#endif
            if (!filesystem_read_blocks(disk, media->MediaId, entry_lba,
                                        sectors, FilesystemGptEntry,
#if X250_BOOT_TRACE
                                        &entries_status)) {
#else
                                        NULL)) {
#endif
#if X250_BOOT_TRACE
                invalid_partition = true;
                if (diagnostic &&
                    diagnostic->entries_status == EFI_SUCCESS) {
                    diagnostic->entries_status = entries_status;
                }
#endif
                continue;
            }
            entry = FilesystemGptEntry + within;
            first = filesystem_le64(entry + 32U);
            last = filesystem_le64(entry + 40U);
            for (u32 byte = 0; byte < 16U; byte++) {
                if (entry[byte] != 0U) nonzero_type = true;
            }
            if (!nonzero_type || first > last || last > media->LastBlock) {
#if X250_BOOT_TRACE
                if (nonzero_type) invalid_partition = true;
#endif
                continue;
            }
            if (filesystem_gpt_name(entry, GPT_ROOT_NAME)) {
                if (disk_has_root) {
                    root_ambiguous = true;
                } else {
                    disk_has_root = true;
                    candidate_root_start = first;
                    candidate_root_end = last;
#if X250_BOOT_TRACE
                    if (diagnostic) {
                        diagnostic->root_partition_found = true;
                        diagnostic->root_start = first;
                        diagnostic->root_end = last;
                        diagnostic->root_guid =
                            filesystem_gpt_unique_guid(entry);
                    }
#endif
                }
            }
            if (filesystem_gpt_name(entry, GPT_ESP_NAME)) {
#if X250_BOOT_TRACE
                esp_named = true;
                if (diagnostic && !diagnostic->esp_partition_found) {
                    diagnostic->esp_partition_found = true;
                    diagnostic->esp_partition_start = first;
                    diagnostic->esp_partition_end = last;
                    diagnostic->esp_guid = filesystem_gpt_unique_guid(entry);
                }
                if (filesystem_gpt_esp_type(entry)) {
                    esp_type_found = true;
                    if (diagnostic) diagnostic->esp_type_found = true;
                    if (!loaded_partition ||
                        (first == loaded_esp_start &&
                         last == loaded_esp_end)) {
                        if (disk_has_boot) boot_ambiguous = true;
                        else disk_has_boot = true;
                    } else {
                        esp_range_mismatch = true;
                    }
                }
#else
                if (filesystem_gpt_esp_type(entry) &&
                    (!loaded_partition ||
                     (first == loaded_esp_start && last == loaded_esp_end))) {
                    if (disk_has_boot) boot_ambiguous = true;
                    else disk_has_boot = true;
                }
#endif
            }
        }

        if (disk_has_root && !root_ambiguous && disk_has_boot && !boot_ambiguous) {
#if X250_BOOT_TRACE
            if (diagnostic) {
                diagnostic->result = FILESYSTEM_CANDIDATE_MATCHED;
            }
#endif
            if (found) {
                status = EFI_COMPROMISED_DATA;
#if X250_BOOT_TRACE
                if (ambiguous) *ambiguous = true;
                FilesystemDiagnostics.root_ambiguous = true;
#endif
                goto done;
            }
            *root_disk = disk;
            *root_start = candidate_root_start;
            *root_blocks = candidate_root_end - candidate_root_start + 1ULL;
            found = true;
        }
#if X250_BOOT_TRACE
        else if (diagnostic) {
            if (root_ambiguous || boot_ambiguous) {
                diagnostic->result = FILESYSTEM_CANDIDATE_DUPLICATE_ROLE;
            } else if (!disk_has_root) {
                diagnostic->result = FILESYSTEM_CANDIDATE_ROOT_MISSING;
            } else if (!disk_has_boot && !esp_named) {
                diagnostic->result = FILESYSTEM_CANDIDATE_ESP_MISSING;
            } else if (!disk_has_boot && !esp_type_found) {
                diagnostic->result = FILESYSTEM_CANDIDATE_ESP_TYPE_MISMATCH;
            } else if (!disk_has_boot && esp_range_mismatch) {
                diagnostic->result = FILESYSTEM_CANDIDATE_ESP_RANGE_MISMATCH;
            } else if (invalid_partition) {
                diagnostic->result = FILESYSTEM_CANDIDATE_INVALID_PARTITION;
            } else {
                diagnostic->result = FILESYSTEM_CANDIDATE_NOT_CHECKED;
            }
            if (invalid_partition &&
                diagnostic->result != FILESYSTEM_CANDIDATE_DUPLICATE_ROLE) {
                diagnostic->result = FILESYSTEM_CANDIDATE_INVALID_PARTITION;
            } else if (diagnostic->entries_status != EFI_SUCCESS &&
                       diagnostic->result != FILESYSTEM_CANDIDATE_DUPLICATE_ROLE) {
                diagnostic->result = FILESYSTEM_CANDIDATE_ENTRY_READ_FAILED;
            }
        }
#endif
    }

    status = found ? EFI_SUCCESS : EFI_NOT_FOUND;
done:
    if (handles) BootServices->FreePool(handles);
    return status;
}

EFI_STATUS filesystem_init(EFI_HANDLE ImageHandle,
                           EFI_SYSTEM_TABLE *SystemTable
#if X250_BOOT_TRACE
                           , FILESYSTEM_FAILURE *Failure
#endif
)
{
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    EFI_BLOCK_IO_PROTOCOL *loaded_esp = NULL;
    EFI_PARTITION_INFO_PROTOCOL *partition_info = NULL;
    EFI_BLOCK_IO_PROTOCOL *root_disk = NULL;
    u64 esp_start = 0;
    u64 esp_end = 0;
    bool loaded_partition = false;
#if X250_BOOT_TRACE
    bool root_ambiguous = false;
#endif
    EFI_STATUS status;

#if X250_BOOT_TRACE
    u8 *diagnostic_bytes = (u8 *)&FilesystemDiagnostics;
    for (usize i = 0; i < sizeof(FilesystemDiagnostics); i++) {
        diagnostic_bytes[i] = 0U;
    }
#endif
#if X250_BOOT_TRACE
    if (Failure) *Failure = FILESYSTEM_FAILURE_NONE;
#endif
    if (!ImageHandle || !SystemTable || !SystemTable->BootServices) {
        return EFI_INVALID_PARAMETER;
    }
#if X250_BOOT_TRACE
    if (Failure) *Failure = FILESYSTEM_FAILURE_DEVICE_DISCOVERY;
#endif
    BootServices = SystemTable->BootServices;
    status = BootServices->HandleProtocol(
        ImageHandle, &EFI_LOADED_IMAGE_PROTOCOL_GUID,
        (void **)&loaded_image);
    if (EFI_ERROR(status) || !loaded_image) return status;
#if X250_BOOT_TRACE
    FilesystemDiagnostics.loaded_device_handle = loaded_image->DeviceHandle;
#endif
    status = BootServices->HandleProtocol(
        loaded_image->DeviceHandle, &EFI_BLOCK_IO_PROTOCOL_GUID,
        (void **)&loaded_esp);
#if X250_BOOT_TRACE
    FilesystemDiagnostics.loaded_block_checked = true;
    FilesystemDiagnostics.loaded_block_status = status;
#endif
    if (EFI_ERROR(status) || !loaded_esp || !loaded_esp->Media) return status;
#if X250_BOOT_TRACE
    FilesystemDiagnostics.loaded_esp_present = true;
    FilesystemDiagnostics.loaded_block_io_revision = loaded_esp->Revision;
    FilesystemDiagnostics.loaded_media_block_size = loaded_esp->Media->BlockSize;
    FilesystemDiagnostics.loaded_media_io_align = loaded_esp->Media->IoAlign;
    FilesystemDiagnostics.loaded_media_id = loaded_esp->Media->MediaId;
    FilesystemDiagnostics.loaded_media_last_block =
        loaded_esp->Media->LastBlock;
    FilesystemDiagnostics.loaded_media_present =
        loaded_esp->Media->MediaPresent;
    FilesystemDiagnostics.loaded_media_logical_partition =
        loaded_esp->Media->LogicalPartition;
    FilesystemDiagnostics.loaded_media_removable =
        loaded_esp->Media->RemovableMedia;
#endif

    status = BootServices->HandleProtocol(
        loaded_image->DeviceHandle, &EFI_PARTITION_INFO_PROTOCOL_GUID,
        (void **)&partition_info);
#if X250_BOOT_TRACE
    FilesystemDiagnostics.loaded_partition_checked = true;
    FilesystemDiagnostics.loaded_partition_status = status;
#endif
    if (!EFI_ERROR(status) && partition_info &&
        partition_info->Type == EFI_PARTITION_TYPE_GPT) {
        esp_start = partition_info->Info.Gpt.StartingLBA;
        esp_end = partition_info->Info.Gpt.EndingLBA;
        loaded_partition = true;
#if X250_BOOT_TRACE
        FilesystemDiagnostics.loaded_gpt_partition = true;
        FilesystemDiagnostics.esp_guid =
            partition_info->Info.Gpt.UniquePartitionGUID;
#endif
    }
#if X250_BOOT_TRACE
    FilesystemDiagnostics.esp_start = esp_start;
    FilesystemDiagnostics.esp_end = esp_end;
    {
        status = BootServices->HandleProtocol(
            loaded_image->DeviceHandle, &FilesystemDevicePathProtocolGuid,
            (void **)&FilesystemDiagnostics.loaded_device_path);
        FilesystemDiagnostics.loaded_path_checked = true;
        FilesystemDiagnostics.loaded_path_status = status;
        FilesystemDiagnostics.loaded_path_present =
            !EFI_ERROR(status) && FilesystemDiagnostics.loaded_device_path;
        if (FilesystemDiagnostics.loaded_path_present) {
            FilesystemDiagnostics.loaded_path_summary_valid =
                filesystem_device_path_summary(
                FilesystemDiagnostics.loaded_device_path,
                &FilesystemDiagnostics.loaded_path_prefix_hash,
                &FilesystemDiagnostics.loaded_path_prefix_nodes,
                &FilesystemDiagnostics.loaded_esp_path_partition,
                &FilesystemDiagnostics.loaded_path_partition_number,
                &FilesystemDiagnostics.loaded_path_partition_start,
                &FilesystemDiagnostics.loaded_path_partition_size,
                &FilesystemDiagnostics.loaded_path_partition_guid);
        }
        filesystem_diagnostic_capture_loaded_esp(loaded_esp);
    }
#endif
#if X250_BOOT_TRACE
    status = filesystem_find_root(esp_start, esp_end,
                                  loaded_partition, &root_disk,
                                  &esp_start, &esp_end, &root_ambiguous);
#else
    status = filesystem_find_root(esp_start, esp_end,
                                  loaded_partition, &root_disk,
                                  &esp_start, &esp_end);
#endif
    if (EFI_ERROR(status)) {
#if X250_BOOT_TRACE
        if (Failure) {
            *Failure = status == EFI_NOT_FOUND
                ? FILESYSTEM_FAILURE_ROOT_NOT_FOUND
                : root_ambiguous
                    ? FILESYSTEM_FAILURE_AMBIGUOUS_ROOT
                    : FILESYSTEM_FAILURE_DEVICE_DISCOVERY;
        }
#endif
        return status;
    }

    status = mgfs_boot_init(&RootFs, BootServices, root_disk,
                            root_disk->Media->MediaId, esp_start, esp_end);
#if X250_BOOT_TRACE
    if (EFI_ERROR(status) && Failure) *Failure = FILESYSTEM_FAILURE_MGFS;
#endif
    return status;
}

#if X250_BOOT_TRACE
void filesystem_report_diagnostics(void)
{
    console_write(L"Handles=");
    filesystem_console_decimal(FilesystemDiagnostics.handle_count);
    console_write(L" B=");
    filesystem_console_decimal(FilesystemDiagnostics.block_io_count);
    console_write(L" D=");
    filesystem_console_decimal(FilesystemDiagnostics.whole_disk_count);
    console_write(L" P=");
    filesystem_console_decimal(FilesystemDiagnostics.partition_count);
    console_write(L" locate=");
    filesystem_console_status(FilesystemDiagnostics.locate_status);
    console_write(L"\r\nESP B/I/path=");
    filesystem_console_status(FilesystemDiagnostics.loaded_block_status);
    console_write(L"/");
    filesystem_console_status(FilesystemDiagnostics.loaded_partition_status);
    console_write(L"/");
    filesystem_console_status(FilesystemDiagnostics.loaded_path_status);
    if (FilesystemDiagnostics.loaded_esp_present) {
        console_write(L" bs=");
        filesystem_console_decimal(
            FilesystemDiagnostics.loaded_media_block_size);
        console_write(L" rev=");
        filesystem_console_hex(
            FilesystemDiagnostics.loaded_block_io_revision, 8U);
        console_write(L" id=");
        filesystem_console_decimal(FilesystemDiagnostics.loaded_media_id);
        console_write(L"\r\nESP L0=");
        filesystem_console_status(FilesystemDiagnostics.loaded_esp_read_status);
        console_write(L" data=");
        if (FilesystemDiagnostics.loaded_esp_read) {
            filesystem_console_bytes(FilesystemDiagnostics.loaded_esp_prefix, 8U);
            console_write(L" sig=");
            filesystem_console_bytes(FilesystemDiagnostics.loaded_esp_signature,
                                     2U);
        } else {
            console_write(L"--");
        }
        console_write(L"\r\n");
    }

    if (FilesystemDiagnostics.loaded_esp_path_partition) {
        console_write(L"HD#");
        filesystem_console_decimal(
            FilesystemDiagnostics.loaded_path_partition_number);
        console_write(L" start=");
        filesystem_console_decimal(
            (usize)FilesystemDiagnostics.loaded_path_partition_start);
        console_write(L" size=");
        filesystem_console_decimal(
            (usize)FilesystemDiagnostics.loaded_path_partition_size);
        console_write(L"\r\n");
    }

    for (usize i = 0; i < FilesystemDiagnostics.partitions_recorded; i++) {
        FILESYSTEM_PARTITION_DIAGNOSTIC *partition =
            &FilesystemDiagnostics.partitions[i];
        console_write(L"P");
        filesystem_console_decimal(partition->handle_index);
        console_write(partition->loaded_handle ? L"*" : L" ");
        console_write(L" ");
        console_write(partition->root_named ? L"ROOT" :
                      partition->esp_named ? L"ESP " : L"other");
        console_write(L" parent=");
        console_write(partition->parent_path_match ? L"Y" : L"N");
        if (partition->path_valid &&
            partition->path_partition_size != 0U) {
            console_write(L" hd");
            filesystem_console_decimal(partition->path_partition_number);
            console_write(L"=");
            filesystem_console_decimal(
                (usize)partition->path_partition_start);
            console_write(L"+");
            filesystem_console_decimal(
                (usize)partition->path_partition_size);
        } else if (partition->gpt_info) {
            console_write(L" ");
            filesystem_console_decimal((usize)partition->start);
            console_write(L"+");
            filesystem_console_decimal(
                (usize)(partition->end - partition->start + 1U));
        }
        console_write(L" b");
        filesystem_console_decimal(partition->block_size);
        console_write(L" i=");
        filesystem_console_status(partition->partition_info_status);
        console_write(L" r=");
        filesystem_console_status(partition->read_status);
        if (partition->read) {
            console_write(L"/");
            filesystem_console_bytes(partition->prefix, 8U);
        } else {
            console_write(L"/--");
        }
        console_write(L"\r\n");
    }

    for (usize i = 0; i < FilesystemDiagnostics.candidates_recorded; i++) {
        FILESYSTEM_CANDIDATE_DIAGNOSTIC *candidate =
            &FilesystemDiagnostics.candidates[i];
        console_write(L"D");
        filesystem_console_decimal(candidate->handle_index);
        console_write(candidate->device_path_prefix ? L"* bs=" : L"  bs=");
        filesystem_console_decimal(candidate->block_size);
        console_write(L" al=");
        filesystem_console_decimal(candidate->io_align);
        console_write(L" id=");
        filesystem_console_decimal(candidate->media_id);
        console_write(L" last=");
        filesystem_console_decimal((usize)candidate->last_block);
        console_write(L" rev=");
        filesystem_console_hex(candidate->block_io_revision, 8U);
        console_write(L" ");
        console_write(filesystem_candidate_result_name(candidate->result));
        console_write(L"\r\n");
        console_write(L"D");
        filesystem_console_decimal(candidate->handle_index);
        console_write(L" L0=");
        filesystem_console_status(candidate->lba0_status);
        if (candidate->lba0_read) {
            console_write(L"/");
            filesystem_console_bytes(candidate->lba0_signature, 2U);
        }
        console_write(L" L1=");
        filesystem_console_status(candidate->lba1_status);
        if (candidate->lba1_read) {
            console_write(L"/");
            filesystem_console_bytes(candidate->lba1_prefix, 8U);
        }
        console_write(L"\r\nD");
        filesystem_console_decimal(candidate->handle_index);
        console_write(L" Disk=");
        filesystem_console_status(candidate->disk_io_lba1_status);
        if (candidate->disk_io_read) {
            console_write(L"/");
            filesystem_console_bytes(candidate->disk_io_lba1_prefix, 8U);
        }
        console_write(L" ESP@=");
        filesystem_console_status(candidate->esp_start_status);
        if (candidate->esp_start_read) {
            console_write(L"/");
            filesystem_console_bytes(candidate->esp_start_prefix, 8U);
        }
        console_write(L"\r\n");
    }
}
#endif

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
