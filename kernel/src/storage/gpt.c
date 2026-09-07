/* SPDX-License-Identifier: GPL-3.0-only */
#include <storage/gpt.h>

#include <entropy.h>
#include <kprint.h>
#include <stddef.h>
#include <string.h>
#include <timekeeping.h>
#include <vfs.h>
#include <storage/format.h>

#define GPT_SECTOR_BYTES          512U
#define GPT_HEADER_BYTES          92U
#define GPT_PRIMARY_HEADER_LBA    1ULL
#define GPT_PRIMARY_ENTRIES_LBA   2ULL
#define GPT_ENTRY_ARRAY_BYTES     (GPT_MAX_PARTITION_ENTRIES * GPT_PARTITION_ENTRY_BYTES)
#define GPT_ENTRY_ARRAY_SECTORS   (GPT_ENTRY_ARRAY_BYTES / GPT_SECTOR_BYTES)
#define GPT_ALIGNMENT_BYTES       (1024ULL * 1024ULL)
#define GPT_MIN_SECTORS           (2ULL + GPT_ENTRY_ARRAY_SECTORS + 1ULL + \
                                   GPT_ENTRY_ARRAY_SECTORS + 1ULL)
#define GPT_REGISTERED_MAX        16U

/* Mangrove's generic data partition type.  This is the on-disk EFI byte
 * order for 4d414e47-524f-5645-4441-544100000001, and is deliberately
 * distinct from the system ESP/root role markers. */
static const u8 mangrove_data_type_guid[GPT_GUID_BYTES] = {
    0x47, 0x4e, 0x41, 0x4d, 0x4f, 0x52, 0x45, 0x56,
    0x44, 0x41, 0x54, 0x41, 0x00, 0x00, 0x00, 0x01
};

static const u8 esp_type_guid[GPT_GUID_BYTES] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
};

typedef struct {
    gpt_table_info_t info;
    u64 primary_entries_lba;
    u64 backup_entries_lba;
    u32 array_sectors;
    u8 disk_guid[GPT_GUID_BYTES];
} gpt_table_t;

/* The GPT array is only 16 KiB at the fixed 128-entry standard size.  Static
 * work buffers keep validation and mutation off the small kernel stacks. */
static u8 primary_entries[GPT_ENTRY_ARRAY_BYTES];
static u8 backup_entries[GPT_ENTRY_ARRAY_BYTES];
static gpt_partition_info_t partitions[GPT_REGISTERED_MAX];
static u32 partition_count;

static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static u64 le64(const u8 *p)
{
    u64 value = 0;
    for (u32 index = 0; index < 8U; index++)
        value |= (u64)p[index] << (index * 8U);
    return value;
}

static void store_le32(u8 *p, u32 value)
{
    for (u32 index = 0; index < 4U; index++)
        p[index] = (u8)(value >> (index * 8U));
}

static void store_le64(u8 *p, u64 value)
{
    for (u32 index = 0; index < 8U; index++)
        p[index] = (u8)(value >> (index * 8U));
}

static bool add_u64(u64 left, u64 right, u64 *out)
{
    if (!out || left > ~(u64)0 - right) return false;
    *out = left + right;
    return true;
}

static bool multiply_u64(u64 left, u64 right, u64 *out)
{
    if (!out || (left && right > ~(u64)0 / left)) return false;
    *out = left * right;
    return true;
}

static bool gpt_read(block_device_t *device, u64 lba, u32 count, void *buffer)
{
    if (!device || !buffer || !count || lba > device->sector_count ||
        (u64)count > device->sector_count - lba)
        return false;
    return block_probe_read(device, lba, count, buffer);
}

static bool gpt_write(block_device_t *device, u64 lba, u32 count,
                      const void *buffer)
{
    if (!device || !buffer || !count || !block_device_is_live(device) ||
        lba > device->sector_count ||
        (u64)count > device->sector_count - lba)
        return false;
    return block_write(device, lba, count, buffer);
}

static u32 crc32(const u8 *data, usize length, u32 zero_offset)
{
    u32 crc = 0xffffffffU;

    for (usize index = 0; index < length; index++) {
        u8 value = (zero_offset != ~(u32)0U && index >= zero_offset &&
                    index - zero_offset < 4U) ? 0U : data[index];
        crc ^= value;
        for (u32 bit = 0; bit < 8U; bit++)
            crc = (crc & 1U) ? (crc >> 1U) ^ 0xedb88320U : crc >> 1U;
    }
    return crc ^ 0xffffffffU;
}

static bool guid_is_zero(const u8 *guid)
{
    if (!guid) return true;
    for (u32 index = 0; index < GPT_GUID_BYTES; index++)
        if (guid[index]) return false;
    return true;
}

