/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <storage/format.h>
#include <storage/format_internal.h>

#include <heap.h>
#include <mg/utf8.h>
#include <string.h>
#include <vfs.h>
#include <storage/fat32.h>
#include <storage/exfat.h>
#include <storage/gpt.h>

#define FORMAT_SECTOR_BYTES 512U

#define FAT32_RESERVED_SECTORS 32U
#define FAT32_FAT_COUNT        2U
#define FAT32_MIN_CLUSTERS     65525ULL
#define FAT32_MAX_CLUSTERS     0x0ffffff5ULL
/* Legacy BPB geometry is not used for addressing on modern media, but
 * non-zero conventional values are required by common interoperable tools. */
#define FAT32_BPB_SECTORS_PER_TRACK 63U
#define FAT32_BPB_HEADS             255U

#define MGFS_BLOCK_BYTES             4096U
#define MGFS_BLOCK_SECTORS           8ULL
#define MGFS_FORMAT_MAJOR            1ULL
#define MGFS_FORMAT_MINOR            2ULL
#define MGFS_HEADER_BYTES            200ULL
#define MGFS_RECORD_BYTES            192ULL
#define MGFS_RECORDS_PER_TABLE_BLOCK 21ULL
#define MGFS_BITMAP_HEADER_BYTES     24ULL
#define MGFS_BITMAP_BITS_PER_BLOCK   32576ULL
#define MGFS_MIN_TOTAL_BLOCKS        64ULL
#define MGFS_TABLE_BLOCKS_DIVISOR    320ULL
#define MGFS_ROOT_RECORD_ID          1ULL
#define MGFS_NEXT_RECORD_ID          2ULL
#define MGFS_INITIAL_GENERATION      1ULL
#define MGFS_STATE_CLEAN             1ULL
#define MGFS_METADATA_ALLOCATION_BITMAP 1ULL
#define MGFS_METADATA_RECORD_BITMAP     2ULL
#define MGFS_METADATA_RECORD_TABLE      3ULL
#define MGFS_RECORD_DIRECTORY            2ULL
#define MGFS_CRC64_POLYNOMIAL         0x42F0E1EBA9EA3693ULL

#define MGFS_LABEL_EXTENSION_OFFSET   200U
#define MGFS_LABEL_LENGTH_OFFSET      208U
#define MGFS_LABEL_DATA_OFFSET        216U
#define MGFS_LABEL_CHECKSUM_OFFSET    280U
#define MGFS_LABEL_BYTES              63U
#define MGFS_LABEL_EXTENSION_BYTES    88U

typedef struct {
    u64 allocation_bitmap_start;
    u64 allocation_bitmap_blocks;
    u64 record_bitmap_start;
    u64 record_bitmap_blocks;
    u64 record_table_start;
    u64 record_table_blocks;
    u64 record_count;
    u64 data_start;
    u64 data_blocks;
} mgfs_format_layout_t;

static bool storage_operation_in_progress;

