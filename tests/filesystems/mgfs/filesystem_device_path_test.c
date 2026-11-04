/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define X250_BOOT_TRACE 1
#include "../../../boot/src/filesystem.c"

static bool diagnostic_leave_untouched;
static EFI_DISK_IO_PROTOCOL diagnostic_disk_io;
static bool diagnostic_disk_io_leave_untouched;
static EFI_PARTITION_INFO_PROTOCOL diagnostic_partition_info;
static const u8 *diagnostic_device_path;
static const u8 *diagnostic_other_device_path;

static EFI_STATUS EFIAPI diagnostic_allocate_pool(EFI_MEMORY_TYPE type,
                                                   usize size, void **buffer)
{
    assert(type == EFI_BOOT_SERVICES_DATA);
    *buffer = malloc(size);
    return *buffer ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

static EFI_STATUS EFIAPI diagnostic_free_pool(void *buffer)
{
    free(buffer);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI diagnostic_handle_protocol(EFI_HANDLE handle,
                                                     EFI_GUID *protocol,
                                                     void **interface)
{
    assert(handle == (EFI_HANDLE)(uintptr_t)1U ||
           handle == (EFI_HANDLE)(uintptr_t)5U);
    if (protocol->Data1 == EFI_DISK_IO_PROTOCOL_GUID.Data1 &&
        handle == (EFI_HANDLE)(uintptr_t)1U) {
        *interface = &diagnostic_disk_io;
        return EFI_SUCCESS;
    }
    if (protocol->Data1 == EFI_PARTITION_INFO_PROTOCOL_GUID.Data1) {
        if (handle == (EFI_HANDLE)(uintptr_t)5U) return EFI_UNSUPPORTED;
        *interface = &diagnostic_partition_info;
        return EFI_SUCCESS;
    }
    if (protocol->Data1 == 0x09576E91U) {
        *interface = (void *)(handle == (EFI_HANDLE)(uintptr_t)1U
                                  ? diagnostic_device_path
                                  : diagnostic_other_device_path);
        return EFI_SUCCESS;
    }
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI diagnostic_read_disk(EFI_DISK_IO_PROTOCOL *protocol,
                                              u32 media_id, u64 offset,
                                              usize size, void *buffer)
{
    assert(protocol == &diagnostic_disk_io);
    assert(media_id == 9U && offset == 512U && size == 512U);
    if (diagnostic_disk_io_leave_untouched) return EFI_SUCCESS;
    u8 *bytes = buffer;
    for (usize i = 0; i < size; i++) bytes[i] = 0U;
    const u8 signature[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    for (usize i = 0; i < sizeof(signature); i++) bytes[i] = signature[i];
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI diagnostic_read_blocks(EFI_BLOCK_IO_PROTOCOL *protocol,
                                                u32 media_id, u64 lba,
                                                usize size, void *buffer)
{
    assert(protocol && protocol->Media);
    assert(media_id == 9U && size == 512U && lba <= 16U);
    assert(protocol->Media->IoAlign == 0U ||
           (uintptr_t)buffer % protocol->Media->IoAlign == 0U);
    if (diagnostic_leave_untouched) return EFI_SUCCESS;
    u8 *bytes = buffer;
    for (usize i = 0; i < size; i++) bytes[i] = 0U;
    if (lba == 0U) {
        bytes[510] = 0x55U;
        bytes[511] = 0xAAU;
    } else if (lba == 1U) {
        const u8 signature[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
        for (usize i = 0; i < sizeof(signature); i++) bytes[i] = signature[i];
    } else {
        bytes[0] = 0xEBU;
        bytes[1] = 0x3CU;
    }
    return EFI_SUCCESS;
}

int main(void)
{
    const u8 parent[] = {
        1, 1, 6, 0, 0xAA, 0x55,
        0x7F, 0xFF, 4, 0
    };
    const u8 child[] = {
        1, 1, 6, 0, 0xAA, 0x55,
        4, 1, 4, 0,
        0x7F, 0xFF, 4, 0
    };
    const u8 different[] = {
        1, 1, 6, 0, 0xAA, 0x56,
        4, 1, 4, 0,
        0x7F, 0xFF, 4, 0
    };
    const u8 exact[] = {
        1, 1, 6, 0, 0xAA, 0x55,
        0x7F, 0xFF, 4, 0
    };
    const u8 malformed[] = {
        1, 1, 3, 0, 0xAA,
        0x7F, 0xFF, 4, 0
    };
    const u8 other_partition_path[52] = {
        1, 1, 6, 0, 0xAA, 0x55,
        4, 1, 42, 0,
        2, 0, 0, 0,
        0, 8, 2, 0, 0, 0, 0, 0,
        0, 0, 2, 0, 0, 0, 0, 0,
        0x88, 0x88, 0x88, 0x88, 0x22, 0x22, 0x33, 0x33,
        0x44, 0x44, 0x55, 0x55, 0x66, 0x66, 0x77, 0x77,
        0x7F, 0xFF, 4, 0
    };
    u8 loaded_path[52] = {
        1, 1, 6, 0, 0xAA, 0x55,
        4, 1, 42, 0,
        1, 0, 0, 0,
        0, 8, 0, 0, 0, 0, 0, 0,
        0, 0, 2, 0, 0, 0, 0, 0,
        0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x33, 0x33,
        0x44, 0x44, 0x55, 0x55, 0x66, 0x66, 0x77, 0x77,
        0x7F, 0xFF, 4, 0
    };
    u64 parent_hash = 0;
    u64 loaded_hash = 0;
    usize parent_nodes = 0;
    usize loaded_nodes = 0;
    bool has_partition = false;
    u32 partition_number = 0;
    u64 partition_start = 0;
    u64 partition_size = 0;
    EFI_GUID partition_guid = {0};

    assert(filesystem_device_path_prefix(parent, child));
    assert(filesystem_device_path_prefix(parent, exact));
    assert(!filesystem_device_path_prefix(parent, different));
    assert(!filesystem_device_path_prefix(child, parent));
    assert(!filesystem_device_path_prefix(malformed, child));
    assert(filesystem_device_path_summary(
        parent, &parent_hash, &parent_nodes, NULL, NULL, NULL, NULL, NULL));
    assert(filesystem_device_path_summary(
        loaded_path, &loaded_hash, &loaded_nodes, &has_partition,
        &partition_number, &partition_start, &partition_size,
        &partition_guid));
    assert(has_partition && parent_hash == loaded_hash &&
           parent_nodes == loaded_nodes && partition_number == 1U &&
           partition_start == 2048U && partition_size == 131072U &&
           partition_guid.Data1 == 0x11111111U);
    u64 other_hash = 0;
    u64 other_start = 0;
    u64 other_size = 0;
    u32 other_number = 0;
    bool other_has_partition = false;
    assert(filesystem_device_path_summary(
        other_partition_path, &other_hash, NULL, &other_has_partition,
        &other_number, &other_start, &other_size, NULL));
    assert(other_has_partition && other_hash == loaded_hash &&
           other_number == 2U && other_start == 133120U &&
           other_size == partition_size);
    puts("UEFI device-path parent-prefix diagnostics passed");
    diagnostic_device_path = loaded_path;
    diagnostic_other_device_path = other_partition_path;

    EFI_BLOCK_IO_MEDIA media = {
        .MediaId = 9U,
        .BlockSize = 512U,
        .IoAlign = 4U,
        .LastBlock = 1U
    };
    EFI_BLOCK_IO_PROTOCOL block_io = {
        .Media = &media,
        .ReadBlocks = diagnostic_read_blocks
    };
    EFI_BOOT_SERVICES boot_services = {
        .AllocatePool = diagnostic_allocate_pool,
        .FreePool = diagnostic_free_pool,
        .HandleProtocol = diagnostic_handle_protocol
    };
    BootServices = &boot_services;
    FILESYSTEM_CANDIDATE_DIAGNOSTIC diagnostic = {0};
    diagnostic_leave_untouched = true;
    filesystem_diagnostic_capture_direct_reads(&block_io, media.MediaId,
                                               &diagnostic);
    assert(diagnostic.direct_lba0_read &&
           diagnostic.direct_lba0_signature_unchanged);
    assert(diagnostic.direct_lba1_read &&
           diagnostic.direct_lba1_prefix_unchanged);
    diagnostic_leave_untouched = false;
    bool attempted = false;
    assert(filesystem_diagnostic_direct_read(&block_io, media.MediaId, 0U,
                                             &attempted) == EFI_SUCCESS);
    assert(attempted && FilesystemDiagnosticBlock[510] == 0x55U &&
           FilesystemDiagnosticBlock[511] == 0xAAU);
    assert(filesystem_diagnostic_direct_read(&block_io, media.MediaId, 1U,
                                             &attempted) == EFI_SUCCESS);
    assert(attempted &&
           filesystem_equal(FilesystemDiagnosticBlock,
                            (const u8 *)"EFI PART", 8U));
    media.LastBlock = 16U;
    FilesystemDiagnostics.loaded_esp_path_partition = true;
    FilesystemDiagnostics.loaded_path_partition_start = 16U;
    FILESYSTEM_CANDIDATE_DIAGNOSTIC candidate = {
        .device_path_prefix = true
    };
    filesystem_diagnostic_capture_parent_esp(&block_io, &candidate);
    assert(candidate.esp_start_read && candidate.esp_start_prefix[0] == 0xEBU);
    filesystem_diagnostic_capture_loaded_esp(&block_io);
    assert(FilesystemDiagnostics.loaded_esp_read &&
           FilesystemDiagnostics.loaded_esp_prefix[0] == 0U &&
           FilesystemDiagnostics.loaded_esp_signature[0] == 0x55U &&
           FilesystemDiagnostics.loaded_esp_signature[1] == 0xAAU);
    FilesystemDiagnostics.loaded_device_handle =
        (EFI_HANDLE)(uintptr_t)1U;
    FilesystemDiagnostics.loaded_path_summary_valid = true;
    FilesystemDiagnostics.loaded_path_prefix_hash = loaded_hash;
    FilesystemDiagnostics.loaded_path_partition_number = partition_number;
    FilesystemDiagnostics.loaded_path_partition_start = partition_start;
    FilesystemDiagnostics.loaded_path_partition_size = partition_size;
    FilesystemDiagnostics.loaded_path_partition_guid = partition_guid;
    FilesystemDiagnostics.loaded_gpt_partition = true;
    FilesystemDiagnostics.esp_start = partition_start;
    FilesystemDiagnostics.esp_end = partition_start + partition_size - 1U;
    FilesystemDiagnostics.esp_guid = partition_guid;
    diagnostic_partition_info.Type = EFI_PARTITION_TYPE_GPT;
    diagnostic_partition_info.Info.Gpt.StartingLBA = partition_start;
    diagnostic_partition_info.Info.Gpt.EndingLBA =
        partition_start + partition_size - 1U;
    diagnostic_partition_info.Info.Gpt.UniquePartitionGUID = partition_guid;
    const CHAR16 esp_name[] = L"MANGROVE_ESP";
    for (usize i = 0; i < sizeof(esp_name) / sizeof(esp_name[0]); i++) {
        diagnostic_partition_info.Info.Gpt.PartitionName[i] = esp_name[i];
    }
    FILESYSTEM_PARTITION_DIAGNOSTIC partition = {0};
    filesystem_diagnostic_capture_partition(
        FilesystemDiagnostics.loaded_device_handle, 3U, &block_io);
    partition = FilesystemDiagnostics.partitions[0];
    assert(partition.loaded_handle && partition.read &&
           partition.parent_path_match && partition.matches_loaded_path &&
           partition.path_partition_number == partition_number &&
           partition.path_partition_start == partition_start &&
           partition.path_partition_size == partition_size &&
           partition.esp_named && !partition.root_named &&
           partition.prefix[0] == FilesystemDiagnostics.loaded_esp_prefix[0] &&
           partition.signature[0] == 0x55U);
    FilesystemDiagnostics.partitions_recorded = 0U;
    filesystem_diagnostic_capture_partition(
        (EFI_HANDLE)(uintptr_t)5U, 5U, &block_io);
    partition = FilesystemDiagnostics.partitions[0];
    assert(!partition.loaded_handle && partition.path_valid &&
           partition.parent_path_match && !partition.matches_loaded_path &&
           partition.partition_info_status == EFI_UNSUPPORTED &&
           partition.path_partition_number == 2U &&
           partition.path_partition_start == 133120U &&
           partition.path_partition_size == 131072U);
    diagnostic_disk_io.ReadDisk = diagnostic_read_disk;
    filesystem_diagnostic_capture_disk_io((EFI_HANDLE)(uintptr_t)1U,
                                          media.MediaId, &candidate);
    assert(candidate.disk_io_read &&
           filesystem_equal(candidate.disk_io_lba1_prefix,
                            (const u8 *)"EFI PART", 8U));
    diagnostic_disk_io_leave_untouched = true;
    filesystem_diagnostic_capture_disk_io((EFI_HANDLE)(uintptr_t)1U,
                                          media.MediaId, &candidate);
    assert(candidate.disk_io_read && candidate.disk_io_prefix_unchanged);
    puts("UEFI Block/Disk I/O and partition diagnostics passed");
    media.IoAlign = 3U;
    assert(filesystem_diagnostic_direct_read(&block_io, media.MediaId, 1U,
                                             &attempted) == EFI_UNSUPPORTED);
    assert(!attempted);
    puts("UEFI direct Block I/O diagnostic reads passed");
    return 0;
}