static bool mbr_has_partition(const u8 *sector)
{
    if (!sector || sector[510] != 0x55 || sector[511] != 0xaa) return false;
    for (u32 index = 0; index < 4U; index++)
        if (sector[446U + index * 16U + 4U]) return true;
    return false;
}

static bool mbr_is_protective(const u8 *sector)
{
    return sector && sector[510] == 0x55 && sector[511] == 0xaa &&
           sector[446U + 4U] == 0xee;
}

static bool gpt_signature(const u8 *sector)
{
    static const u8 signature[8] = {
        'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'
    };
    return sector && memcmp(sector, signature, sizeof(signature)) == 0;
}

static bool gpt_header_valid(const u8 *header, u64 expected_current,
                             u64 expected_backup, u64 total_sectors,
                             u64 *out_entries_lba, u32 *out_count,
                             u32 *out_size, u64 *out_first,
                             u64 *out_last, u32 *out_array_crc,
                             u32 *out_array_sectors, u8 *out_guid)
{
    u32 header_size;
    u32 entry_count;
    u32 entry_size;
    u64 entries_lba;
    u64 first;
    u64 last;
    u64 array_bytes;
    u64 array_sectors;

    if (!gpt_signature(header) || le32(header + 8) != 0x00010000U)
        return false;
    if (guid_is_zero(header + 56)) return false;
    header_size = le32(header + 12);
    if (header_size < GPT_HEADER_BYTES || header_size > GPT_SECTOR_BYTES ||
        crc32(header, header_size, 16U) != le32(header + 16)) return false;
    if (le64(header + 24) != expected_current ||
        le64(header + 32) != expected_backup) return false;
    first = le64(header + 40);
    last = le64(header + 48);
    if (first > last || last >= total_sectors) return false;
    entry_count = le32(header + 80);
    entry_size = le32(header + 84);
    if (!entry_count || entry_count > GPT_MAX_PARTITION_ENTRIES ||
        entry_size != GPT_PARTITION_ENTRY_BYTES) return false;
    if (!multiply_u64(entry_count, entry_size, &array_bytes)) return false;
    array_sectors = (array_bytes + GPT_SECTOR_BYTES - 1ULL) /
                    GPT_SECTOR_BYTES;
    entries_lba = le64(header + 72);
    if (!array_sectors || entries_lba >= total_sectors ||
        array_sectors > total_sectors - entries_lba) return false;
    if (out_entries_lba) *out_entries_lba = entries_lba;
    if (out_count) *out_count = entry_count;
    if (out_size) *out_size = entry_size;
    if (out_first) *out_first = first;
    if (out_last) *out_last = last;
    if (out_array_crc) *out_array_crc = le32(header + 88);
    if (out_array_sectors) *out_array_sectors = (u32)array_sectors;
    if (out_guid) memcpy(out_guid, header + 56, GPT_GUID_BYTES);
    return true;
}

static bool gpt_entry_used(const u8 *entry)
{
    if (!entry) return false;
    for (u32 index = 0; index < GPT_GUID_BYTES; index++)
        if (entry[index]) return true;
    return false;
}

static bool gpt_entries_valid(const u8 *entries, u32 entry_count,
                              u64 first_usable, u64 last_usable,
                              u32 *out_count)
{
    u32 used = 0;

    if (!entries || entry_count > GPT_MAX_PARTITION_ENTRIES ||
        first_usable > last_usable) return false;
    for (u32 index = 0; index < entry_count; index++) {
        const u8 *entry = entries + index * GPT_PARTITION_ENTRY_BYTES;
        u64 first;
        u64 last;
        if (!gpt_entry_used(entry)) continue;
        first = le64(entry + 32);
        last = le64(entry + 40);
        if (guid_is_zero(entry + 16) || first > last ||
            first < first_usable || last > last_usable) return false;
        for (u32 prior = 0; prior < index; prior++) {
            const u8 *other = entries + prior * GPT_PARTITION_ENTRY_BYTES;
            u64 other_first;
            u64 other_last;
            if (!gpt_entry_used(other)) continue;
            other_first = le64(other + 32);
            other_last = le64(other + 40);
            if (memcmp(entry + 16, other + 16, GPT_GUID_BYTES) == 0 ||
                (first <= other_last && other_first <= last)) return false;
        }
        used++;
    }
    if (out_count) *out_count = used;
    return true;
}