bool storage_mutation_begin(void)
{
    bool expected = false;
    if (!__atomic_compare_exchange_n(&storage_operation_in_progress, &expected,
                                    true, false, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) return false;
    /* Nonblocking: a controller service worker must still process events
     * needed by the current I/O owner. No spinlock spans this operation. */
    if (!block_io_begin_quiesce()) {
        __atomic_store_n(&storage_operation_in_progress, false, __ATOMIC_RELEASE);
        return false;
    }
    return true;
}

void storage_mutation_end(void)
{
    /* All management work is complete; the block gate still protects shared
     * buffers. Clear admission before its FIFO handoff, so a granted worker
     * cannot mistake this completed operation for ongoing management. Other
     * contenders must still acquire quiescence before touching those buffers. */
    __atomic_store_n(&storage_operation_in_progress, false, __ATOMIC_RELEASE);
    block_io_end_quiesce();
}

bool storage_mutation_busy(void)
{
    return __atomic_load_n(&storage_operation_in_progress, __ATOMIC_ACQUIRE);
}

int storage_management_range_preflight(block_device_t *device, u64 expected_id,
                                        u64 lba, u64 count)
{
    block_device_t target;
    block_physical_range_t range;
    if (!storage_mutation_busy()) return VFS_ERR_BUSY;
    if (!block_device_snapshot(device, &target) || target.id != expected_id ||
        !block_device_id_is_live(expected_id)) return VFS_ERR_DEVICE_GONE;
    if (target.read_only) return VFS_ERR_ACCESS_DENIED;
    if (!count || lba >= target.sector_count || count > target.sector_count - lba)
        return VFS_ERR_INVALID_PARAM;
    if (!block_device_physical_range(device, expected_id, &range))
        return VFS_ERR_UNSUPPORTED;
    u64 first = range.first_lba + lba;
    u64 last = first + count - 1U;
    for (u32 i = 0; i < block_device_count(); i++) {
        block_device_t *other = block_get_device(i), snapshot;
        block_physical_range_t alias;
        if (!other || !block_device_snapshot(other, &snapshot)) return VFS_ERR_BUSY;
        if (snapshot.id == expected_id) continue;
        if (target.type == BLOCK_DEVICE_PARTITION && snapshot.id == range.storage_id) {
            if (!block_device_id_is_live(snapshot.id)) return VFS_ERR_DEVICE_GONE;
            if (snapshot.read_only) return VFS_ERR_ACCESS_DENIED;
            continue;
        }
        if (snapshot.type != BLOCK_DEVICE_PARTITION && snapshot.id != range.storage_id)
            continue;
        /* Retiring registrations still own backing state. Establish their
         * parent identity before ignoring an unrelated physical device. */
        if (snapshot.type == BLOCK_DEVICE_PARTITION) {
            gpt_partition_info_t part;
            block_device_t parent;
            if (!gpt_get_partition_info(other, &part) ||
                !block_device_snapshot(part.parent, &parent)) return VFS_ERR_BUSY;
            if (parent.id != range.storage_id) continue;
        }
        if (!block_device_physical_range(other, snapshot.id, &alias)) return VFS_ERR_BUSY;
        if (alias.storage_id == range.storage_id && first <= alias.last_lba &&
            alias.first_lba <= last) return VFS_ERR_BUSY;
    }
    return VFS_OK;
}

/* Largest existing raw-format request: one exFAT boot region (12 sectors).
 * A failed destructive format has no rollback: retain the last attempted
 * image, progress, filesystem selection and the complete physical-scope pin. */
#define FORMAT_PENDING_SECTORS 12U
typedef enum {
    FORMAT_PREPARING, FORMAT_WRITING, FORMAT_FLUSHING, FORMAT_VERIFYING,
    FORMAT_COMPLETE,
} storage_format_phase_t;
struct storage_format_context {
    block_device_t *device;
    block_device_t snapshot;
    vfs_super_t *owner;
    const char *filesystem;
    u64 pin, attempted_lba, write_attempts;
    u32 attempted_sectors;
    storage_format_phase_t phase;
    int failure;
    bool mutated, resolved, pending_written;
    u8 pending[FORMAT_PENDING_SECTORS * FORMAT_SECTOR_BYTES];
};

static bool storage_format_identity_live(const storage_format_context_t *context)
{
    block_device_t snapshot;
    return context && block_device_snapshot(context->device, &snapshot) &&
           snapshot.id == context->snapshot.id &&
           block_device_id_is_live(context->snapshot.id);
}

const block_device_t *storage_format_target(const storage_format_context_t *context)
{
    return context ? &context->snapshot : NULL;
}

bool storage_format_read(storage_format_context_t *context, u64 lba,
                          u32 count, void *buffer)
{
    if (!context || !buffer || !count || !storage_format_identity_live(context) ||
        lba >= context->snapshot.sector_count ||
        count > context->snapshot.sector_count - lba) return false;
    return block_read_for_id(context->device, context->snapshot.id, lba, count, buffer);
}

bool storage_format_write(storage_format_context_t *context, u64 lba,
                           u32 count, const void *buffer)
{
    if (!context || !buffer || !count || count > FORMAT_PENDING_SECTORS ||
        !storage_format_identity_live(context) || context->device->read_only ||
        lba >= context->snapshot.sector_count ||
        count > context->snapshot.sector_count - lba) return false;
    memcpy(context->pending, buffer, (usize)count * FORMAT_SECTOR_BYTES);
    context->attempted_lba = lba;
    context->attempted_sectors = count;
    context->write_attempts++;
    context->mutated = true; /* A false callback may already have written bytes. */
    context->phase = FORMAT_WRITING;
    context->pending_written = block_write_for_id(context->device,
        context->snapshot.id, lba, count, buffer);
    return context->pending_written;
}

static int storage_format_owner_unmount(vfs_super_t *sb)
{
    kfree(sb->private_data);
    sb->private_data = NULL;
    return VFS_OK;
}

static const vfs_super_ops_t storage_format_owner_ops = {
    .unmount = storage_format_owner_unmount,
};

static bool storage_format_finish(vfs_super_t *sb, void *opaque)
{
    storage_format_context_t *context = opaque;
    return sb->private_data == context && context->resolved;
}

int storage_format_execute_for_id(block_device_t *device, u64 expected_id,
                                  const char *filesystem, storage_format_writer_t writer)
{
    if (!device || !filesystem || !writer) return VFS_ERR_INVALID_PARAM;
    /* Store a static selection, never retain a caller's transient string. */
    const char *selected = !strcmp(filesystem, "fat32") ? "fat32" :
        !strcmp(filesystem, "exfat") ? "exfat" :
        !strcmp(filesystem, "mgfs") ? "mgfs" : NULL;
    if (!selected) return VFS_ERR_UNSUPPORTED;
    if (!storage_mutation_begin()) return VFS_ERR_BUSY;
    int result = VFS_ERR_DEVICE_GONE;
    block_device_t snapshot;
    if (!block_device_snapshot(device, &snapshot) || snapshot.id != expected_id ||
        !block_device_id_is_live(expected_id)) goto end;
    if (snapshot.read_only) { result = VFS_ERR_ACCESS_DENIED; goto end; }
    if (snapshot.sector_size != FORMAT_SECTOR_BYTES) {
        result = VFS_ERR_UNSUPPORTED; goto end;
    }
    result = storage_management_range_preflight(device, expected_id, 0,
                                                 snapshot.sector_count);
    if (result != VFS_OK) goto end;
    storage_format_context_t *context = kmalloc(sizeof(*context));
    if (!context) { result = VFS_ERR_NO_MEM; goto end; }
    memset(context, 0, sizeof(*context));
    context->device = device;
    context->snapshot = snapshot;
    context->filesystem = selected;
    vfs_super_t *owner;
    u64 pin;
    result = vfs_recovery_private_begin(device, expected_id, &storage_format_owner_ops,
        storage_format_finish, context, &owner, &pin);
    if (result != VFS_OK) { kfree(context); goto end; }
    owner->private_data = context;
    context->owner = owner;
    context->pin = pin;
    if (!mutex_lock(&owner->operation_lock)) {
        result = VFS_ERR_IO;
    } else {
        /* Admission may have failed/lost the registration while preparing
         * the private owner. No loading or writer call precedes this check. */
        result = !storage_format_identity_live(context) ? VFS_ERR_DEVICE_GONE :
            device->read_only ? VFS_ERR_ACCESS_DENIED : VFS_ERR_IO;
        if (result == VFS_ERR_IO && writer(context)) {
            context->phase = FORMAT_FLUSHING;
            if (block_flush_for_id(device, expected_id) &&
                storage_format_identity_live(context)) {
                context->phase = FORMAT_VERIFYING;
                const char *probed = vfs_probe_filesystem_for_id(device, expected_id);
                if (probed && !strcmp(probed, selected) &&
                    storage_format_identity_live(context)) result = VFS_OK;
            }
        }
        if (!context->mutated && !storage_format_identity_live(context))
            result = VFS_ERR_DEVICE_GONE;
        context->failure = result;
        (void)mutex_unlock(&owner->operation_lock);
    }
    if (result != VFS_OK && context->mutated) {
        (void)vfs_recovery_pin_mark_unresolved(pin);
        result = VFS_ERR_UNCERTAIN;
    } else {
        context->resolved = true; /* Completion, or confirmed no-write cancellation. */
        if (result == VFS_OK) context->phase = FORMAT_COMPLETE;
        if (vfs_recovery_pin_finish_handoff(pin) != VFS_OK) {
            (void)vfs_recovery_pin_mark_unresolved(pin);
            result = VFS_ERR_UNCERTAIN;
        }
    }
    vfs_recovery_private_release(owner);
end:
    storage_mutation_end();
    return result;
}

static void store_le16(u8 *data, u16 value)
{
    data[0] = (u8)value;
    data[1] = (u8)(value >> 8);
}

static void store_le32(u8 *data, u32 value)
{
    for (u32 index = 0; index < 4U; index++)
        data[index] = (u8)(value >> (index * 8U));
}

static void store_le64(u8 *data, u64 value)
{
    for (u32 index = 0; index < 8U; index++)
        data[index] = (u8)(value >> (index * 8U));
}

static u64 crc64(const u8 *data, usize length, u32 zero_offset)
{
    u64 crc = 0;

    for (usize index = 0; index < length; index++) {
        u8 value = (zero_offset != ~(u32)0U && index >= zero_offset &&
                    index - zero_offset < 8U)
            ? 0U : data[index];
        crc ^= (u64)value << 56U;
        for (u32 bit = 0; bit < 8U; bit++)
            crc = (crc & 0x8000000000000000ULL)
                ? (crc << 1U) ^ MGFS_CRC64_POLYNOMIAL
                : crc << 1U;
    }
    return crc;
}

static bool write_zero_sectors(storage_format_context_t *context, u64 lba, u64 count)
{
    u8 zero[MGFS_BLOCK_BYTES];

    memset(zero, 0, sizeof(zero));
    while (count) {
        u32 batch = count > 8ULL ? 8U : (u32)count;
        if (!storage_format_write(context, lba, batch, zero)) return false;
        if (lba > ~(u64)0 - batch) return false;
        lba += batch;
        count -= batch;
    }
    return true;
}

static bool calculate_mgfs_layout(u64 total_blocks,
                                  mgfs_format_layout_t *layout)
{
    u64 table_blocks;
    u64 record_count;
    u64 record_bitmap_blocks;
    u64 allocation_bitmap_blocks;
    u64 data_start;

    if (!layout || total_blocks < MGFS_MIN_TOTAL_BLOCKS) return false;
    table_blocks = total_blocks / MGFS_TABLE_BLOCKS_DIVISOR;
    if (!table_blocks) table_blocks = 1ULL;
    if (table_blocks > ~(u64)0 / MGFS_RECORDS_PER_TABLE_BLOCK) return false;
    record_count = table_blocks * MGFS_RECORDS_PER_TABLE_BLOCK;
    if (record_count > ~(u64)0 - (MGFS_BITMAP_BITS_PER_BLOCK - 1ULL) ||
        total_blocks > ~(u64)0 - (MGFS_BITMAP_BITS_PER_BLOCK - 1ULL))
        return false;
    record_bitmap_blocks = (record_count + MGFS_BITMAP_BITS_PER_BLOCK - 1ULL) /
                           MGFS_BITMAP_BITS_PER_BLOCK;
    allocation_bitmap_blocks = (total_blocks + MGFS_BITMAP_BITS_PER_BLOCK - 1ULL) /
                               MGFS_BITMAP_BITS_PER_BLOCK;
    if (allocation_bitmap_blocks > ~(u64)0 - 1ULL ||
        record_bitmap_blocks > ~(u64)0 - 1ULL - allocation_bitmap_blocks ||
        table_blocks > ~(u64)0 - 1ULL - allocation_bitmap_blocks -
                         record_bitmap_blocks) return false;
    data_start = 1ULL + allocation_bitmap_blocks + record_bitmap_blocks +
                 table_blocks;
    if (data_start >= total_blocks) return false;
    layout->allocation_bitmap_start = 1ULL;
    layout->allocation_bitmap_blocks = allocation_bitmap_blocks;
    layout->record_bitmap_start = 1ULL + allocation_bitmap_blocks;
    layout->record_bitmap_blocks = record_bitmap_blocks;
    layout->record_table_start = layout->record_bitmap_start +
                                 record_bitmap_blocks;
    layout->record_table_blocks = table_blocks;
    layout->record_count = record_count;
    layout->data_start = data_start;
    layout->data_blocks = total_blocks - data_start;
    return true;
}

static bool format_owned_block_write(storage_format_context_t *context,
                                      u64 block_number, const void *buffer)
{
    return block_number <= ~(u64)0 / MGFS_BLOCK_SECTORS &&
        storage_format_write(context, block_number * MGFS_BLOCK_SECTORS, 8U, buffer);
}

static bool format_mgfs(storage_format_context_t *context)
{
    const block_device_t *device = storage_format_target(context);
    mgfs_format_layout_t layout;
    u8 block[MGFS_BLOCK_BYTES];
    u64 total_blocks;
    u32 root_owner;
    u32 root_permissions;
    u64 root_flags;

    if (!device || device->sector_size != FORMAT_SECTOR_BYTES) return false;
    /* A runtime-created removable MGFS volume belongs to the authenticated
     * formatting process.  Reusing the image-builder's system-owned root
     * would make a freshly formatted removable volume read-only to its
     * normal user despite a successful format. */
    if (!vfs_current_uid(&root_owner)) return false;
    root_permissions = root_owner == VFS_UID_SYSTEM
        ? VFS_DEFAULT_SYSTEM_PERMISSIONS : VFS_DEFAULT_DIRECTORY_PERMISSIONS;
    root_flags = ((u64)root_owner << 1U) |
                 ((u64)root_permissions << 33U);
    /* GPT's final usable LBA is not necessarily 4 KiB aligned.  MGFS owns
     * complete 4 KiB blocks, so retain at most seven harmless trailing
     * sectors inside the selected extent rather than rejecting an otherwise
     * valid partition or ever writing past its boundary. */
    total_blocks = device->sector_count / MGFS_BLOCK_SECTORS;
    if (!calculate_mgfs_layout(total_blocks, &layout)) return false;

    for (u64 index = 0; index < layout.allocation_bitmap_blocks; index++) {
        u64 first_bit = index * MGFS_BITMAP_BITS_PER_BLOCK;
        u64 valid_bits = first_bit < layout.data_blocks
            ? layout.data_blocks - first_bit : 0ULL;
        if (valid_bits > MGFS_BITMAP_BITS_PER_BLOCK)
            valid_bits = MGFS_BITMAP_BITS_PER_BLOCK;
        memset(block, 0, sizeof(block));
        store_le64(block, MGFS_METADATA_ALLOCATION_BITMAP);
        store_le64(block + 8, index);
        memset(block + MGFS_BITMAP_HEADER_BYTES, 0xff,
               sizeof(block) - MGFS_BITMAP_HEADER_BYTES);
        for (u64 bit = 0; bit < valid_bits; bit++)
            block[MGFS_BITMAP_HEADER_BYTES + bit / 8ULL] &=
                (u8)~(1U << (bit % 8ULL));
        store_le64(block + 16, crc64(block, sizeof(block), 16U));
        if (layout.allocation_bitmap_start > ~(u64)0 - index ||
            !format_owned_block_write(context, layout.allocation_bitmap_start + index,
                                block)) return false;
    }

    for (u64 index = 0; index < layout.record_bitmap_blocks; index++) {
        u64 first_bit = index * MGFS_BITMAP_BITS_PER_BLOCK;
        u64 valid_bits = first_bit < layout.record_count
            ? layout.record_count - first_bit : 0ULL;
        if (valid_bits > MGFS_BITMAP_BITS_PER_BLOCK)
            valid_bits = MGFS_BITMAP_BITS_PER_BLOCK;
        memset(block, 0, sizeof(block));
        store_le64(block, MGFS_METADATA_RECORD_BITMAP);
        store_le64(block + 8, index);
        memset(block + MGFS_BITMAP_HEADER_BYTES, 0xff,
               sizeof(block) - MGFS_BITMAP_HEADER_BYTES);
        for (u64 bit = 0; bit < valid_bits; bit++)
            block[MGFS_BITMAP_HEADER_BYTES + bit / 8ULL] &=
                (u8)~(1U << (bit % 8ULL));
        if (!index) block[MGFS_BITMAP_HEADER_BYTES] |= 1U;
        store_le64(block + 16, crc64(block, sizeof(block), 16U));
        if (layout.record_bitmap_start > ~(u64)0 - index ||
            !format_owned_block_write(context, layout.record_bitmap_start + index,
                                block)) return false;
    }

    for (u64 index = 0; index < layout.record_table_blocks; index++) {
        memset(block, 0, sizeof(block));
        store_le64(block, MGFS_METADATA_RECORD_TABLE);
        store_le64(block + 8, index);
        if (!index) {
            u8 *root = block + MGFS_BITMAP_HEADER_BYTES;
            store_le64(root, MGFS_RECORD_DIRECTORY);
            store_le64(root + 8, root_flags);
            store_le64(root + 16, MGFS_ROOT_RECORD_ID);
            store_le64(root + 24, MGFS_INITIAL_GENERATION);
            store_le64(root + 184, crc64(root, MGFS_RECORD_BYTES, 184U));
        }
        store_le64(block + 16, crc64(block, sizeof(block), 16U));
        if (layout.record_table_start > ~(u64)0 - index ||
            !format_owned_block_write(context, layout.record_table_start + index,
                                block)) return false;
    }

    memset(block, 0, sizeof(block));
    {
        static const u8 magic[8] = {'M','G','F','S','v','1',0,0};
        memcpy(block, magic, sizeof(magic));
        store_le64(block + 8, MGFS_FORMAT_MAJOR);
        store_le64(block + 16, MGFS_FORMAT_MINOR);
        store_le64(block + 24, MGFS_HEADER_BYTES);
        store_le64(block + 32, MGFS_BLOCK_BYTES);
        store_le64(block + 40, total_blocks);
        store_le64(block + 64, MGFS_STATE_CLEAN);
        store_le64(block + 88, MGFS_ROOT_RECORD_ID);
        store_le64(block + 96, MGFS_NEXT_RECORD_ID);
        store_le64(block + 104, layout.allocation_bitmap_start);
        store_le64(block + 112, layout.allocation_bitmap_blocks);
        store_le64(block + 120, layout.record_bitmap_start);
        store_le64(block + 128, layout.record_bitmap_blocks);
        store_le64(block + 136, layout.record_table_start);
        store_le64(block + 144, layout.record_table_blocks);
        store_le64(block + 152, layout.record_count);
        store_le64(block + 160, layout.data_start);
        store_le64(block + 168, layout.data_blocks);
        /* An empty label is represented by an all-zero optional extension. */
        store_le64(block + 192, crc64(block, MGFS_HEADER_BYTES, 192U));
    }
    if (!format_owned_block_write(context, 0, block)) return false;
    if (layout.data_start > ~(u64)0 / MGFS_BLOCK_SECTORS) return false;
    return write_zero_sectors(context, layout.data_start * MGFS_BLOCK_SECTORS,
                              8ULL);
}

static u32 fat32_fat_size(u32 total_sectors, u32 sectors_per_cluster,
                          u64 *out_clusters)
{
    u64 fat_size = 1ULL;
    u64 clusters = 0;

    /* The integer equation can oscillate by one sector on small volumes:
     * calculating a smaller FAT exposes another cluster, which in turn
     * needs the larger FAT again.  Grow until the current FAT can hold all
     * entries for the geometry it creates; never select the smaller side. */
    for (u32 iteration = 0; iteration < 64U; iteration++) {
        u64 overhead = FAT32_RESERVED_SECTORS +
                       (u64)FAT32_FAT_COUNT * fat_size;
        u64 data_sectors;
        u64 next_size;
        if (overhead >= total_sectors) return 0;
        data_sectors = total_sectors - overhead;
        clusters = data_sectors / sectors_per_cluster;
        next_size = ((clusters + 2ULL) * 4ULL + FORMAT_SECTOR_BYTES - 1ULL) /
                    FORMAT_SECTOR_BYTES;
        if (next_size <= fat_size) {
            if (!clusters || clusters < FAT32_MIN_CLUSTERS ||
                clusters > FAT32_MAX_CLUSTERS || fat_size > 0xffffffffULL)
                return 0;
            if (out_clusters) *out_clusters = clusters;
            return (u32)fat_size;
        }
        fat_size = next_size;
    }
    return 0;
}

static bool format_fat32(storage_format_context_t *context)
{
    const block_device_t *device = storage_format_target(context);
    u32 total_sectors;
    u32 sectors_per_cluster = 1U;
    u32 fat_size;
    u64 clusters = 0;
    u64 data_start;
    u8 sector[FORMAT_SECTOR_BYTES];
    u8 fsinfo[FORMAT_SECTOR_BYTES];

    if (!device || device->sector_size != FORMAT_SECTOR_BYTES ||
        device->sector_count > 0xffffffffULL || device->sector_count < 2ULL)
        return false;
    total_sectors = (u32)device->sector_count;
    for (;;) {
        fat_size = fat32_fat_size(total_sectors, sectors_per_cluster,
                                  &clusters);
        if (fat_size || sectors_per_cluster >= 64U) break;
        sectors_per_cluster <<= 1U;
    }
    if (!fat_size) return false;
    data_start = FAT32_RESERVED_SECTORS +
                 (u64)FAT32_FAT_COUNT * fat_size;
    if (data_start >= device->sector_count ||
        clusters > 0xffffffffULL || data_start + sectors_per_cluster >=
        device->sector_count) return false;

    memset(sector, 0, sizeof(sector));
    sector[0] = 0xeb; sector[1] = 0x58; sector[2] = 0x90;
    memcpy(sector + 3, "MANGROVE", 8);
    store_le16(sector + 11, FORMAT_SECTOR_BYTES);
    sector[13] = (u8)sectors_per_cluster;
    store_le16(sector + 14, FAT32_RESERVED_SECTORS);
    sector[16] = FAT32_FAT_COUNT;
    sector[21] = 0xf8;
    store_le16(sector + 24, FAT32_BPB_SECTORS_PER_TRACK);
    store_le16(sector + 26, FAT32_BPB_HEADS);
    store_le32(sector + 32, total_sectors);
    store_le32(sector + 36, fat_size);
    store_le32(sector + 44, 2U);
    store_le16(sector + 48, 1U);
    store_le16(sector + 50, 6U);
    sector[64] = 0x80;
    sector[66] = 0x29;
    store_le32(sector + 67, (u32)device->id);
    memcpy(sector + 71, "NO NAME    ", 11U);
    memcpy(sector + 82, "FAT32   ", 8U);
    sector[510] = 0x55; sector[511] = 0xaa;
    if (!storage_format_write(context, 0, 1U, sector) ||
        !storage_format_write(context, 6, 1U, sector)) return false;

    memset(fsinfo, 0, sizeof(fsinfo));
    store_le32(fsinfo, 0x41615252U);
    store_le32(fsinfo + 484, 0x61417272U);
    store_le32(fsinfo + 488, (u32)(clusters - 1ULL));
    store_le32(fsinfo + 492, 3U);
    store_le32(fsinfo + 508, 0xaa550000U);
    if (!storage_format_write(context, 1, 1U, fsinfo) ||
        !storage_format_write(context, 7, 1U, fsinfo)) return false;

    if (!write_zero_sectors(context, FAT32_RESERVED_SECTORS,
                            (u64)FAT32_FAT_COUNT * fat_size)) return false;
    memset(sector, 0, sizeof(sector));
    store_le32(sector, 0x0ffffff8U);
    store_le32(sector + 4, 0xffffffffU);
    store_le32(sector + 8, 0x0fffffffU);
    if (!storage_format_write(context, FAT32_RESERVED_SECTORS, 1U, sector) ||
        !storage_format_write(context, FAT32_RESERVED_SECTORS + fat_size, 1U, sector))
        return false;
    return write_zero_sectors(context, data_start, sectors_per_cluster);
}

static bool utf8_mgfs_label_valid(const char *label)
{
    usize length;
    usize offset = 0;
    u32 first = 0;
    u32 last = 0;
    bool content = false;

    if (!label || (length = strlen(label)) > MGFS_LABEL_BYTES) return false;
    if (!length) return true;
    while (offset < length) {
        u32 codepoint;
        usize consumed;
        if (mg_utf8_decode_one((const u8 *)label + offset, length - offset,
                               &codepoint, &consumed) != MG_UTF8_OK ||
            !consumed || codepoint == 0U) return false;
        offset += consumed;
        if (codepoint < 0x20U || (codepoint >= 0x7fU && codepoint <= 0x9fU) ||
            (codepoint >= 0x1f000U && codepoint <= 0x1ffffU) ||
            (codepoint >= 0x2000U && codepoint <= 0x2fffU) ||
            (codepoint >= 0xfe00U && codepoint <= 0xfeffU) ||
            (codepoint >= 0xe0000U && codepoint <= 0xe0fffU) ||
            (codepoint >= 0x202aU && codepoint <= 0x202eU) ||
            (codepoint >= 0x2066U && codepoint <= 0x2069U)) return false;
        if (!first) first = codepoint;
        last = codepoint;
        if (codepoint != 0x20U && codepoint != 0xa0U &&
            !(codepoint >= 0x2000U && codepoint <= 0x200aU) &&
            codepoint != 0x2028U && codepoint != 0x2029U &&
            codepoint != 0x202fU && codepoint != 0x205fU &&
            codepoint != 0x3000U)
            content = true;
    }
    return content && first != 0x20U && first != 0xa0U &&
           !(first >= 0x2000U && first <= 0x200aU) &&
           first != 0x2028U && first != 0x2029U && first != 0x202fU &&
           first != 0x205fU && first != 0x3000U && last != 0x20U &&
           last != 0xa0U && !(last >= 0x2000U && last <= 0x200aU) &&
           last != 0x2028U && last != 0x2029U && last != 0x202fU &&
           last != 0x205fU && last != 0x3000U;
}

/* One superblock update, with no rollback or retry after attempted mutation.
 * Reuse the formatter's private-owner disposer and registry admission. The
 * whole selected physical range remains excluded while evidence is retained. */
typedef struct {
    block_device_t *device;
    block_device_t snapshot;
    u64 pin;
    bool original_read, proposed_ready, write_attempted, write_confirmed;
    bool flush_attempted, flush_confirmed, readback_attempted, readback_confirmed;
    bool resolved, no_op, handoff_attempted;
    int failure, handoff_result;
    u8 original[MGFS_BLOCK_BYTES];
    u8 proposed[MGFS_BLOCK_BYTES];
    u8 observed[MGFS_BLOCK_BYTES];
} mgfs_label_transaction_t;

static bool mgfs_label_identity_live(const mgfs_label_transaction_t *tx)
{
    block_device_t snapshot;
    block_physical_range_t range;
    return block_device_snapshot(tx->device, &snapshot) &&
        snapshot.id == tx->snapshot.id && block_device_id_is_live(snapshot.id) &&
        block_device_physical_range(tx->device, snapshot.id, &range);
}

static bool mgfs_label_finish(vfs_super_t *sb, void *opaque)
{
    mgfs_label_transaction_t *tx = opaque;
    if (sb->private_data != tx || !tx->resolved) return false;
    if (!tx->write_attempted) return true; /* Confirmed no-write cancellation/no-op. */
    return tx->original_read && tx->proposed_ready && tx->write_confirmed &&
        tx->flush_attempted && tx->flush_confirmed && tx->readback_attempted &&
        tx->readback_confirmed && mgfs_label_identity_live(tx);
}

static int storage_set_filesystem_label_guarded(block_device_t *device,
                                                 u64 expected_id,
                                                 const char *filesystem,
                                                 const char *label)
{
    block_device_t snapshot;
    if (strcmp(filesystem, "mgfs")) return VFS_ERR_UNSUPPORTED;
    if (!utf8_mgfs_label_valid(label)) return VFS_ERR_INVALID_PARAM;
    if (!block_device_snapshot(device, &snapshot) || snapshot.id != expected_id ||
        !block_device_id_is_live(expected_id)) return VFS_ERR_DEVICE_GONE;
    if (snapshot.read_only) return VFS_ERR_ACCESS_DENIED;
    if (snapshot.sector_size != FORMAT_SECTOR_BYTES) return VFS_ERR_UNSUPPORTED;
    int result = storage_management_range_preflight(device, expected_id, 0,
                                                     MGFS_BLOCK_SECTORS);
    if (result != VFS_OK) return result;
    mgfs_label_transaction_t *tx = kmalloc(sizeof(*tx));
    if (!tx) return VFS_ERR_NO_MEM;
    memset(tx, 0, sizeof(*tx));
    tx->device = device;
    tx->snapshot = snapshot;
    vfs_super_t *owner;
    u64 pin;
    result = vfs_recovery_private_begin(device, expected_id, &storage_format_owner_ops,
                                         mgfs_label_finish, tx, &owner, &pin);
    if (result != VFS_OK) { kfree(tx); return result; }
    tx->pin = pin;
    owner->private_data = tx;
    result = VFS_ERR_IO;
    if (mutex_lock(&owner->operation_lock)) {
        if (!mgfs_label_identity_live(tx)) { result = VFS_ERR_DEVICE_GONE; goto unlock; }
        if (!block_read_for_id(device, expected_id, 0, MGFS_BLOCK_SECTORS, tx->original))
            goto unlock;
        tx->original_read = true;
        const char *probed = vfs_probe_filesystem_for_id(device, expected_id);
        if (!probed || strcmp(probed, "mgfs")) { result = VFS_ERR_BAD_FORMAT; goto unlock; }
        memcpy(tx->proposed, tx->original, MGFS_BLOCK_BYTES);
        memset(tx->proposed + MGFS_LABEL_EXTENSION_OFFSET, 0, MGFS_LABEL_EXTENSION_BYTES);
        usize length = strlen(label);
        if (length) {
            memcpy(tx->proposed + MGFS_LABEL_EXTENSION_OFFSET, "MGLABEL1", 8U);
            store_le64(tx->proposed + MGFS_LABEL_LENGTH_OFFSET, length);
            memcpy(tx->proposed + MGFS_LABEL_DATA_OFFSET, label, length);
            store_le64(tx->proposed + MGFS_LABEL_CHECKSUM_OFFSET,
                crc64(tx->proposed + MGFS_LABEL_EXTENSION_OFFSET, MGFS_LABEL_EXTENSION_BYTES, 80U));
        }
        tx->proposed_ready = true;
        if (!mgfs_label_identity_live(tx)) { result = VFS_ERR_DEVICE_GONE; goto unlock; }
        if (device->read_only) { result = VFS_ERR_ACCESS_DENIED; goto unlock; }
        if (!memcmp(tx->original, tx->proposed, MGFS_BLOCK_BYTES)) {
            tx->no_op = true;
            result = VFS_OK;
            goto unlock;
        }
        tx->write_attempted = true; /* False can mean partial/torn media changes. */
        tx->write_confirmed = block_write_for_id(device, expected_id, 0,
                                                 MGFS_BLOCK_SECTORS, tx->proposed);
        if (!tx->write_confirmed) goto unlock;
        tx->flush_attempted = true;
        tx->flush_confirmed = block_flush_for_id(device, expected_id);
        if (!tx->flush_confirmed) goto unlock;
        /* The write-through block cache invalidates every overlapping alias
         * before an attempted write, including a failed callback. Readback
         * cannot be satisfied from the original cached superblock. */
        tx->readback_attempted = true;
        tx->readback_confirmed = block_read_for_id(device, expected_id, 0,
            MGFS_BLOCK_SECTORS, tx->observed) &&
            !memcmp(tx->observed, tx->proposed, MGFS_BLOCK_BYTES) &&
            mgfs_label_identity_live(tx);
        if (tx->readback_confirmed) result = VFS_OK;
    unlock:
        if (!tx->write_attempted && !mgfs_label_identity_live(tx)) result = VFS_ERR_DEVICE_GONE;
        (void)mutex_unlock(&owner->operation_lock);
    }
    tx->failure = result;
    if (tx->write_attempted && result != VFS_OK) {
        (void)vfs_recovery_pin_mark_unresolved(pin);
        result = VFS_ERR_UNCERTAIN;
    } else {
        tx->resolved = true;
        tx->handoff_attempted = true;
        tx->handoff_result = vfs_recovery_pin_finish_handoff(pin);
        if (tx->handoff_result != VFS_OK) {
            tx->failure = tx->handoff_result;
            (void)vfs_recovery_pin_mark_unresolved(pin);
            result = VFS_ERR_UNCERTAIN;
        }
    }
    vfs_recovery_private_release(owner);
    return result;
}

int storage_format_filesystem_for_id(block_device_t *device, u64 expected_id,
                                     const char *filesystem)
{
    if (!device || !filesystem) return VFS_ERR_INVALID_PARAM;
    if (!strcmp(filesystem, "exfat"))
        return exfat_format_device_for_id(device, expected_id);
    if (strcmp(filesystem, "fat32") && strcmp(filesystem, "mgfs"))
        return VFS_ERR_UNSUPPORTED;
    return storage_format_execute_for_id(device, expected_id, filesystem,
        !strcmp(filesystem, "fat32") ? format_fat32 : format_mgfs);
}

int storage_set_filesystem_label_for_id(block_device_t *device, u64 expected_id,
                                        const char *filesystem, const char *label)
{
    if (!device || !filesystem || !label) return VFS_ERR_INVALID_PARAM;
    /* FAT32/exFAT acquire the same management/quiescence guard at their public
     * entries, including direct kernel callers. The retained owner controls
     * completion; append no generic I/O after that owner is released. */
    if (filesystem && strcmp(filesystem, "exfat") == 0)
        return exfat_set_label_for_id(device, expected_id, label);
    if (filesystem && strcmp(filesystem, "fat32") == 0)
        return fat32_set_label_for_id(device, expected_id, label);
    int result;
    if (!storage_mutation_begin()) return VFS_ERR_BUSY;
    block_device_t snapshot;
    result = block_device_snapshot(device, &snapshot) && snapshot.id == expected_id
        ? storage_set_filesystem_label_guarded(device, expected_id, filesystem, label) : VFS_ERR_DEVICE_GONE;
    storage_mutation_end();
    return result;
}

int storage_set_filesystem_label(block_device_t *device,
                                 const char *filesystem, const char *label)
{
    return storage_set_filesystem_label_for_id(device, device ? device->id : 0U,
                                               filesystem, label);
}
