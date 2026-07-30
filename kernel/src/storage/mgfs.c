#include <storage/mgfs.h>

#include <block.h>
#include <heap.h>
#include <string.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#define MGFS_BLOCK_BYTES             4096U
#define MGFS_FORMAT_MAJOR            1ULL
#define MGFS_FORMAT_MINOR            0ULL
#define MGFS_HEADER_BYTES            200ULL
#define MGFS_RECORD_BYTES            192U
#define MGFS_RECORDS_PER_TABLE_BLOCK 21ULL
#define MGFS_BITMAP_HEADER_BYTES     24U
#define MGFS_BITMAP_BITS_PER_BLOCK   32576ULL
#define MGFS_MIN_TOTAL_BLOCKS        64ULL
#define MGFS_TABLE_BLOCKS_DIVISOR    320ULL

#define MGFS_STATE_KNOWN_MASK        0x3ULL
#define MGFS_STATE_CLEAN             0x1ULL
#define MGFS_STATE_NEEDS_FSCK        0x2ULL

#define MGFS_METADATA_ALLOCATION_BITMAP 1ULL
#define MGFS_METADATA_RECORD_BITMAP     2ULL
#define MGFS_METADATA_RECORD_TABLE      3ULL

#define MGFS_RECORD_FILE              1ULL
#define MGFS_RECORD_DIRECTORY         2ULL
#define MGFS_RECORD_INLINE_DATA       0x1ULL
#define MGFS_RECORD_FLAGS_KNOWN       MGFS_RECORD_INLINE_DATA

#define MGFS_EXTENT_DATA              0x1ULL
#define MGFS_EXTENT_DIRECTORY_METADATA 0x2ULL
#define MGFS_EXTENT_LIST_METADATA     0x4ULL

#define MGFS_SUPER_CHECKSUM_OFFSET    192U
#define MGFS_METADATA_CHECKSUM_OFFSET 16U
#define MGFS_RECORD_CHECKSUM_OFFSET   184U

#define MGFS_CRC64_POLYNOMIAL         0x42F0E1EBA9EA3693ULL
#define MGFS_U64_MAX                  0xFFFFFFFFFFFFFFFFULL

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
} mgfs_layout_t;

typedef struct {
    block_device_t *dev;
    mgfs_layout_t layout;
    u64 total_blocks;
    u64 root_record_id;
    u64 next_record_id;
    u8 *record_bitmap;
    u64 record_bitmap_bytes;
    u64 *record_ids;
    u64 record_id_capacity;
    u64 allocated_record_count;
} mgfs_fs_t;

static const vfs_ops_t mgfs_node_ops;
static const vfs_super_ops_t mgfs_super_ops;
static const char *mgfs_error = "no error";

static u64 mgfs_get_le64(const u8 *data)
{
    u64 value = 0;
    for (u32 i = 0; i < 8; i++) {
        value |= (u64)data[i] << (i * 8);
    }
    return value;
}

static void mgfs_set_error(const char *message)
{
    mgfs_error = message;
}

static bool mgfs_bytes_equal(const u8 *left, const u8 *right, usize length)
{
    for (usize i = 0; i < length; i++) {
        if (left[i] != right[i]) {
            return false;
        }
    }
    return true;
}

const char *mgfs_last_error(void)
{
    return mgfs_error;
}