static int gpt_load(block_device_t *device, gpt_table_t *out_table)
{
    u8 mbr[GPT_SECTOR_BYTES];
    u8 primary[GPT_SECTOR_BYTES];
    u8 backup[GPT_SECTOR_BYTES];
    u64 primary_entries_lba;
    u64 backup_entries_lba;
    u64 primary_first;
    u64 backup_first;
    u64 primary_last;
    u64 backup_last;
    u32 primary_count;
    u32 backup_count;
    u32 primary_size;
    u32 backup_size;
    u32 primary_crc;
    u32 backup_crc;
    u32 primary_array_sectors;
    u32 backup_array_sectors;
    u32 partition_count_value;
    u8 primary_guid[GPT_GUID_BYTES];
    u8 backup_guid[GPT_GUID_BYTES];

    if (!device || !out_table || device->type == BLOCK_DEVICE_PARTITION ||
        device->sector_size != GPT_SECTOR_BYTES ||
        device->sector_count < GPT_MIN_SECTORS)
        return VFS_ERR_UNSUPPORTED;
    if (!gpt_read(device, 0, 1U, mbr) ||
        !gpt_read(device, GPT_PRIMARY_HEADER_LBA, 1U, primary) ||
        !gpt_read(device, device->sector_count - 1ULL, 1U, backup))
        return VFS_ERR_IO;
    if (!gpt_signature(primary))
        return mbr_has_partition(mbr) ? VFS_ERR_UNSUPPORTED : VFS_ERR_NOT_FOUND;
    if (!mbr_is_protective(mbr)) return VFS_ERR_BAD_FORMAT;
    if (!gpt_signature(backup)) return VFS_ERR_BAD_FORMAT;
    if (!gpt_header_valid(primary, 1ULL, device->sector_count - 1ULL,
                          device->sector_count, &primary_entries_lba,
                          &primary_count, &primary_size, &primary_first,
                          &primary_last, &primary_crc, &primary_array_sectors,
                          primary_guid) ||
        !gpt_header_valid(backup, device->sector_count - 1ULL, 1ULL,
                          device->sector_count, &backup_entries_lba,
                          &backup_count, &backup_size, &backup_first,
                          &backup_last, &backup_crc, &backup_array_sectors,
                          backup_guid))
        return VFS_ERR_BAD_FORMAT;
    if (primary_count != backup_count || primary_size != backup_size ||
        primary_first != backup_first || primary_last != backup_last ||
        primary_array_sectors != backup_array_sectors ||
        primary_crc != backup_crc ||
        memcmp(primary_guid, backup_guid, GPT_GUID_BYTES) != 0 ||
        primary_entries_lba < GPT_PRIMARY_ENTRIES_LBA ||
        primary_entries_lba > primary_first ||
        primary_array_sectors > primary_first - primary_entries_lba ||
        backup_entries_lba > device->sector_count - 1ULL ||
        primary_array_sectors > device->sector_count - 1ULL - backup_entries_lba ||
        backup_entries_lba + (u64)primary_array_sectors !=
            device->sector_count - 1ULL ||
        primary_first > primary_last || backup_first > backup_last ||
        backup_last >= backup_entries_lba)
        return VFS_ERR_BAD_FORMAT;
    if (!gpt_read(device, primary_entries_lba, primary_array_sectors,
                  primary_entries) ||
        !gpt_read(device, backup_entries_lba, primary_array_sectors,
                  backup_entries)) return VFS_ERR_IO;
    if (crc32(primary_entries, (usize)primary_count * primary_size,
              ~(u32)0U) != primary_crc ||
        crc32(backup_entries, (usize)backup_count * backup_size,
              ~(u32)0U) != backup_crc ||
        memcmp(primary_entries, backup_entries,
               (usize)primary_count * primary_size) != 0 ||
        !gpt_entries_valid(primary_entries, primary_count, primary_first,
                           primary_last, &partition_count_value))
        return VFS_ERR_BAD_FORMAT;
    memset(out_table, 0, sizeof(*out_table));
    out_table->info.type = GPT_TABLE_VALID;
    out_table->info.partition_count = partition_count_value;
    out_table->info.entry_count = primary_count;
    out_table->info.entry_size = primary_size;
    out_table->info.first_usable_lba = primary_first;
    out_table->info.last_usable_lba = primary_last;
    out_table->primary_entries_lba = primary_entries_lba;
    out_table->backup_entries_lba = backup_entries_lba;
    out_table->array_sectors = primary_array_sectors;
    memcpy(out_table->disk_guid, primary_guid, GPT_GUID_BYTES);
    return VFS_OK;
}

static bool generate_guid(u8 *guid, u64 salt)
{
    u64 value;

    if (!guid) return false;
    for (u32 index = 0; index < 2U; index++) {
        if (!entropy_random_u64(&value)) {
            value = timekeeping_boot_id() ^ timekeeping_monotonic_ms();
            value ^= salt + (u64)index * 0x9e3779b97f4a7c15ULL;
            value ^= value >> 30;
            value *= 0xbf58476d1ce4e5b9ULL;
            value ^= value >> 27;
        }
        memcpy(guid + index * sizeof(value), &value, sizeof(value));
        salt ^= value + 0x94d049bb133111ebULL;
    }
    guid[6] = (u8)((guid[6] & 0x0fU) | 0x40U);
    guid[8] = (u8)((guid[8] & 0x3fU) | 0x80U);
    return !guid_is_zero(guid);
}

static u64 alignment_sectors(const block_device_t *device)
{
    if (!device || !device->sector_size) return 0;
    return (GPT_ALIGNMENT_BYTES + device->sector_size - 1ULL) /
           device->sector_size;
}

static bool align_up(u64 value, u64 alignment, u64 *out)
{
    u64 remainder;

    if (!out || !alignment) return false;
    remainder = value % alignment;
    if (!remainder) {
        *out = value;
        return true;
    }
    return add_u64(value, alignment - remainder, out);
}

static bool plan_from_entries(const gpt_table_t *table,
                              const block_device_t *device,
                              u64 requested_bytes, bool use_rest,
                              gpt_partition_plan_t *out_plan)
{
    u64 alignment;
    u64 requested_sectors = 0;
    u64 best_first = 0;
    u64 best_last = 0;
    u64 best_size = 0;
    u32 best_number = 0;

    if (!table || !device || !out_plan || device->sector_size != 512U)
        return false;
    alignment = alignment_sectors(device);
    if (!alignment) return false;
    if (!use_rest) {
        if (!requested_bytes || requested_bytes > ~(u64)0 -
            (device->sector_size - 1ULL)) return false;
        /* A size token describes an upper capacity bound.  Round down to
         * complete sectors rather than silently creating a partition larger
         * than requested; callers can request another byte/unit when they
         * need the next sector. */
        requested_sectors = requested_bytes / device->sector_size;
        if (!requested_sectors) return false;
    }
    /* Walk free extents in ascending order.  There are at most 128 GPT
     * entries, so sorting is unnecessary and bounded selection is clearer. */
    {
        u64 cursor = table->info.first_usable_lba;
        for (;;) {
            u64 next_first = table->info.last_usable_lba + 1ULL;
            u64 next_last = 0;
            u64 candidate_first;
            u64 gap_last;
            u64 candidate_size;
            for (u32 index = 0; index < table->info.entry_count; index++) {
                const u8 *entry = primary_entries + index * GPT_PARTITION_ENTRY_BYTES;
                u64 first;
                u64 last;
                if (!gpt_entry_used(entry)) continue;
                first = le64(entry + 32);
                last = le64(entry + 40);
                if (first >= cursor && first < next_first) {
                    next_first = first;
                    next_last = last;
                }
            }
            gap_last = next_first == table->info.last_usable_lba + 1ULL
                ? table->info.last_usable_lba : next_first - 1ULL;
            if (align_up(cursor, alignment, &candidate_first) &&
                candidate_first <= gap_last) {
                candidate_size = gap_last - candidate_first + 1ULL;
                if ((!use_rest && candidate_size >= requested_sectors) ||
                    (use_rest && candidate_size > best_size)) {
                    best_first = candidate_first;
                    best_last = use_rest ? gap_last :
                               candidate_first + requested_sectors - 1ULL;
                    best_size = use_rest ? candidate_size : requested_sectors;
                    if (!use_rest) break;
                }
            }
            if (next_first == table->info.last_usable_lba + 1ULL ||
                next_last >= table->info.last_usable_lba) break;
            cursor = next_last + 1ULL;
        }
    }
    if (!best_size || best_last < best_first) return false;
    for (u32 index = 0; index < table->info.entry_count; index++)
        if (!gpt_entry_used(primary_entries + index * GPT_PARTITION_ENTRY_BYTES)) {
            best_number = index + 1U;
            break;
        }
    if (!best_number) return false;
    out_plan->first_lba = best_first;
    out_plan->last_lba = best_last;
    if (!multiply_u64(best_last - best_first + 1ULL, device->sector_size,
                     &out_plan->size_bytes)) return false;
    out_plan->partition_number = best_number;
    return true;
}