static u64 mgfs_crc64(const u8 *data, usize length)
{
    u64 crc = 0;

    for (usize byte = 0; byte < length; byte++) {
        crc ^= (u64)data[byte] << 56;
        for (u32 bit = 0; bit < 8; bit++) {
            if (crc & (1ULL << 63)) {
                crc = (crc << 1) ^ MGFS_CRC64_POLYNOMIAL;
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

static bool mgfs_checksum_block(u8 *block, u32 checksum_offset, usize covered_bytes)
{
    u64 stored = mgfs_get_le64(block + checksum_offset);
    u64 calculated;

    memset(block + checksum_offset, 0, 8);
    calculated = mgfs_crc64(block, covered_bytes);

    /* Restore the serialized value without relying on host byte order. */
    for (u32 i = 0; i < 8; i++) {
        block[checksum_offset + i] = (u8)(stored >> (i * 8));
    }

    return calculated == stored;
}

static bool mgfs_read_block(block_device_t *dev, u64 block_number, u8 *buffer)
{
    u64 lba;

    if (!dev || !buffer || dev->sector_size != 512U) {
        mgfs_set_error("MGFS requires 512-byte block-device sectors");
        return false;
    }

    if (block_number > (MGFS_U64_MAX / 8ULL)) {
        mgfs_set_error("MGFS block address overflows the sector address");
        return false;
    }

    lba = block_number * 8ULL;
    if (lba > dev->sector_count || dev->sector_count - lba < 8ULL) {
        mgfs_set_error("MGFS block lies outside the block device");
        return false;
    }

    if (!block_read(dev, lba, 8, buffer)) {
        mgfs_set_error("MGFS block read failed");
        return false;
    }

    return true;
}

static u64 mgfs_ceil_div(u64 value, u64 divisor)
{
    return (value / divisor) + ((value % divisor) != 0ULL);
}

static bool mgfs_calculate_layout(u64 total_blocks, mgfs_layout_t *layout)
{
    u64 table_blocks;
    u64 record_count;
    u64 record_bitmap_blocks;
    u64 allocation_bitmap_blocks;
    u64 data_start;

    if (total_blocks < MGFS_MIN_TOTAL_BLOCKS) {
        return false;
    }

    table_blocks = total_blocks / MGFS_TABLE_BLOCKS_DIVISOR;
    if (table_blocks == 0ULL) {
        table_blocks = 1ULL;
    }

    if (table_blocks > MGFS_U64_MAX / MGFS_RECORDS_PER_TABLE_BLOCK) {
        return false;
    }
    record_count = table_blocks * MGFS_RECORDS_PER_TABLE_BLOCK;
    record_bitmap_blocks = mgfs_ceil_div(record_count, MGFS_BITMAP_BITS_PER_BLOCK);
    allocation_bitmap_blocks = mgfs_ceil_div(total_blocks, MGFS_BITMAP_BITS_PER_BLOCK);

    if (allocation_bitmap_blocks > MGFS_U64_MAX - 1ULL ||
        record_bitmap_blocks > MGFS_U64_MAX - 1ULL - allocation_bitmap_blocks ||
        table_blocks > MGFS_U64_MAX - 1ULL - allocation_bitmap_blocks - record_bitmap_blocks) {
        return false;
    }

    data_start = 1ULL + allocation_bitmap_blocks + record_bitmap_blocks + table_blocks;
    if (data_start >= total_blocks) {
        return false;
    }

    layout->allocation_bitmap_start = 1ULL;
    layout->allocation_bitmap_blocks = allocation_bitmap_blocks;
    layout->record_bitmap_start = 1ULL + allocation_bitmap_blocks;
    layout->record_bitmap_blocks = record_bitmap_blocks;
    layout->record_table_start = layout->record_bitmap_start + record_bitmap_blocks;
    layout->record_table_blocks = table_blocks;
    layout->record_count = record_count;
    layout->data_start = data_start;
    layout->data_blocks = total_blocks - data_start;
    return true;
}

static bool mgfs_validate_superblock(
    block_device_t *dev,
    const u8 *block,
    mgfs_fs_t *fs)
{
    static const u8 magic[8] = { 'M', 'G', 'F', 'S', 'v', '1', 0, 0 };
    mgfs_layout_t expected;
    u8 checksum_block[MGFS_BLOCK_BYTES];
    u64 total_blocks;
    u64 state_flags;
    u64 feature_compat;
    u64 feature_incompat;

    if (!mgfs_bytes_equal(block, magic, sizeof(magic))) {
        mgfs_set_error("MGFS magic mismatch");
        return false;
    }

    if (mgfs_get_le64(block + 8) != MGFS_FORMAT_MAJOR ||
        mgfs_get_le64(block + 16) != MGFS_FORMAT_MINOR) {
        mgfs_set_error("unsupported MGFS version");
        return false;
    }

    if (mgfs_get_le64(block + 24) != MGFS_HEADER_BYTES ||
        mgfs_get_le64(block + 32) != MGFS_BLOCK_BYTES) {
        mgfs_set_error("invalid MGFS header or block size");
        return false;
    }

    for (u32 i = (u32)MGFS_HEADER_BYTES; i < MGFS_BLOCK_BYTES; i++) {
        if (block[i] != 0) {
            mgfs_set_error("nonzero MGFS superblock reserved bytes");
            return false;
        }
    }

    memcpy(checksum_block, block, MGFS_BLOCK_BYTES);
    if (!mgfs_checksum_block(checksum_block, MGFS_SUPER_CHECKSUM_OFFSET, MGFS_HEADER_BYTES)) {
        mgfs_set_error("MGFS superblock checksum mismatch");
        return false;
    }

    total_blocks = mgfs_get_le64(block + 40);
    if (total_blocks < MGFS_MIN_TOTAL_BLOCKS ||
        total_blocks != dev->sector_count / 8ULL ||
        (dev->sector_count % 8ULL) != 0ULL) {
        mgfs_set_error("MGFS total block count does not match the device");
        return false;
    }

    feature_compat = mgfs_get_le64(block + 48);
    feature_incompat = mgfs_get_le64(block + 56);
    if (feature_compat != 0ULL || feature_incompat != 0ULL) {
        mgfs_set_error("unsupported MGFS feature flags");
        return false;
    }

    state_flags = mgfs_get_le64(block + 64);
    if ((state_flags & ~MGFS_STATE_KNOWN_MASK) != 0ULL || state_flags == 0ULL) {
        mgfs_set_error("invalid MGFS state flags");
        return false;
    }

    if (!mgfs_calculate_layout(total_blocks, &expected) ||
        mgfs_get_le64(block + 104) != expected.allocation_bitmap_start ||
        mgfs_get_le64(block + 112) != expected.allocation_bitmap_blocks ||
        mgfs_get_le64(block + 120) != expected.record_bitmap_start ||
        mgfs_get_le64(block + 128) != expected.record_bitmap_blocks ||
        mgfs_get_le64(block + 136) != expected.record_table_start ||
        mgfs_get_le64(block + 144) != expected.record_table_blocks ||
        mgfs_get_le64(block + 152) != expected.record_count ||
        mgfs_get_le64(block + 160) != expected.data_start ||
        mgfs_get_le64(block + 168) != expected.data_blocks) {
        mgfs_set_error("invalid MGFS region layout");
        return false;
    }

    if (mgfs_get_le64(block + 88) != 1ULL || mgfs_get_le64(block + 96) < 2ULL) {
        mgfs_set_error("invalid MGFS root or next Record ID");
        return false;
    }

    fs->total_blocks = total_blocks;
    fs->layout = expected;
    fs->root_record_id = mgfs_get_le64(block + 88);
    fs->next_record_id = mgfs_get_le64(block + 96);
    return true;
}

static bool mgfs_validate_metadata_header(
    u8 *block,
    u64 expected_kind,
    u64 expected_index)
{
    u8 checksum_block[MGFS_BLOCK_BYTES];

    if (mgfs_get_le64(block) != expected_kind ||
        mgfs_get_le64(block + 8) != expected_index) {
        mgfs_set_error("invalid MGFS metadata block header");
        return false;
    }

    memcpy(checksum_block, block, MGFS_BLOCK_BYTES);
    if (!mgfs_checksum_block(checksum_block, MGFS_METADATA_CHECKSUM_OFFSET,
                             MGFS_BLOCK_BYTES)) {
        mgfs_set_error("MGFS metadata block checksum mismatch");
        return false;
    }

    return true;
}

static bool mgfs_validate_bitmap_region(
    mgfs_fs_t *fs,
    u64 start,
    u64 block_count,
    u64 expected_kind,
    u64 valid_bits,
    bool copy_record_bitmap)
{
    u8 block[MGFS_BLOCK_BYTES];

    for (u64 index = 0; index < block_count; index++) {
        u64 first_bit = index * MGFS_BITMAP_BITS_PER_BLOCK;
        u64 block_valid_bits = 0;

        if (first_bit < valid_bits) {
            u64 remaining = valid_bits - first_bit;
            block_valid_bits = remaining < MGFS_BITMAP_BITS_PER_BLOCK
                ? remaining : MGFS_BITMAP_BITS_PER_BLOCK;
        }

        if (!mgfs_read_block(fs->dev, start + index, block) ||
            !mgfs_validate_metadata_header(block, expected_kind, index)) {
            return false;
        }

        for (u64 bit = block_valid_bits; bit < MGFS_BITMAP_BITS_PER_BLOCK; bit++) {
            if ((block[MGFS_BITMAP_HEADER_BYTES + bit / 8ULL] &
                 (1U << (bit % 8ULL))) == 0U) {
                mgfs_set_error("MGFS bitmap has a free out-of-range bit");
                return false;
            }
        }

        if (copy_record_bitmap) {
            memcpy(fs->record_bitmap + index * MGFS_BLOCK_BYTES,
                   block, MGFS_BLOCK_BYTES);
        }
    }

    return true;
}

static bool mgfs_record_slot_allocated(const mgfs_fs_t *fs, u64 slot)
{
    u64 bitmap_block = slot / MGFS_BITMAP_BITS_PER_BLOCK;
    u64 bit = slot % MGFS_BITMAP_BITS_PER_BLOCK;
    u64 byte_offset = bitmap_block * MGFS_BLOCK_BYTES + MGFS_BITMAP_HEADER_BYTES + bit / 8ULL;
    return (fs->record_bitmap[byte_offset] & (1U << (bit % 8ULL))) != 0U;
}

static u64 mgfs_hash_id(u64 id)
{
    id ^= id >> 30;
    id *= 0xbf58476d1ce4e5b9ULL;
    id ^= id >> 27;
    id *= 0x94d049bb133111ebULL;
    return id ^ (id >> 31);
}

static bool mgfs_insert_record_id(mgfs_fs_t *fs, u64 id)
{
    u64 mask = fs->record_id_capacity - 1ULL;
    u64 index = mgfs_hash_id(id) & mask;

    for (u64 probe = 0; probe < fs->record_id_capacity; probe++) {
        if (fs->record_ids[index] == 0ULL) {
            fs->record_ids[index] = id;
            return true;
        }
        if (fs->record_ids[index] == id) {
            mgfs_set_error("duplicate MGFS Record ID");
            return false;
        }
        index = (index + 1ULL) & mask;
    }

    mgfs_set_error("MGFS Record ID index is full");
    return false;
}

static bool mgfs_validate_record(const u8 *record)
{
    u8 checksum_record[MGFS_RECORD_BYTES];
    u64 type = mgfs_get_le64(record);
    u64 flags = mgfs_get_le64(record + 8);
    u64 id = mgfs_get_le64(record + 16);
    u64 generation = mgfs_get_le64(record + 24);
    u64 size = mgfs_get_le64(record + 32);
    u64 extent_count = mgfs_get_le64(record + 40);
    u64 inline_extent_count = mgfs_get_le64(record + 48);
    u64 extent_list_head = mgfs_get_le64(record + 56);

    memcpy(checksum_record, record, MGFS_RECORD_BYTES);
    if (!mgfs_checksum_block(checksum_record, MGFS_RECORD_CHECKSUM_OFFSET,
                             MGFS_RECORD_BYTES)) {
        mgfs_set_error("MGFS File Record checksum mismatch");
        return false;
    }

    if (id == 0ULL || generation == 0ULL ||
        (type != MGFS_RECORD_FILE && type != MGFS_RECORD_DIRECTORY) ||
        (flags & ~MGFS_RECORD_FLAGS_KNOWN) != 0ULL ||
        inline_extent_count > 2ULL || inline_extent_count > extent_count ||
        (extent_count == 0ULL && extent_list_head != 0ULL) ||
        (extent_count <= 2ULL && extent_list_head != 0ULL)) {
        mgfs_set_error("invalid MGFS File Record fields");
        return false;
    }

    if ((flags & MGFS_RECORD_INLINE_DATA) != 0ULL &&
        (type != MGFS_RECORD_FILE || extent_count != 0ULL ||
         inline_extent_count != 0ULL || extent_list_head != 0ULL || size > 56ULL)) {
        mgfs_set_error("invalid MGFS inline-data File Record");
        return false;
    }

    return true;
}

static bool mgfs_scan_records(mgfs_fs_t *fs)
{
    u8 block[MGFS_BLOCK_BYTES];
    u64 allocated = 0;
    u64 capacity = 16ULL;

    for (u64 slot = 0; slot < fs->layout.record_count; slot++) {
        if (mgfs_record_slot_allocated(fs, slot)) {
            allocated++;
        }
    }

    while (capacity < (allocated > MGFS_U64_MAX / 2ULL ? MGFS_U64_MAX : allocated * 2ULL)) {
        if (capacity > MGFS_U64_MAX / 2ULL) {
            mgfs_set_error("MGFS Record ID index size overflow");
            return false;
        }
        capacity <<= 1;
    }

    if (capacity > (u64)(usize)-1 / sizeof(u64)) {
        mgfs_set_error("MGFS Record ID index allocation overflow");
        return false;
    }

    fs->record_ids = (u64 *)kmalloc((usize)(capacity * sizeof(u64)));
    if (!fs->record_ids) {
        mgfs_set_error("unable to allocate MGFS Record ID index");
        return false;
    }
    memset(fs->record_ids, 0, (usize)(capacity * sizeof(u64)));
    fs->record_id_capacity = capacity;
    fs->allocated_record_count = allocated;

    u64 root_slot = MGFS_U64_MAX;
    for (u64 table_index = 0; table_index < fs->layout.record_table_blocks; table_index++) {
        if (!mgfs_read_block(fs->dev, fs->layout.record_table_start + table_index, block) ||
            !mgfs_validate_metadata_header(block, MGFS_METADATA_RECORD_TABLE, table_index)) {
            return false;
        }

        for (u64 slot_in_block = 0; slot_in_block < MGFS_RECORDS_PER_TABLE_BLOCK; slot_in_block++) {
            u64 slot = table_index * MGFS_RECORDS_PER_TABLE_BLOCK + slot_in_block;
            u8 *record;

            if (slot >= fs->layout.record_count || !mgfs_record_slot_allocated(fs, slot)) {
                continue;
            }

            record = block + MGFS_BITMAP_HEADER_BYTES + slot_in_block * MGFS_RECORD_BYTES;
            if (!mgfs_validate_record(record) ||
                !mgfs_insert_record_id(fs, mgfs_get_le64(record + 16))) {
                return false;
            }

            if (mgfs_get_le64(record + 16) == fs->root_record_id) {
                if (root_slot != MGFS_U64_MAX || mgfs_get_le64(record) != MGFS_RECORD_DIRECTORY) {
                    mgfs_set_error("invalid MGFS root Record");
                    return false;
                }
                root_slot = slot;
            }
        }
    }

    if (root_slot == MGFS_U64_MAX) {
        mgfs_set_error("MGFS root Record was not found");
        return false;
    }

    if (root_slot != 0ULL) {
        mgfs_set_error("MGFS root Record is not in table slot zero");
        return false;
    }

    return true;
}

static bool mgfs_probe(block_device_t *dev)
{
    u8 block[MGFS_BLOCK_BYTES];
    static const u8 magic[8] = { 'M', 'G', 'F', 'S', 'v', '1', 0, 0 };

    if (!dev || dev->sector_size != 512U || dev->sector_count < 8ULL ||
        !mgfs_read_block(dev, 0, block)) {
        return false;
    }

    return mgfs_bytes_equal(block, magic, sizeof(magic));
}

static int mgfs_mount(vfs_fs_type_t *fs_type, block_device_t *dev, vfs_super_t **out_sb)
{
    u8 superblock[MGFS_BLOCK_BYTES];
    vfs_super_t *sb;
    vfs_node_t *root;
    mgfs_fs_t *fs;

    (void)fs_type;
    mgfs_set_error("no error");

    if (!dev || !out_sb || dev->sector_size != 512U) {
        mgfs_set_error("invalid MGFS mount device");
        return VFS_ERR_INVALID_PARAM;
    }

    fs = (mgfs_fs_t *)kmalloc(sizeof(mgfs_fs_t));
    if (!fs) {
        mgfs_set_error("unable to allocate MGFS state");
        return VFS_ERR_NO_MEM;
    }
    memset(fs, 0, sizeof(*fs));
    fs->dev = dev;

    if (!mgfs_read_block(dev, 0, superblock) ||
        !mgfs_validate_superblock(dev, superblock, fs)) {
        kfree(fs);
        return VFS_ERR_BAD_FORMAT;
    }

    if (fs->layout.record_bitmap_blocks > (u64)(usize)-1 / MGFS_BLOCK_BYTES) {
        mgfs_set_error("MGFS Record bitmap allocation overflow");
        kfree(fs);
        return VFS_ERR_BAD_FORMAT;
    }
    fs->record_bitmap_bytes = fs->layout.record_bitmap_blocks * MGFS_BLOCK_BYTES;
    fs->record_bitmap = (u8 *)kmalloc((usize)fs->record_bitmap_bytes);
    if (!fs->record_bitmap) {
        mgfs_set_error("unable to allocate MGFS Record bitmap");
        kfree(fs);
        return VFS_ERR_NO_MEM;
    }

    if (!mgfs_validate_bitmap_region(fs,
                                     fs->layout.allocation_bitmap_start,
                                     fs->layout.allocation_bitmap_blocks,
                                     MGFS_METADATA_ALLOCATION_BITMAP,
                                     fs->layout.data_blocks, false) ||
        !mgfs_validate_bitmap_region(fs,
                                     fs->layout.record_bitmap_start,
                                     fs->layout.record_bitmap_blocks,
                                     MGFS_METADATA_RECORD_BITMAP,
                                     fs->layout.record_count, true) ||
        !mgfs_scan_records(fs)) {
        kfree(fs->record_bitmap);
        kfree(fs->record_ids);
        kfree(fs);
        return VFS_ERR_BAD_FORMAT;
    }

    sb = (vfs_super_t *)kmalloc(sizeof(vfs_super_t));
    root = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!sb || !root) {
        kfree(sb);
        kfree(root);
        kfree(fs->record_bitmap);
        kfree(fs->record_ids);
        kfree(fs);
        mgfs_set_error("unable to allocate MGFS VFS objects");
        return VFS_ERR_NO_MEM;
    }

    memset(sb, 0, sizeof(*sb));
    memset(root, 0, sizeof(*root));
    root->inode = fs->root_record_id;
    root->type = VFS_TYPE_DIRECTORY;
    root->ref_count = 1;
    root->super = sb;
    root->fs_data = NULL;
    root->ops = &mgfs_node_ops;

    sb->fs_type = fs_type;
    sb->dev = dev;
    sb->root_node = root;
    sb->private_data = fs;
    sb->ops = &mgfs_super_ops;
    *out_sb = sb;
    return VFS_OK;
}

static int mgfs_unmount(vfs_super_t *sb)
{
    mgfs_fs_t *fs;

    if (!sb) {
        return VFS_ERR_INVALID_PARAM;
    }

    fs = (mgfs_fs_t *)sb->private_data;
    if (fs) {
        kfree(fs->record_bitmap);
        kfree(fs->record_ids);
        kfree(fs);
    }
    kfree(sb->root_node);
    kfree(sb);
    return VFS_OK;
}

static const vfs_ops_t mgfs_node_ops = {
    .read = NULL,
    .write = NULL,
    .finddir = NULL,
    .readdir = NULL,
    .create = NULL,
    .mkdir = NULL,
    .unlink = NULL,
    .rmdir = NULL,
};

static const vfs_super_ops_t mgfs_super_ops = {
    .unmount = mgfs_unmount,
    .sync = NULL,
};

static vfs_fs_type_t mgfs_fs_type = {
    .name = "mgfs",
    .probe = mgfs_probe,
    .mount = mgfs_mount,
    .next = NULL,
};

int mgfs_init(void)
{
    return vfs_register_fs(&mgfs_fs_type);
}