static void gpt_make_header(u8 *header, const gpt_table_t *table,
                            bool backup, u32 array_crc)
{
    u64 current = backup ? table->backup_entries_lba + table->array_sectors
                         : GPT_PRIMARY_HEADER_LBA;
    u64 other = backup ? GPT_PRIMARY_HEADER_LBA :
                         table->backup_entries_lba + table->array_sectors;
    u64 entries = backup ? table->backup_entries_lba : table->primary_entries_lba;

    memset(header, 0, GPT_SECTOR_BYTES);
    memcpy(header, "EFI PART", 8U);
    store_le32(header + 8, 0x00010000U);
    store_le32(header + 12, GPT_HEADER_BYTES);
    store_le64(header + 24, current);
    store_le64(header + 32, other);
    store_le64(header + 40, table->info.first_usable_lba);
    store_le64(header + 48, table->info.last_usable_lba);
    memcpy(header + 56, table->disk_guid, GPT_GUID_BYTES);
    store_le64(header + 72, entries);
    store_le32(header + 80, table->info.entry_count);
    store_le32(header + 84, table->info.entry_size);
    store_le32(header + 88, array_crc);
    store_le32(header + 16, crc32(header, GPT_HEADER_BYTES, 16U));
}

static bool gpt_write_updated(block_device_t *device, gpt_table_t *table)
{
    u8 primary_header[GPT_SECTOR_BYTES];
    u8 backup_header[GPT_SECTOR_BYTES];
    u32 array_bytes;
    u32 array_crc;

    if (!device || !table || table->info.entry_count > GPT_MAX_PARTITION_ENTRIES ||
        table->info.entry_size != GPT_PARTITION_ENTRY_BYTES ||
        !table->array_sectors || table->array_sectors > GPT_ENTRY_ARRAY_SECTORS)
        return false;
    array_bytes = table->info.entry_count * table->info.entry_size;
    array_crc = crc32(primary_entries, array_bytes, ~(u32)0U);
    gpt_make_header(backup_header, table, true, array_crc);
    gpt_make_header(primary_header, table, false, array_crc);
    /* The old primary remains valid until the complete backup copy is on
     * media.  Commit the primary copy last. */
    if (!gpt_write(device, table->backup_entries_lba, table->array_sectors,
                   primary_entries) ||
        !gpt_write(device, device->sector_count - 1ULL, 1U, backup_header) ||
        !block_flush(device) ||
        !gpt_write(device, table->primary_entries_lba, table->array_sectors,
                   primary_entries) ||
        !gpt_write(device, GPT_PRIMARY_HEADER_LBA, 1U, primary_header) ||
        !block_flush(device)) return false;
    return block_device_is_live(device);
}

static bool gpt_write_protective_mbr(block_device_t *device)
{
    u8 mbr[GPT_SECTOR_BYTES];
    u64 count;

    if (!device) return false;
    memset(mbr, 0, sizeof(mbr));
    mbr[446U + 4U] = 0xee;
    memset(mbr + 446U + 1U, 0xff, 3U);
    memset(mbr + 446U + 5U, 0xff, 3U);
    store_le32(mbr + 446U + 8U, 1U);
    count = device->sector_count - 1ULL;
    if (count > 0xffffffffULL) count = 0xffffffffULL;
    store_le32(mbr + 446U + 12U, (u32)count);
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    return gpt_write(device, 0, 1U, mbr);
}

static bool gpt_preflight_children(block_device_t *device)
{
    if (!device || vfs_format_preflight(device) != VFS_OK) return false;
    for (u32 index = 0; index < block_device_count(); index++) {
        block_device_t *child = block_get_device(index);
        gpt_partition_info_t info;
        if (!child || child->type != BLOCK_DEVICE_PARTITION ||
            !gpt_get_partition_info(child, &info) ||
            !gpt_partition_parent_matches(&info, device))
            continue;
        if (vfs_format_preflight(child) != VFS_OK) return false;
    }
    return true;
}

static void discard_allocated_partitions(const u32 *indices, u32 count)
{
    for (u32 index = 0; index < count; index++) {
        (void)block_unregister_by_driver_data(&partitions[indices[index]]);
        memset(&partitions[indices[index]], 0, sizeof(partitions[indices[index]]));
    }
}

static bool partition_read(block_device_t *device, u64 lba, u32 count,
                           void *buffer)
{
    gpt_partition_info_t *part = (gpt_partition_info_t *)device->driver_data;
    u64 length;

    if (!part || part->last_lba < part->first_lba) return false;
    length = part->last_lba - part->first_lba + 1ULL;
    if (lba > length || (u64)count > length - lba) return false;
    return block_read(part->parent, part->first_lba + lba, count, buffer);
}

static bool partition_write(block_device_t *device, u64 lba, u32 count,
                            const void *buffer)
{
    gpt_partition_info_t *part = (gpt_partition_info_t *)device->driver_data;
    u64 length;

    if (!part || part->last_lba < part->first_lba) return false;
    length = part->last_lba - part->first_lba + 1ULL;
    if (lba > length || (u64)count > length - lba) return false;
    return block_write(part->parent, part->first_lba + lba, count, buffer);
}

static bool partition_flush(block_device_t *device)
{
    gpt_partition_info_t *part;

    if (!device || !(part = (gpt_partition_info_t *)device->driver_data) ||
        !part->parent || !gpt_partition_parent_matches(part, part->parent))
        return false;
    return block_flush(part->parent);
}

bool gpt_scan_device(block_device_t *device)
{
    gpt_table_t table;
    u32 discovered_count = 0;
    u32 discovered_indices[GPT_REGISTERED_MAX];
    u32 allocated_indices[GPT_REGISTERED_MAX];
    u32 allocated_count = 0;
    int result;

    result = gpt_load(device, &table);
    if (result != VFS_OK) return false;
    for (u32 index = 0; index < table.info.entry_count; index++) {
        const u8 *entry = primary_entries + index * GPT_PARTITION_ENTRY_BYTES;
        gpt_partition_info_t *part = NULL;
        u32 part_index = 0;
        if (!gpt_entry_used(entry)) continue;
        if (discovered_count >= GPT_REGISTERED_MAX ||
            block_device_count() + discovered_count >= BLOCK_MAX_DEVICES) {
            discard_allocated_partitions(allocated_indices, allocated_count);
            return false;
        }
        for (u32 candidate = 0; candidate < partition_count; candidate++) {
            if (!partitions[candidate].parent) {
                part = &partitions[candidate];
                part_index = candidate;
                break;
            }
        }
        if (!part && partition_count < GPT_REGISTERED_MAX) {
            part_index = partition_count;
            part = &partitions[partition_count++];
        }
        if (!part) {
            discard_allocated_partitions(allocated_indices, allocated_count);
            return false;
        }
        memset(part, 0, sizeof(*part));
        allocated_indices[allocated_count++] = part_index;
        part->parent = device;
        part->first_lba = le64(entry + 32);
        part->last_lba = le64(entry + 40);
        part->number = index + 1U;
        memcpy(part->type_guid, entry, GPT_GUID_BYTES);
        memcpy(part->unique_guid, entry + 16, GPT_GUID_BYTES);
        for (u32 name_index = 0; name_index < 36U; name_index++) {
            u8 low = entry[56U + name_index * 2U];
            u8 high = entry[56U + name_index * 2U + 1U];
            if (low == 0U && high == 0U) break;
            if (high != 0U) {
                part->name[0] = '?';
                break;
            }
            if (name_index + 1U < GPT_PARTITION_NAME_MAX)
                part->name[name_index] = (char)low;
        }
        part->name[GPT_PARTITION_NAME_MAX - 1U] = '\0';
        discovered_indices[discovered_count++] = part_index;
    }
    for (u32 index = 0; index < discovered_count; index++) {
        gpt_partition_info_t *part = &partitions[discovered_indices[index]];
        block_device_t child;
        memset(&child, 0, sizeof(child));
        child.type = BLOCK_DEVICE_PARTITION;
        child.sector_size = device->sector_size;
        child.sector_count = part->last_lba - part->first_lba + 1ULL;
        child.read = partition_read;
        child.write = partition_write;
        child.flush = partition_flush;
        child.driver_data = part;
        child.read_only = device->read_only;
        if (!block_register(&child)) {
            discard_allocated_partitions(allocated_indices, allocated_count);
            return false;
        }
    }
    KERNEL_BOOT_DEBUG_LOG("[GPT] partition table discovered\n");
    return true;
}

int gpt_get_table_info(block_device_t *device, gpt_table_info_t *out_info)
{
    gpt_table_t table;
    int result;
    u8 mbr[GPT_SECTOR_BYTES];
    u8 primary[GPT_SECTOR_BYTES];

    if (!device || !out_info) return VFS_ERR_INVALID_PARAM;
    memset(out_info, 0, sizeof(*out_info));
    result = gpt_load(device, &table);
    if (result == VFS_OK) {
        *out_info = table.info;
        return VFS_OK;
    }
    if (result == VFS_ERR_IO) return result;
    if (device->sector_size != GPT_SECTOR_BYTES || device->sector_count < 2ULL ||
        !gpt_read(device, 0, 1U, mbr) || !gpt_read(device, 1, 1U, primary))
        return VFS_ERR_IO;
    out_info->type = gpt_signature(primary) ? GPT_TABLE_CORRUPT :
                       (mbr_has_partition(mbr) ? GPT_TABLE_MBR : GPT_TABLE_NONE);
    return VFS_OK;
}

int gpt_plan_partition(block_device_t *device, u64 requested_bytes,
                       bool use_rest, gpt_partition_plan_t *out_plan)
{
    gpt_table_t table;
    int result;

    if (!device || !out_plan || device->type == BLOCK_DEVICE_PARTITION)
        return VFS_ERR_INVALID_PARAM;
    if (storage_mutation_busy()) return VFS_ERR_BUSY;
    result = gpt_load(device, &table);
    if (result != VFS_OK) return result;
    if (!use_rest && !requested_bytes) return VFS_ERR_INVALID_PARAM;
    if (!plan_from_entries(&table, device, requested_bytes, use_rest, out_plan))
        return VFS_ERR_NO_SPACE;
    return VFS_OK;
}

int gpt_initialize_device(block_device_t *device)
{
    gpt_table_t table;

    if (!device || device->type == BLOCK_DEVICE_PARTITION)
        return VFS_ERR_INVALID_PARAM;
    if (!block_device_is_live(device)) return VFS_ERR_DEVICE_GONE;
    if (device->read_only) return VFS_ERR_ACCESS_DENIED;
    if (device->sector_size != GPT_SECTOR_BYTES ||
        device->sector_count < GPT_MIN_SECTORS) return VFS_ERR_UNSUPPORTED;
    if (!gpt_preflight_children(device)) return VFS_ERR_BUSY;
    if (!storage_mutation_begin()) return VFS_ERR_BUSY;
    memset(&table, 0, sizeof(table));
    table.info.type = GPT_TABLE_VALID;
    table.info.entry_count = GPT_MAX_PARTITION_ENTRIES;
    table.info.entry_size = GPT_PARTITION_ENTRY_BYTES;
    table.info.first_usable_lba = GPT_PRIMARY_ENTRIES_LBA +
                                  GPT_ENTRY_ARRAY_SECTORS;
    table.info.last_usable_lba = device->sector_count - 1ULL -
                                 GPT_ENTRY_ARRAY_SECTORS - 1ULL;
    table.primary_entries_lba = GPT_PRIMARY_ENTRIES_LBA;
    table.backup_entries_lba = device->sector_count - 1ULL -
                               GPT_ENTRY_ARRAY_SECTORS;
    table.array_sectors = GPT_ENTRY_ARRAY_SECTORS;
    if (table.info.first_usable_lba > table.info.last_usable_lba ||
        !generate_guid(table.disk_guid, device->id)) {
        storage_mutation_end();
        return VFS_ERR_IO;
    }
    memset(primary_entries, 0, sizeof(primary_entries));
    if (!gpt_write_protective_mbr(device) || !gpt_write_updated(device, &table)) {
        storage_mutation_end();
        return block_device_is_live(device) ? VFS_ERR_IO : VFS_ERR_DEVICE_GONE;
    }
    gpt_remove_partitions_for_parent(device);
    if (!gpt_scan_device(device)) {
        storage_mutation_end();
        return VFS_ERR_IO;
    }
    storage_mutation_end();
    return VFS_OK;
}

int gpt_create_partition(block_device_t *device, u32 number,
                         u64 first_lba, u64 last_lba)
{
    gpt_table_t table;
    gpt_partition_plan_t plan;
    u8 *entry;
    int result;
    u64 requested_bytes;

    if (!device || !number || number > GPT_MAX_PARTITION_ENTRIES ||
        first_lba > last_lba) return VFS_ERR_INVALID_PARAM;
    if (!block_device_is_live(device)) return VFS_ERR_DEVICE_GONE;
    if (device->read_only) return VFS_ERR_ACCESS_DENIED;
    if (!gpt_preflight_children(device)) return VFS_ERR_BUSY;
    if (!storage_mutation_begin()) return VFS_ERR_BUSY;
    result = gpt_load(device, &table);
    if (result != VFS_OK) {
        storage_mutation_end();
        return result;
    }
    if (!multiply_u64(last_lba - first_lba + 1ULL, device->sector_size,
                     &requested_bytes) ||
        !plan_from_entries(&table, device, requested_bytes, false, &plan) ||
        plan.partition_number != number || plan.first_lba != first_lba ||
        plan.last_lba != last_lba) {
        storage_mutation_end();
        return VFS_ERR_BUSY;
    }
    entry = primary_entries + (number - 1U) * GPT_PARTITION_ENTRY_BYTES;
    memset(entry, 0, GPT_PARTITION_ENTRY_BYTES);
    memcpy(entry, mangrove_data_type_guid, GPT_GUID_BYTES);
    if (!generate_guid(entry + 16, device->id ^ first_lba ^ last_lba)) {
        storage_mutation_end();
        return VFS_ERR_IO;
    }
    store_le64(entry + 32, first_lba);
    store_le64(entry + 40, last_lba);
    if (!gpt_write_updated(device, &table)) {
        storage_mutation_end();
        return block_device_is_live(device) ? VFS_ERR_IO : VFS_ERR_DEVICE_GONE;
    }
    gpt_remove_partitions_for_parent(device);
    if (!gpt_scan_device(device)) {
        storage_mutation_end();
        return VFS_ERR_IO;
    }
    storage_mutation_end();
    return VFS_OK;
}

int gpt_delete_partition(block_device_t *device, block_device_t *partition,
                         u32 number)
{
    gpt_table_t table;
    gpt_partition_info_t old;
    u8 *entry;
    int result;

    if (!device || !partition || !number ||
        partition->type != BLOCK_DEVICE_PARTITION ||
        !gpt_get_partition_info(partition, &old) ||
        !gpt_partition_parent_matches(&old, device) ||
        old.number != number) return VFS_ERR_INVALID_PARAM;
    if (!block_device_is_live(device) || !block_device_is_live(partition))
        return VFS_ERR_DEVICE_GONE;
    if (device->read_only) return VFS_ERR_ACCESS_DENIED;
    if (!gpt_preflight_children(device)) return VFS_ERR_BUSY;
    if (!storage_mutation_begin()) return VFS_ERR_BUSY;
    result = gpt_load(device, &table);
    if (result != VFS_OK) {
        storage_mutation_end();
        return result;
    }
    entry = primary_entries + (number - 1U) * GPT_PARTITION_ENTRY_BYTES;
    if (!gpt_entry_used(entry) || le64(entry + 32) != old.first_lba ||
        le64(entry + 40) != old.last_lba ||
        memcmp(entry + 16, old.unique_guid, GPT_GUID_BYTES) != 0) {
        storage_mutation_end();
        return VFS_ERR_DEVICE_GONE;
    }
    memset(entry, 0, GPT_PARTITION_ENTRY_BYTES);
    if (!gpt_write_updated(device, &table)) {
        storage_mutation_end();
        return block_device_is_live(device) ? VFS_ERR_IO : VFS_ERR_DEVICE_GONE;
    }
    gpt_remove_partitions_for_parent(device);
    if (!gpt_scan_device(device)) {
        storage_mutation_end();
        return VFS_ERR_IO;
    }
    storage_mutation_end();
    return VFS_OK;
}

bool gpt_get_partition_info(block_device_t *device,
                            gpt_partition_info_t *out_info)
{
    gpt_partition_info_t *part;

    if (!device || !out_info || device->type != BLOCK_DEVICE_PARTITION ||
        !device->driver_data) return false;
    part = (gpt_partition_info_t *)device->driver_data;
    *out_info = *part;
    return true;
}

bool gpt_partition_parent_matches(const gpt_partition_info_t *partition,
                                  const block_device_t *parent)
{
    return partition && partition->parent && parent &&
           partition->parent->id == parent->id;
}

bool gpt_partition_is_esp(const gpt_partition_info_t *info)
{
    return info && memcmp(info->type_guid, esp_type_guid, GPT_GUID_BYTES) == 0;
}

void gpt_mark_partitions_gone_for_parent(block_device_t *parent)
{
    if (!parent) return;
    for (u32 index = 0; index < block_device_count(); index++) {
        block_device_t *device = block_get_device(index);
        gpt_partition_info_t info;
        if (!device || device->type != BLOCK_DEVICE_PARTITION ||
            !gpt_get_partition_info(device, &info) ||
            !gpt_partition_parent_matches(&info, parent))
            continue;
        (void)block_device_mark_gone(device->id);
    }
}

void gpt_remove_partitions_for_parent(block_device_t *parent)
{
    bool removed;

    if (!parent) return;
    do {
        removed = false;
        for (u32 index = 0; index < block_device_count(); index++) {
            block_device_t *device = block_get_device(index);
            gpt_partition_info_t info;
            if (!device || device->type != BLOCK_DEVICE_PARTITION ||
                !gpt_get_partition_info(device, &info) ||
                !gpt_partition_parent_matches(&info, parent))
                continue;
            (void)block_device_mark_gone(device->id);
            (void)block_unregister_by_id(device->id);
            removed = true;
            break;
        }
    } while (removed);
    for (u32 index = 0; index < partition_count; index++)
        if (gpt_partition_parent_matches(&partitions[index], parent))
            memset(&partitions[index], 0, sizeof(partitions[index]));
}
