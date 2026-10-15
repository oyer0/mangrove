/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <storage/fat32.h>
#include <storage/format.h>
#include <heap.h>
#include <string.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

/* FAT32 has no portable ownership or permission fields.  Removable FAT32
 * volumes therefore follow the same shared-volume policy as exFAT: their
 * synthetic VFS nodes are system-owned but writable by ordinary users.  A
 * system ESP is mounted read-only by VFS, so this does not make /boot/efi
 * mutable from userspace. */
#define FAT32_VOLUME_PERMISSIONS \
    (VFS_DEFAULT_SYSTEM_PERMISSIONS | VFS_PERMISSION_OTHER_WRITE)

/* Requested heap per complete proof, including the directory scratch buffer.
 * Claims and pending directories grow in chunks; exhaustion proves nothing. */
#define FAT32_SCAN_MEMORY_BUDGET (8U * 1024U * 1024U)
#define FAT32_SCAN_CHUNK_BYTES 4096U
#define FAT32_SCAN_CHUNK_CLUSTERS (FAT32_SCAN_CHUNK_BYTES * 8U)
/* Safe directory growth retains its existing separate slot bound. */
#define FAT32_SCAN_MAX_ENTRIES 65536U
#define FAT32_WALK_MAX_DIR_CLUSTERS 4096U
#define FAT32_REPLACE_MAX_BYTES (2U * 1024U * 1024U)

struct fat32_replace_transaction;

typedef struct fat32_node_state {
    struct fat32_node_state *next;
    vfs_node_t *node;
    u64 generation;
    u32 first_cluster;
    u32 parent_cluster;
    u32 entry_index;
    /* Mapping summary is usable only with the mount ownership certificate. */
    u32 allocation_count, allocation_tail;
    u64 allocation_generation;
    bool alive;
    bool version_valid;
} fat32_node_state_t;

#pragma pack(push, 1)
typedef struct {
    u8  jmp_boot[3];
    char oem_name[8];

    u16 bytes_per_sector;
    u8  sectors_per_cluster;
    u16 reserved_sector_count;
    u8  fat_count;
    u16 root_entry_count;
    u16 total_sectors_16;
    u8  media_type;
    u16 sectors_per_fat_16;

    u16 sectors_per_track;
    u16 head_count;
    u32 hidden_sectors;
    u32 total_sectors_32;

    u32 sectors_per_fat_32;
    u16 ext_flags;
    u16 fs_version;
    u32 root_cluster;
    u16 fs_info_sector;
    u16 backup_boot_sector;
    u8  reserved[12];
    u8  drive_number;
    u8  reserved1;
    u8  boot_signature;
    u32 volume_id;
    char volume_label[11];
    char fs_type_label[8];
} fat32_bpb_t;

typedef struct {
    char name[11];             // 8.3 Short Filename
    u8   attr;                 // Attributes
    u8   nt_reserved;
    u8   create_time_tenths;
    u16  create_time;
    u16  create_date;
    u16  last_access_date;
    u16  first_cluster_high;
    u16  write_time;
    u16  write_date;
    u16  first_cluster_low;
    u32  file_size;
} fat32_on_disk_entry_t;

typedef struct {
    u8   order;
    u16  name1[5];
    u8   attr;
    u8   type;
    u8   checksum;
    u16  name2[6];
    u16  first_cluster;
    u16  name3[2];
} fat32_lfn_entry_t;
#pragma pack(pop)
_Static_assert(sizeof(fat32_on_disk_entry_t) == 32U,
               "FAT32 directory entries must fit one sector");

typedef struct {
    u64 device_id;
    fat32_bpb_t bpb;
    u32 bytes_per_sector;
    u32 sectors_per_cluster;
    u32 cluster_size_bytes;
    u32 reserved_sector_count;
    u32 fat_count;
    u32 fat_size_sectors;
    u32 fs_info_sector;
    u32 backup_boot_sector;
    u32 fat_start_lba;
    u32 data_start_lba;
    u32 root_cluster;
    u32 total_clusters;
    u32 next_free_cluster;
    u64 next_inode;
    fat32_node_state_t *nodes;
    bool version_confident;
    bool namespace_hidden;
    bool ownership_proven;
    struct fat32_replace_transaction *replace_transaction;
} fat32_fs_t;

static bool fat32_mounted_read(vfs_super_t *sb, u64 lba, u32 count,
                               void *buffer)
{
    if (!sb || !sb->private_data) return false;
    fat32_fs_t *fs = (fat32_fs_t *)sb->private_data;
    return block_read_for_id(sb->dev, fs->device_id, lba, count, buffer);
}

static bool fat32_mounted_write(vfs_super_t *sb, u64 lba, u32 count,
                                const void *buffer)
{
    if (!sb || !sb->private_data || sb->read_only) return false;
    fat32_fs_t *fs = (fat32_fs_t *)sb->private_data;
    /* Every mounted mutation invalidates the certificate before admission. */
    fs->ownership_proven = false;
    bool written = block_write_for_id(sb->dev, fs->device_id, lba, count,
                                      buffer);
    if (!written) fs->version_confident = false;
    return written;
}

static const vfs_ops_t fat32_node_ops;
/* Read-only operation-local FAT sectors. No view survives a media mutation
 * or lock release. Each mirror is loaded and compared entry-by-entry. */
typedef struct {
    u8 sectors[4][512];
    u64 lbas[4];
    fat32_fs_t *owner;
    u64 device_id;
    u8 valid;
} fat32_read_view_t;

static int fat32_get_next_checked_view(vfs_super_t *sb, fat32_fs_t *fs,
    u32 cluster, u32 *out, fat32_read_view_t *view);

static u32 fat32_get_next_cluster(vfs_super_t *sb, fat32_fs_t *fs, u32 current_cluster);
static int fat32_get_next_checked(vfs_super_t *sb, fat32_fs_t *fs,
                                   u32 cluster, u32 *out);

/* The operation lock serializes this mount-local list and its generations.
 * VFS nodes are long lived; a deleted slot stays tombstoned until unmount,
 * so reusing that slot cannot revive an old handle. */
static fat32_node_state_t *fat32_state(const vfs_node_t *node)
{
    return node ? (fat32_node_state_t *)node->fs_data : NULL;
}

static u32 fat32_node_cluster(const vfs_node_t *node)
{
    fat32_node_state_t *state = fat32_state(node);
    fat32_fs_t *fs = node && node->super
        ? (fat32_fs_t *)node->super->private_data : NULL;
    return state && state->alive && state->version_valid
        && fs && fs->version_confident
        ? state->first_cluster : FAT32_CLUSTER_ERR;
}

static fat32_node_state_t *fat32_find_state(fat32_fs_t *fs, u32 parent,
                                            u32 index)
{
    for (fat32_node_state_t *state = fs->nodes; state; state = state->next)
        if (state->alive && state->parent_cluster == parent &&
            state->entry_index == index) return state;
    return NULL;
}

static void fat32_version_advance(fat32_node_state_t *state)
{
    if (!state || !state->version_valid) return;
    if (state->generation == ~(u64)0) state->version_valid = false;
    else state->generation++;
}

static vfs_node_t *fat32_attach_node(vfs_super_t *sb, fat32_fs_t *fs,
                                     u32 parent_cluster, u32 entry_index,
                                     u32 first_cluster, u32 size,
                                     vfs_node_type_t type,
                                     vfs_node_t *node,
                                     fat32_node_state_t *state)
{
    memset(node, 0, sizeof(*node));
    memset(state, 0, sizeof(*state));
    node->inode = ++fs->next_inode;
    node->type = type;
    node->size = size;
    vfs_node_set_security(node, VFS_UID_SYSTEM, FAT32_VOLUME_PERMISSIONS);
    node->child_mutation_policy = VFS_CHILD_MUTATION_OPEN;
    node->ref_count = 1;
    node->super = sb;
    node->fs_data = state;
    node->ops = &fat32_node_ops;
    state->node = node;
    state->first_cluster = first_cluster;
    state->parent_cluster = parent_cluster;
    state->entry_index = entry_index;
    state->generation = 1;
    state->alive = true;
    state->version_valid = true;
    state->next = fs->nodes;
    fs->nodes = state;
    vfs_node_register(node);
    return node;
}

static vfs_node_t *fat32_bind_node(vfs_super_t *sb, fat32_fs_t *fs,
                                   u32 parent_cluster, u32 entry_index,
                                   u32 first_cluster, u32 size,
                                   vfs_node_type_t type)
{
    fat32_node_state_t *state = fat32_find_state(fs, parent_cluster,
                                                 entry_index);
    vfs_node_t *node;
    if ((size && (first_cluster < 2U ||
                  first_cluster >= fs->total_clusters)) ||
        (type == VFS_TYPE_DIRECTORY &&
         (first_cluster < 2U || first_cluster >= fs->total_clusters)))
        return NULL;
    if (first_cluster >= 2U) {
        for (fat32_node_state_t *other = fs->nodes; other;
             other = other->next) {
            if (other->alive && other->first_cluster == first_cluster &&
                (other->parent_cluster != parent_cluster ||
                 other->entry_index != entry_index)) {
                fs->version_confident = false;
                return NULL;
            }
        }
    }
    if (state) {
        if (state->first_cluster != first_cluster || state->node->type != type ||
            state->node->size != size) {
            state->version_valid = false;
            return NULL;
        }
        return state->node;
    }
    if (fs->next_inode == ~(u64)0) return NULL;
    node = (vfs_node_t *)kmalloc(sizeof(*node));
    state = (fat32_node_state_t *)kmalloc(sizeof(*state));
    if (!node || !state) {
        kfree(node);
        kfree(state);
        return NULL;
    }
    return fat32_attach_node(sb, fs, parent_cluster, entry_index,
                             first_cluster, size, type, node, state);
}

static void fat32_store_le32(void *target, u32 value) {
    u8 *bytes = (u8 *)target;
    bytes[0] = (u8)value;
    bytes[1] = (u8)(value >> 8U);
    bytes[2] = (u8)(value >> 16U);
    bytes[3] = (u8)(value >> 24U);
}

static u32 fat32_load_le32(const u8 *source) {
    return (u32)source[0] | ((u32)source[1] << 8U) |
           ((u32)source[2] << 16U) | ((u32)source[3] << 24U);
}

static u64 fat32_cluster_to_lba(fat32_fs_t *fs, u32 cluster) {
    return (u64)fs->data_start_lba + (u64)(cluster - 2) * fs->sectors_per_cluster;
}

static char fat32_ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

typedef struct {
    u32 first, last;
    i32 delta;
    u32 stride;
} fat32_upper_range_t;

/* Fixed simple (one scalar to one scalar) uppercase, not full casefolding or
 * locale-sensitive casing. No mount state, allocations or mutable cache. */
static const fat32_upper_range_t fat32_upper_ranges[] = {
#include "fat32_uppercase.inc"
};

static u32 fat32_uppercase(u32 value)
{
    if (value < 0x80U)
        return value >= 'a' && value <= 'z' ? value - ('a' - 'A') : value;
    usize low = 0, high = sizeof(fat32_upper_ranges) / sizeof(*fat32_upper_ranges);
    while (low < high) {
        usize middle = low + (high - low) / 2U;
        const fat32_upper_range_t *range = &fat32_upper_ranges[middle];
        if (value < range->first) high = middle;
        else if (value > range->last) low = middle + 1U;
        else return (value - range->first) % range->stride == 0U
            ? (u32)((i32)value + range->delta) : value;
    }
    return value;
}

/* Strict UTF-8: never substitute U+FFFD, accept overlong encodings or read
 * beyond the terminator. Both name validation and comparison use this. */
static bool fat32_utf8_next(const char **cursor, u32 *out)
{
    const u8 *p = (const u8 *)*cursor;
    u32 value, minimum, count;
    if (*p < 0x80U) { value = *p++; count = 0U; minimum = 0U; }
    else if (*p >= 0xc2U && *p <= 0xdfU) {
        value = *p++ & 0x1fU; count = 1U; minimum = 0x80U;
    } else if (*p >= 0xe0U && *p <= 0xefU) {
        value = *p++ & 0x0fU; count = 2U; minimum = 0x800U;
    } else if (*p >= 0xf0U && *p <= 0xf4U) {
        value = *p++ & 0x07U; count = 3U; minimum = 0x10000U;
    } else return false;
    for (u32 i = 0; i < count; i++) {
        if ((*p & 0xc0U) != 0x80U) return false;
        value = (value << 6U) | (*p++ & 0x3fU);
    }
    if (value < minimum || value > 0x10ffffU ||
        (value >= 0xd800U && value <= 0xdfffU)) return false;
    *cursor = (const char *)p;
    *out = value;
    return true;
}

static bool fat32_name_valid(const char *name)
{
    u32 units = 0, value;
    if (!name || !*name) return false;
    while (*name) {
        if (!fat32_utf8_next(&name, &value) || value == 0xffffU) return false;
        units += value >= 0x10000U ? 2U : 1U;
        if (units > 255U) return false;
    }
    return true;
}

/* FAT's boundary spaces and trailing periods do not distinguish names.
 * This is independent of Unicode normalization, which is deliberately absent. */
static void fat32_name_bounds(const char **start, const char **end)
{
    *end = *start + strlen(*start);
    while (*start < *end && **start == ' ') (*start)++;
    while (*end > *start && ((*end)[-1] == ' ' || (*end)[-1] == '.')) (*end)--;
}

static bool fat32_short_ascii(const fat32_on_disk_entry_t *entry)
{
    /* FAT has no code-page tag. High bytes (including the escaped 0xe5 first
     * byte 0x05) cannot be decoded without guessing an OEM/DBCS encoding. */
    for (u32 i = 0; i < 11U; i++) {
        u8 c = (u8)entry->name[i];
        if (c < 0x20U || c >= 0x7fU) return false;
        if (c == '"' || c == '*' || c == '+' || c == ',' || c == '.' ||
            c == '/' || c == ':' || c == ';' || c == '<' || c == '=' ||
            c == '>' || c == '?' || c == '[' || c == '\\' || c == ']' ||
            c == '|') return false;
    }
    /* Spaces pad each field; they cannot occur inside its actual name. */
    for (u32 first = 0U, last = 8U; first < 11U; first = 8U, last = 11U) {
        bool padded = false;
        for (u32 i = first; i < last; i++) {
            if (entry->name[i] == ' ') padded = true;
            else if (padded) return false;
        }
        if (last == 11U) break;
    }
    if (entry->name[0] == ' ') return false;
    return true;
}

static bool fat32_format_83_name(const fat32_on_disk_entry_t *entry,
                                 char *dst_out) {
    dst_out[0] = '\0';
    if (!fat32_short_ascii(entry)) return false;
    const char *src_11 = entry->name;
    u32 name_len = 8;
    while (name_len > 0 && src_11[name_len - 1] == ' ') {
        name_len--;
    }

    u32 ext_len = 3;
    while (ext_len > 0 && src_11[8 + ext_len - 1] == ' ') {
        ext_len--;
    }

    u32 pos = 0;
    for (u32 i = 0; i < name_len; i++) {
        char c = src_11[i];
        if (entry->nt_reserved & 0x08) c = fat32_ascii_lower(c);
        dst_out[pos++] = c;
    }

    if (ext_len > 0) {
        dst_out[pos++] = '.';
        for (u32 i = 0; i < ext_len; i++) {
            char c = src_11[8 + i];
            if (entry->nt_reserved & 0x10) c = fat32_ascii_lower(c);
            dst_out[pos++] = c;
        }
    }

    dst_out[pos] = '\0';
    return pos != 0U;
}

static bool fat32_match_text(const char *stored_name, const char *target_name) {
    const char *s1 = stored_name, *s2 = target_name, *end1, *end2;
    if (!fat32_name_valid(s1) || !fat32_name_valid(s2)) return false;
    fat32_name_bounds(&s1, &end1);
    fat32_name_bounds(&s2, &end2);
    if (s1 == end1 || s2 == end2) return false;
    while (s1 < end1 && s2 < end2) {
        u32 a, b;
        if (!fat32_utf8_next(&s1, &a) || !fat32_utf8_next(&s2, &b) ||
            fat32_uppercase(a) != fat32_uppercase(b)) return false;
    }
    return s1 == end1 && s2 == end2;
}

static bool fat32_match_name(const fat32_on_disk_entry_t *entry,
                             const char *target_name) {
    char formatted[13];
    return fat32_format_83_name(entry, formatted) &&
           fat32_match_text(formatted, target_name);
}

typedef struct {
    u16 units[260];
    bool present[20];
    u32 cluster[20], slot[20];
    u8 total;
    u8 checksum;
    u8 next_expected;
    bool valid;
} fat32_lfn_state_t;

static void fat32_lfn_reset(fat32_lfn_state_t *state) {
    if (state) memset(state, 0, sizeof(*state));
}

static u8 fat32_lfn_checksum(const char *short_name) {
    u8 checksum = 0;
    for (u32 i = 0; i < 11U; i++) {
        checksum = (u8)(((checksum & 1U) ? 0x80U : 0U) +
                        (checksum >> 1U) + (u8)short_name[i]);
    }
    return checksum;
}

static void fat32_lfn_collect(fat32_lfn_state_t *state,
                              const fat32_lfn_entry_t *entry,
                              u32 cluster, u32 slot) {
    u8 sequence;
    u32 base;

    if (!state || !entry) return;
    if (entry->attr != 0x0FU || entry->type != 0U ||
        entry->first_cluster != 0U || (entry->order & 0xa0U)) {
        state->valid = false;
        return;
    }
    sequence = entry->order & 0x1FU;
    if (sequence == 0U || sequence > 20U) {
        state->valid = false;
        return;
    }
    if (entry->order & 0x40U) {
        fat32_lfn_reset(state);
        state->total = sequence;
        state->checksum = entry->checksum;
        state->next_expected = sequence;
        state->valid = true;
    }
    if (!state->valid || sequence != state->next_expected ||
        state->present[sequence - 1U] || entry->checksum != state->checksum) {
        state->valid = false;
        return;
    }
    base = (u32)(sequence - 1U) * 13U;
    for (u32 i = 0; i < 5U; i++) state->units[base + i] = entry->name1[i];
    for (u32 i = 0; i < 6U; i++) state->units[base + 5U + i] = entry->name2[i];
    for (u32 i = 0; i < 2U; i++) state->units[base + 11U + i] = entry->name3[i];
    state->present[sequence - 1U] = true;
    state->cluster[sequence - 1U] = cluster;
    state->slot[sequence - 1U] = slot;
    state->next_expected--;
}

static bool fat32_lfn_length(const fat32_lfn_state_t *state,
                              const fat32_on_disk_entry_t *short_entry,
                              u32 *out_length)
{
    if (!state || !short_entry || !state->valid || state->next_expected != 0U ||
        state->total == 0U || state->total > 20U ||
        fat32_lfn_checksum(short_entry->name) != state->checksum) return false;
    for (u32 i = 0; i < state->total; i++) {
        if (!state->present[i]) return false;
    }
    u32 unit_count = (u32)state->total * 13U, length = 0U;
    while (length < unit_count && state->units[length] != 0U) {
        u16 value = state->units[length++];
        if (value == 0xffffU || length > 255U) return false;
        if (value >= 0xd800U && value <= 0xdbffU) {
            if (length >= unit_count || state->units[length] < 0xdc00U ||
                state->units[length] > 0xdfffU || ++length > 255U) return false;
        } else if (value >= 0xdc00U && value <= 0xdfffU) return false;
    }
    if (!length || length <= (u32)(state->total - 1U) * 13U) return false;
    if (length < unit_count) {
        for (u32 i = length + 1U; i < unit_count; i++)
            if (state->units[i] != 0xffffU) return false;
    }
    *out_length = length;
    return true;
}

/* Called only after fat32_lfn_length has validated every surrogate pair. */
static u32 fat32_lfn_next(const fat32_lfn_state_t *state, u32 *index)
{
    u32 value = state->units[(*index)++];
    if (value >= 0xd800U && value <= 0xdbffU)
        value = 0x10000U + ((value - 0xd800U) << 10U) +
                ((u32)state->units[(*index)++] - 0xdc00U);
    return value;
}

static bool fat32_lfn_encode(const fat32_lfn_state_t *state,
                             const fat32_on_disk_entry_t *short_entry,
                             char *output, usize capacity) {
    u32 unit_count;
    usize position = 0;
    if (!output || !capacity ||
        !fat32_lfn_length(state, short_entry, &unit_count)) return false;
    for (u32 i = 0; i < unit_count; i++) {
        u32 codepoint;
        u16 value = state->units[i];
        usize encoded;
        if (value == 0U) break;
        if (value == 0xFFFFU) return false;
        if (value >= 0xD800U && value <= 0xDBFFU) {
            u16 low;
            if (i + 1U >= unit_count) return false;
            low = state->units[++i];
            if (low < 0xDC00U || low > 0xDFFFU) return false;
            codepoint = 0x10000U +
                        (((u32)value - 0xD800U) << 10U) +
                        ((u32)low - 0xDC00U);
        } else if (value >= 0xDC00U && value <= 0xDFFFU) {
            return false;
        } else {
            codepoint = value;
        }
        if (codepoint < 0x80U) encoded = 1U;
        else if (codepoint < 0x800U) encoded = 2U;
        else if (codepoint < 0x10000U) encoded = 3U;
        else encoded = 4U;
        if (encoded >= capacity || position > capacity - 1U - encoded) return false;
        if (encoded == 1U) output[position++] = (char)codepoint;
        else if (encoded == 2U) {
            output[position++] = (char)(0xC0U | (codepoint >> 6U));
            output[position++] = (char)(0x80U | (codepoint & 0x3FU));
        } else if (encoded == 3U) {
            output[position++] = (char)(0xE0U | (codepoint >> 12U));
            output[position++] = (char)(0x80U | ((codepoint >> 6U) & 0x3FU));
            output[position++] = (char)(0x80U | (codepoint & 0x3FU));
        } else {
            output[position++] = (char)(0xF0U | (codepoint >> 18U));
            output[position++] = (char)(0x80U | ((codepoint >> 12U) & 0x3FU));
            output[position++] = (char)(0x80U | ((codepoint >> 6U) & 0x3FU));
            output[position++] = (char)(0x80U | (codepoint & 0x3FU));
        }
    }
    if (position == 0U || position >= capacity) return false;
    output[position] = '\0';
    return true;
}

static bool fat32_match_entry(const fat32_on_disk_entry_t *entry,
                              const fat32_lfn_state_t *state,
                              const char *target_name) {
    u32 length, index = 0U;
    const char *target = target_name, *end;
    if (!fat32_name_valid(target)) return false;
    if (fat32_lfn_length(state, entry, &length)) {
        while (index < length && state->units[index] == ' ') index++;
        while (length > index && (state->units[length - 1U] == ' ' ||
                                  state->units[length - 1U] == '.')) length--;
        fat32_name_bounds(&target, &end);
        bool equal = index < length && target < end;
        while (equal && index < length && target < end) {
            u32 b;
            equal = fat32_utf8_next(&target, &b) &&
                fat32_uppercase(fat32_lfn_next(state, &index)) == fat32_uppercase(b);
        }
        if (equal && index == length && target == end) return true;
    }
    return fat32_match_name(entry, target_name);
}

typedef struct {
    fat32_on_disk_entry_t disk;
    u32 cluster;
    u32 slot;
    bool deleted;
    bool unused;
    bool dot;
    bool has_lfn;
    fat32_lfn_state_t lfn;
} fat32_walk_entry_t;

/* Return 1 to stop successfully, 0 to continue, or a negative VFS error.
 * A NULL entry announces a directory-chain cluster before its contents are
 * read. The ownership scanner uses that event to claim directory storage. */
typedef int (*fat32_walk_visit_t)(void *, u32, const fat32_walk_entry_t *);

static int fat32_dir_walk_limit(vfs_super_t *sb, u32 first, bool complete,
                          bool strict, fat32_walk_visit_t visit, void *context,
                          fat32_read_view_t *view, u32 max_clusters, u8 *scratch_buffer)
{
    fat32_fs_t *fs = (fat32_fs_t *)sb->private_data;
    u8 *buffer;
    fat32_lfn_state_t lfn;
    u32 current = first, tortoise = first, power = 1U, span = 0U;
    bool ended = false;
    int result = VFS_OK;

    if (!fs || first < 2U || first >= fs->total_clusters || !visit)
        return VFS_ERR_BAD_FORMAT;
    buffer = scratch_buffer ? scratch_buffer : (u8 *)kmalloc(fs->cluster_size_bytes);
    if (!buffer) return VFS_ERR_NO_MEM;
    fat32_lfn_reset(&lfn);
    for (u32 traversed = 0; traversed < max_clusters;
         traversed++) {
        if (current < 2U || current >= fs->total_clusters) {
            result = VFS_ERR_BAD_FORMAT;
            break;
        }
        result = visit(context, current, NULL);
        if (result != 0) {
            if (result > 0) result = VFS_OK;
            break;
        }
        if (!fat32_mounted_read(sb, fat32_cluster_to_lba(fs, current),
                                fs->sectors_per_cluster, buffer)) {
            result = VFS_ERR_IO;
            break;
        }
        fat32_on_disk_entry_t *entries = (fat32_on_disk_entry_t *)buffer;
        u32 slots = fs->cluster_size_bytes / sizeof(*entries);
        for (u32 i = 0; i < slots; i++) {
            fat32_walk_entry_t item = {0};
            u8 first_byte = (u8)entries[i].name[0];
            item.disk = entries[i];
            item.cluster = current;
            item.slot = i;
            if (ended) {
                if (strict && first_byte != 0U) {
                    result = VFS_ERR_BAD_FORMAT;
                    goto done;
                }
                continue;
            }
            if (first_byte == 0U) {
                if (strict && lfn.valid) {
                    result = VFS_ERR_BAD_FORMAT;
                    goto done;
                }
                item.unused = true;
                result = visit(context, current, &item);
                if (result != 0) goto done;
                ended = true;
                if (!complete) goto done;
                continue;
            }
            if (first_byte == 0xe5U) {
                if (strict && lfn.valid) {
                    result = VFS_ERR_BAD_FORMAT;
                    goto done;
                }
                item.has_lfn = lfn.valid;
                fat32_lfn_reset(&lfn);
                item.deleted = true;
                result = visit(context, current, &item);
                if (result != 0) goto done;
                continue;
            }
            if ((entries[i].attr & 0x0fU) == 0x0fU) {
                if (strict && (entries[i].name[0] & 0x40U) && lfn.valid) {
                    result = VFS_ERR_BAD_FORMAT;
                    goto done;
                }
                fat32_lfn_collect(&lfn,
                    (const fat32_lfn_entry_t *)&entries[i], current, i);
                if (strict && !lfn.valid) {
                    result = VFS_ERR_BAD_FORMAT;
                    goto done;
                }
                continue;
            }
            item.lfn = lfn;
            item.has_lfn = lfn.valid;
            if (strict && lfn.valid) {
                u32 length;
                if (!fat32_lfn_length(&lfn, &entries[i], &length)) {
                    result = VFS_ERR_BAD_FORMAT;
                    goto done;
                }
            }
            fat32_lfn_reset(&lfn);
            item.dot = entries[i].name[0] == '.' &&
                (entries[i].name[1] == ' ' || entries[i].name[1] == '.');
            result = visit(context, current, &item);
            if (result != 0) goto done;
        }
        u32 next;
        result = strict ? fat32_get_next_checked_view(sb, fs, current, &next, view)
                        : ((next = fat32_get_next_cluster(sb, fs, current)) ==
                           FAT32_CLUSTER_ERR ? VFS_ERR_IO : VFS_OK);
        if (result != VFS_OK) {
            break;
        }
        if (next == FAT32_EOC_MIN) {
            if (strict && lfn.valid) result = VFS_ERR_BAD_FORMAT;
            break;
        }
        if (next < 2U || next >= fs->total_clusters) {
            result = VFS_ERR_BAD_FORMAT;
            break;
        }
        if (power == span) {
            tortoise = current;
            if (power <= max_clusters / 2U) power *= 2U;
            span = 0U;
        }
        current = next;
        span++;
        if (current == tortoise) {
            result = VFS_ERR_BAD_FORMAT;
            break;
        }
        if (traversed + 1U == max_clusters)
            result = VFS_ERR_BUFFER_TOO_SMALL;
    }
done:
    if (!scratch_buffer) kfree(buffer);
    return result > 0 ? VFS_OK : result;
}

static int fat32_dir_walk_view(vfs_super_t *sb, u32 first, bool complete,
                          bool strict, fat32_walk_visit_t visit, void *context,
                          fat32_read_view_t *view)
{
    return fat32_dir_walk_limit(sb, first, complete, strict, visit, context,
                                view, FAT32_WALK_MAX_DIR_CLUSTERS, NULL);
}

static int fat32_dir_walk(vfs_super_t *sb, u32 first, bool complete,
                          bool strict, fat32_walk_visit_t visit, void *context)
{
    return fat32_dir_walk_view(sb, first, complete, strict, visit, context, NULL);
}

typedef enum { FAT32_QUERY_NAME, FAT32_QUERY_INDEX, FAT32_QUERY_CREATE } fat32_query_mode_t;
typedef struct {
    fat32_query_mode_t mode;
    const char *name;
    u32 wanted_index;
    u32 visible;
    bool complete;
    bool found;
    bool free_found;
    const fat32_walk_entry_t *exclude;
    fat32_walk_entry_t match;
    fat32_walk_entry_t free_slot;
} fat32_query_t;

/* readdir's ABI is 255 UTF-8 bytes. An unrepresentable LFN can still use a
 * portable short alias; an OEM-only entry is omitted, not emitted as invalid
 * UTF-8 or used to terminate listing before later readable entries. */
static bool fat32_entry_readable(const fat32_walk_entry_t *entry)
{
    u32 length, bytes = 0U;
    if (fat32_lfn_length(&entry->lfn, &entry->disk, &length)) {
        for (u32 i = 0; i < length;) {
            u32 value = fat32_lfn_next(&entry->lfn, &i);
            bytes += value < 0x80U ? 1U : value < 0x800U ? 2U :
                     value < 0x10000U ? 3U : 4U;
        }
        if (bytes < sizeof(((vfs_dirent_t *)0)->name)) return true;
    }
    return fat32_short_ascii(&entry->disk);
}

static int fat32_query_visit(void *context, u32 cluster,
                              const fat32_walk_entry_t *entry)
{
    fat32_query_t *query = (fat32_query_t *)context;
    (void)cluster;
    if (!entry) return 0;
    if (entry->deleted || entry->unused) {
        /* A deleted short entry may still have live LFN slots before it.
         * Reusing it would attach those slots to a different short name. */
        if (!query->free_found && !(entry->deleted && entry->has_lfn)) {
            query->free_slot = *entry;
            query->free_found = true;
        }
        return 0;
    }
    if (entry->dot) return 0;
    if (entry->disk.attr & 0x08U) {
        if (query->mode == FAT32_QUERY_CREATE &&
            fat32_match_name(&entry->disk, query->name))
            query->found = true;
        return 0;
    }
    /* A namespace mutation must exclude collisions with every live alias.
     * An unknown OEM encoding is unsupported, not evidence of corruption.
     * Complete NAME queries are used by unlink/rmdir/rename. Volume labels
     * are not object aliases and need not be decoded for that proof. */
    if ((query->mode == FAT32_QUERY_CREATE ||
         (query->mode == FAT32_QUERY_NAME && query->complete)) &&
        !fat32_short_ascii(&entry->disk)) return VFS_ERR_UNSUPPORTED;
    if (query->exclude && entry->cluster == query->exclude->cluster &&
        entry->slot == query->exclude->slot) return 0;
    if (query->mode == FAT32_QUERY_INDEX) {
        u32 length;
        if (!fat32_entry_readable(entry) &&
            !fat32_lfn_length(&entry->lfn, &entry->disk, &length)) return 0;
        if (query->visible++ != query->wanted_index) return 0;
    } else if (!fat32_match_entry(&entry->disk, &entry->lfn, query->name)) {
        return 0;
    }
    if (query->found && query->mode == FAT32_QUERY_NAME)
        return VFS_ERR_BAD_FORMAT;
    query->match = *entry;
    query->found = true;
    return query->mode == FAT32_QUERY_CREATE || query->complete ? 0 : 1;
}

static int fat32_dir_query_view(vfs_node_t *dir, fat32_query_t *query,
                                     fat32_read_view_t *view)
{
    if (query->mode != FAT32_QUERY_INDEX && !fat32_name_valid(query->name))
        return VFS_ERR_INVALID_PARAM;
    u32 cluster = fat32_node_cluster(dir);
    if (cluster == FAT32_CLUSTER_ERR) return VFS_ERR_UNCERTAIN;
    int result = fat32_dir_walk_view(dir->super, cluster,
                           query->mode == FAT32_QUERY_CREATE || query->complete,
                           query->mode == FAT32_QUERY_CREATE || query->complete,
                           fat32_query_visit, query, view);
    if (result == VFS_ERR_BAD_FORMAT)
        ((fat32_fs_t *)dir->super->private_data)->version_confident = false;
    return result;
}

static int fat32_dir_query(vfs_node_t *dir, fat32_query_t *query)
{
    return fat32_dir_query_view(dir, query, NULL);
}

static int fat32_readdir_checked(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry)
{
    if (!dir || !out_entry || !dir->super || !dir->super->private_data)
        return VFS_ERR_INVALID_PARAM;
    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    if (!block_device_id_is_live(fs->device_id)) return VFS_ERR_DEVICE_GONE;
    if (((fat32_fs_t *)dir->super->private_data)->namespace_hidden)
        return VFS_ERR_UNCERTAIN;
    fat32_query_t query = {.mode = FAT32_QUERY_INDEX, .wanted_index = index};
    int result = fat32_dir_query(dir, &query);
    if (result != VFS_OK) return block_device_id_is_live(fs->device_id)
        ? result : VFS_ERR_DEVICE_GONE;
    if (!query.found) return 0;
    vfs_dirent_t candidate = {0};
    fat32_walk_entry_t *item = &query.match;
    if (!fat32_lfn_encode(&item->lfn, &item->disk, candidate.name,
                           sizeof(candidate.name)) &&
        !fat32_format_83_name(&item->disk, candidate.name)) return VFS_ERR_NAME_TOO_LONG;
    u32 first = ((u32)item->disk.first_cluster_high << 16U) |
                item->disk.first_cluster_low;
    vfs_node_t *member = fat32_bind_node(
        dir->super, fs, item->cluster, item->slot, first,
        item->disk.file_size,
        (item->disk.attr & 0x10U) ? VFS_TYPE_DIRECTORY : VFS_TYPE_FILE);
    if (!member) return fs->version_confident ? VFS_ERR_NO_MEM : VFS_ERR_UNCERTAIN;
    candidate.inode = member->inode;
    candidate.type = member->type;
    *out_entry = candidate;
    return 1;
}

static bool fat32_readdir(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry)
{
    return fat32_readdir_checked(dir, index, out_entry) == 1;
}

static vfs_node_t *fat32_finddir(vfs_node_t *dir, const char *name)
{
    if (!dir || !name || !dir->super || !dir->super->private_data)
        return NULL;
    if (((fat32_fs_t *)dir->super->private_data)->namespace_hidden)
        return NULL;
    fat32_query_t query = {.mode = FAT32_QUERY_NAME, .name = name};
    if (fat32_dir_query(dir, &query) != VFS_OK || !query.found) return NULL;
    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    fat32_walk_entry_t *item = &query.match;
    u32 first = ((u32)item->disk.first_cluster_high << 16U) |
                item->disk.first_cluster_low;
    return fat32_bind_node(dir->super, fs, item->cluster, item->slot,
                           first, item->disk.file_size,
                           (item->disk.attr & 0x10U)
                               ? VFS_TYPE_DIRECTORY : VFS_TYPE_FILE);
}

/* Safe-create invokes this read-only query with operation_lock held. */
static int fat32_check_name_available_locked_view(vfs_node_t *dir, const char *name,
                                        fat32_walk_entry_t *out_free)
{
    fat32_read_view_t view;
    view.valid = 0;
    fat32_query_t query = {.mode = FAT32_QUERY_CREATE, .name = name};
    int result = fat32_dir_query_view(dir, &query, &view);
    if (result != VFS_OK) return result;
    if (query.found) return VFS_ERR_INVALID_PARAM;
    if (!query.free_found) return VFS_ERR_NO_SPACE;
    if (out_free) *out_free = query.free_slot;
    return VFS_OK;
}

static int fat32_dir_find(vfs_node_t *dir, const char *name,
                           fat32_walk_entry_t *out)
{
    fat32_query_t query = {.mode = FAT32_QUERY_NAME, .name = name,
                            .complete = true};
    int result = fat32_dir_query(dir, &query);
    if (result != VFS_OK) return result;
    if (!query.found) return VFS_ERR_NOT_FOUND;
    *out = query.match;
    return VFS_OK;
}

static int fat32_dir_load_slot(vfs_super_t *sb,
                                const fat32_walk_entry_t *slot, u8 **out)
{
    fat32_fs_t *fs = (fat32_fs_t *)sb->private_data;
    u8 *buffer = (u8 *)kmalloc(fs->cluster_size_bytes);
    if (!buffer) return VFS_ERR_NO_MEM;
    if (!fat32_mounted_read(sb, fat32_cluster_to_lba(fs, slot->cluster),
                            fs->sectors_per_cluster, buffer)) {
        kfree(buffer);
        return VFS_ERR_IO;
    }
    if (slot->slot >= fs->cluster_size_bytes / sizeof(slot->disk) ||
        memcmp(buffer + slot->slot * sizeof(slot->disk), &slot->disk,
               sizeof(slot->disk)) != 0) {
        kfree(buffer);
        fs->version_confident = false;
        return VFS_ERR_UNCERTAIN;
    }
    *out = buffer;
    return VFS_OK;
}

static u32 fat32_get_next_cluster(vfs_super_t *sb, fat32_fs_t *fs, u32 current_cluster) {
    if (!sb || !fs || current_cluster < 2 ||
        current_cluster >= fs->total_clusters) {
        return FAT32_CLUSTER_ERR;
    }

    u32 fat_offset = current_cluster * 4;
    u32 active = (fs->bpb.ext_flags & 0x80U)
        ? fs->bpb.ext_flags & 0x0fU : 0U;
    if (active >= fs->fat_count) return FAT32_CLUSTER_ERR;
    u32 fat_sector = fs->fat_start_lba +
        active * fs->fat_size_sectors +
        (fat_offset / fs->bytes_per_sector);
    u32 entry_offset = fat_offset % fs->bytes_per_sector;

    u8 *sector_buf = (u8 *)kmalloc(fs->bytes_per_sector);
    if (!sector_buf) {
        return FAT32_CLUSTER_ERR;
    }

    if (!fat32_mounted_read(sb, fat_sector, 1, sector_buf)) {
        kfree(sector_buf);
        return FAT32_CLUSTER_ERR;
    }

    u32 next_cluster = (*(u32 *)(sector_buf + entry_offset)) & 0x0FFFFFFF;
    kfree(sector_buf);

    if (next_cluster >= FAT32_EOC_MIN) {
        return FAT32_EOC_MIN;
    }

    return next_cluster;
}

/* A proof must not silently choose one of disagreeing FAT copies. */
static int fat32_get_next_checked_view(vfs_super_t *sb, fat32_fs_t *fs,
                                   u32 cluster, u32 *out, fat32_read_view_t *view)
{
    u8 scratch[512];
    if (view && (!sb || !fs || sb->private_data != fs || !sb->dev ||
        sb->dev->id != fs->device_id || !block_device_id_is_live(fs->device_id))) {
        view->valid = 0;
        return VFS_ERR_IO;
    }
    u32 value = 0;
    if (view && view->valid &&
        (view->owner != fs || view->device_id != fs->device_id)) view->valid = 0;
    if (!sb || !fs || !out || cluster < 2U ||
        cluster >= fs->total_clusters || !fs->fat_count)
        return VFS_ERR_BAD_FORMAT;
    u32 offset = cluster * 4U;
    u32 sector_index = offset / 512U;
    if (sector_index >= fs->fat_size_sectors) return VFS_ERR_BAD_FORMAT;
    bool mirrored = !(fs->bpb.ext_flags & 0x80U);
    u32 active = fs->bpb.ext_flags & 0x0fU;
    if (!mirrored && active >= fs->fat_count) return VFS_ERR_BAD_FORMAT;
    u32 copies = mirrored ? fs->fat_count : 1U;
    for (u32 copy = 0; copy < copies; copy++) {
        u32 selected = mirrored ? copy : active;
        u64 lba = (u64)fs->fat_start_lba +
                  (u64)selected * fs->fat_size_sectors + sector_index;
        u8 *sector = view && copy < 4U ? view->sectors[copy] : scratch;
        if (!view || copy >= 4U || !(view->valid & (1U << copy)) ||
            view->lbas[copy] != lba) {
            if (!fat32_mounted_read(sb, lba, 1U, sector)) {
                if (view) view->valid = 0;
                return VFS_ERR_IO;
            }
            if (view && copy < 4U) {
                view->owner = fs; view->device_id = fs->device_id;
                view->lbas[copy] = lba;
                view->valid |= (u8)(1U << copy);
            }
        }
        u32 at = offset % 512U;
        u32 current = ((u32)sector[at] | ((u32)sector[at + 1U] << 8U) |
                       ((u32)sector[at + 2U] << 16U) |
                       ((u32)sector[at + 3U] << 24U)) & 0x0fffffffU;
        if (copy && current != value) return VFS_ERR_BAD_FORMAT;
        value = current;
    }
    *out = value >= FAT32_EOC_MIN ? FAT32_EOC_MIN : value;
    return VFS_OK;
}

static int fat32_get_next_checked(vfs_super_t *sb, fat32_fs_t *fs,
                                   u32 cluster, u32 *out)
{
    return fat32_get_next_checked_view(sb, fs, cluster, out, NULL);
}

/* Read-only check for the mapped chain of a snapshot-sized file. Brent's
 * cycle check uses constant memory; the 2 MiB bound keeps malformed or
 * overallocated chains from causing an unbounded FAT scan. This does not
 * establish uniqueness against directory entries not yet traversed. */
static int fat32_mapping_check(vfs_node_t *node, u32 first, u32 size)
{
    fat32_fs_t *fs = (fat32_fs_t *)node->super->private_data;
    u32 current = first, tortoise = first;
    u32 power = 1, span = 0, count = 0;
    u32 required = size / fs->cluster_size_bytes +
                   (size % fs->cluster_size_bytes != 0U);
    u32 limit = MG_FILE_CONTENT_MAX_BYTES / fs->cluster_size_bytes;

    if (!first) return size ? VFS_ERR_BAD_FORMAT : VFS_OK;
    if (!limit || required > limit) return VFS_ERR_BUFFER_TOO_SMALL;
    while (count < limit) {
        u32 next;
        if (current < 2U || current >= fs->total_clusters)
            return VFS_ERR_BAD_FORMAT;
        next = fat32_get_next_cluster(node->super, fs, current);
        if (next == FAT32_CLUSTER_ERR) return VFS_ERR_IO;
        count++;
        if (next == FAT32_EOC_MIN)
            return count >= required ? VFS_OK : VFS_ERR_BAD_FORMAT;
        if (next < 2U || next >= fs->total_clusters)
            return VFS_ERR_BAD_FORMAT;
        if (power == span) {
            tortoise = current;
            power *= 2U;
            span = 0;
        }
        current = next;
        span++;
        if (current == tortoise) return VFS_ERR_BAD_FORMAT;
    }
    return VFS_ERR_BUFFER_TOO_SMALL;
}

u32 fat32_get_cluster_link(vfs_super_t *sb, u32 cluster) {
    if (!sb || !sb->private_data) return FAT32_CLUSTER_ERR;
    fat32_fs_t *fs = (fat32_fs_t *)sb->private_data;
    return fat32_get_next_cluster(sb, fs, cluster);
}

static int fat32_read_checked(vfs_node_t *node, u64 offset, u64 size, void *buffer, u64 *out_bytes) {
    *out_bytes = 0;
    if (!node || !buffer || node->type != VFS_TYPE_FILE || !node->super || !node->super->private_data) {
        return VFS_ERR_INVALID_PARAM;
    }
    fat32_fs_t *fs = (fat32_fs_t *)node->super->private_data;
    fat32_node_state_t *state = fat32_state(node);
    if (!state || !state->alive) return VFS_ERR_NOT_FOUND;
    if (!fs->version_confident || !state->version_valid) return VFS_ERR_UNCERTAIN;

    if (!size || offset >= node->size) {
        return VFS_OK;
    }

    u64 avail = node->size - offset;
    if (size > avail) {
        size = avail;
    }

    u32 cluster_size = fs->cluster_size_bytes;

    u32 curr_cluster = fat32_node_cluster(node);
    u64 skip_clusters = offset / cluster_size;
    u64 offset_in_cluster = offset % cluster_size;

    /* Advance to starting cluster for requested offset */
    for (u64 i = 0; i < skip_clusters; i++) {
        curr_cluster = fat32_get_next_cluster(node->super, fs, curr_cluster);
        if (curr_cluster < 2U || curr_cluster >= fs->total_clusters ||
            curr_cluster >= FAT32_EOC_MIN) {
            return VFS_ERR_IO;
        }
    }

    u8 *cluster_buf = (u8 *)kmalloc(cluster_size);
    if (!cluster_buf) {
        return VFS_ERR_NO_MEM;
    }

    u64 bytes_read = 0;
    u64 bytes_remaining = size;
    u8 *out_ptr = (u8 *)buffer;

    while (bytes_remaining > 0 && curr_cluster >= 2U &&
           curr_cluster < fs->total_clusters &&
           curr_cluster < FAT32_EOC_MIN) {
        u64 lba = fat32_cluster_to_lba(fs, curr_cluster);
        if (!fat32_mounted_read(node->super, lba, fs->sectors_per_cluster, cluster_buf)) {
            break;
        }

        u64 chunk_avail = cluster_size - offset_in_cluster;
        u64 chunk_size = (bytes_remaining < chunk_avail) ? bytes_remaining : chunk_avail;

        memcpy(out_ptr, cluster_buf + offset_in_cluster, chunk_size);

        bytes_read += chunk_size;
        bytes_remaining -= chunk_size;
        out_ptr += chunk_size;

        offset_in_cluster = 0;

        if (bytes_remaining > 0) {
            curr_cluster = fat32_get_next_cluster(node->super, fs, curr_cluster);
        }
    }

    kfree(cluster_buf);
    *out_bytes = bytes_read;
    return bytes_remaining ? VFS_ERR_IO : VFS_OK;
}

static u64 fat32_read(vfs_node_t *node, u64 offset, u64 size, void *buffer)
{
    u64 bytes = 0;
    (void)fat32_read_checked(node, offset, size, buffer, &bytes);
    return bytes;
}

/* Legacy byte-count dispatch cannot represent an uncertain outcome or transfer
 * a pin. All ordinary VFS paths select write_pinned/truncate_pinned instead. */
static u64 fat32_write(vfs_node_t *node, u64 offset, u64 size, const void *buffer)
{
    (void)node; (void)offset; (void)size; (void)buffer;
    return 0U;
}

static int fat32_truncate(vfs_node_t *node)
{
    (void)node;
    return VFS_ERR_UNSUPPORTED;
}

typedef struct { u32 first, parent; } fat32_scan_directory_t;
#define FAT32_SCAN_QUEUE_ITEMS 510U
/* One allocation stays below 4 KiB on the native x86-64 target. */
typedef struct fat32_scan_queue {
    struct fat32_scan_queue *next;
    u32 head, count;
    fat32_scan_directory_t items[FAT32_SCAN_QUEUE_ITEMS];
} fat32_scan_queue_t;
_Static_assert(sizeof(fat32_scan_queue_t) <= FAT32_SCAN_CHUNK_BYTES,
               "ownership queue allocations must fit one chunk");
typedef struct {
    vfs_super_t *sb;
    fat32_fs_t *fs;
    fat32_node_state_t *target;
    u8 **owned;
    u8 *directory_buffer;
    u32 chunk_count;
    fat32_scan_queue_t *pending, *tail;
    usize allocated;
    u32 current, parent;
    bool found_target, next_view;
    fat32_read_view_t view, alternate_view;
} fat32_ownership_scan_t;

/* No allocation in this read-only proof survives its operation-lock hold. */
static void *fat32_scan_alloc(fat32_ownership_scan_t *scan, usize bytes)
{
    if (!bytes || scan->allocated > FAT32_SCAN_MEMORY_BUDGET ||
        bytes > FAT32_SCAN_MEMORY_BUDGET - scan->allocated) return NULL;
    void *memory = kmalloc(bytes);
    if (memory) scan->allocated += bytes;
    return memory;
}

static int fat32_scan_enqueue(fat32_ownership_scan_t *scan, u32 first, u32 parent)
{
    if (!scan->tail || scan->tail->count == FAT32_SCAN_QUEUE_ITEMS) {
        usize bytes = sizeof(fat32_scan_queue_t);
        if (bytes > FAT32_SCAN_MEMORY_BUDGET - scan->allocated)
            return VFS_ERR_BUFFER_TOO_SMALL;
        fat32_scan_queue_t *page = fat32_scan_alloc(scan, bytes);
        if (!page) return VFS_ERR_NO_MEM;
        page->next = NULL;
        page->head = page->count = 0U;
        if (scan->tail) scan->tail->next = page;
        else scan->pending = page;
        scan->tail = page;
    }
    scan->tail->items[scan->tail->count++] =
        (fat32_scan_directory_t){first, parent};
    return VFS_OK;
}

static bool fat32_scan_dequeue(fat32_ownership_scan_t *scan,
                               fat32_scan_directory_t *out)
{
    fat32_scan_queue_t *page = scan->pending;
    if (!page || page->head == page->count) return false;
    *out = page->items[page->head++];
    if (page->head == page->count) {
        if (page->next) {
            scan->pending = page->next;
            scan->allocated -= sizeof(*page);
            kfree(page);
        } else {
            /* Reuse the final page for a narrow/deep tree. */
            page->head = page->count = 0U;
        }
    }
    return true;
}

static void fat32_scan_free(fat32_ownership_scan_t *scan)
{
    while (scan->pending) {
        fat32_scan_queue_t *page = scan->pending;
        scan->pending = page->next;
        kfree(page);
    }
    if (scan->owned)
        for (u32 i = 0U; i < scan->chunk_count; i++) kfree(scan->owned[i]);
    kfree(scan->owned);
    kfree(scan->directory_buffer);
}

static int fat32_claim_cluster(fat32_ownership_scan_t *scan, u32 cluster)
{
    if (cluster < 2U || cluster >= scan->fs->total_clusters)
        return VFS_ERR_BAD_FORMAT;
    u32 page = cluster / FAT32_SCAN_CHUNK_CLUSTERS;
    u32 within = cluster % FAT32_SCAN_CHUNK_CLUSTERS;
    if (!scan->owned[page]) {
        u32 remaining = scan->fs->total_clusters -
                        page * FAT32_SCAN_CHUNK_CLUSTERS;
        usize bytes = remaining < FAT32_SCAN_CHUNK_CLUSTERS
            ? ((usize)remaining + 7U) / 8U : FAT32_SCAN_CHUNK_BYTES;
        if (bytes > FAT32_SCAN_MEMORY_BUDGET - scan->allocated)
            return VFS_ERR_BUFFER_TOO_SMALL;
        scan->owned[page] = fat32_scan_alloc(scan, bytes);
        if (!scan->owned[page]) return VFS_ERR_NO_MEM;
        memset(scan->owned[page], 0, bytes);
    }
    u8 bit = (u8)(1U << (within % 8U));
    u8 *byte = &scan->owned[page][within / 8U];
    if (*byte & bit) return VFS_ERR_BAD_FORMAT;
    *byte |= bit;
    return VFS_OK;
}

static int fat32_claim_file_chain(fat32_ownership_scan_t *scan,
                                   u32 first, u32 size)
{
    if (!first) return size ? VFS_ERR_BAD_FORMAT : VFS_OK;
    u32 current = first;
    u64 capacity = 0;
    for (;;) {
        int result = fat32_claim_cluster(scan, current);
        if (result != VFS_OK) return result;
        capacity += scan->fs->cluster_size_bytes;
        u32 next;
        /* Two mirrored-sector views keep alternating fragmented runs cached.
         * Both expire with this proof; every lookup still checks device identity. */
        u32 active = (scan->fs->bpb.ext_flags & 0x80U)
            ? scan->fs->bpb.ext_flags & 0x0fU : 0U;
        u64 lba = (u64)scan->fs->fat_start_lba +
            (u64)active * scan->fs->fat_size_sectors + current / 128U;
        fat32_read_view_t *view = &scan->view;
        if (!(view->valid & 1U) || view->lbas[0] != lba) {
            view = &scan->alternate_view;
            if (!(view->valid & 1U) || view->lbas[0] != lba) {
                scan->next_view = !scan->next_view;
                view = scan->next_view ? &scan->alternate_view : &scan->view;
            }
        }
        result = fat32_get_next_checked_view(scan->sb, scan->fs, current, &next, view);
        if (result != VFS_OK) return result;
        if (next == FAT32_EOC_MIN)
            return capacity >= size ? VFS_OK : VFS_ERR_BAD_FORMAT;
        if (next < 2U || next >= scan->fs->total_clusters)
            return VFS_ERR_BAD_FORMAT;
        current = next;
    }
}

static u32 fat32_entry_first(const fat32_on_disk_entry_t *entry)
{
    return ((u32)entry->first_cluster_high << 16U) |
           entry->first_cluster_low;
}

static int fat32_ownership_visit(void *context, u32 cluster,
                                  const fat32_walk_entry_t *entry)
{
    fat32_ownership_scan_t *scan = (fat32_ownership_scan_t *)context;
    fat32_fs_t *fs = scan->fs;
    if (!entry) return fat32_claim_cluster(scan, cluster);
    if (entry->deleted || entry->unused) return 0;
    const fat32_on_disk_entry_t *disk = &entry->disk;
    u32 first = fat32_entry_first(disk);
    if (disk->attr & 0xc0U) return VFS_ERR_BAD_FORMAT;
    if (entry->dot) {
        static const char dot[11] = {'.',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '};
        static const char dotdot[11] = {'.','.',' ',' ',' ',' ',' ',' ',' ',' ',' '};
        bool self = memcmp(disk->name, dot, 11U) == 0;
        bool ancestor = memcmp(disk->name, dotdot, 11U) == 0;
        if (!(disk->attr & 0x10U) || (disk->attr & 0x08U) ||
            entry->has_lfn || disk->file_size ||
            (!self && !ancestor) ||
            (self && first != scan->current) ||
            (ancestor && first != scan->parent &&
             !(scan->parent == fs->root_cluster && first == 0U)))
            return VFS_ERR_BAD_FORMAT;
        return 0;
    }
    if (disk->attr & 0x08U)
        return first || disk->file_size || (disk->attr & 0x10U)
            ? VFS_ERR_BAD_FORMAT : 0;
    if (scan->target && scan->target->node != scan->sb->root_node &&
        scan->target->parent_cluster == entry->cluster &&
        scan->target->entry_index == entry->slot) {
        bool directory = scan->target->node->type == VFS_TYPE_DIRECTORY;
        if (!!(disk->attr & 0x10U) != directory ||
            first != scan->target->first_cluster ||
            disk->file_size != scan->target->node->size)
            return VFS_ERR_BAD_FORMAT;
        scan->found_target = true;
    }
    if (disk->attr & 0x10U) {
        if (first < 2U || first >= fs->total_clusters || disk->file_size)
            return VFS_ERR_BAD_FORMAT;
        return fat32_scan_enqueue(scan, first, scan->current);
    }
    return fat32_claim_file_chain(scan, first, disk->file_size);
}

/* Caller holds the mount operation lock throughout this scan and the later
 * decision that consumes it. No persistent writes, allocations, or frees are
 * made on media. The result is not a certificate after unlocking. */
int fat32_ownership_preflight_locked(vfs_node_t *target)
{
    fat32_ownership_scan_t scan = {0};
    int result = VFS_OK;
    usize index_bytes;
    if (!target || (target->type != VFS_TYPE_FILE &&
                    target->type != VFS_TYPE_DIRECTORY))
        return VFS_ERR_INVALID_PARAM;
    if (!target->super || !target->super->private_data)
        return VFS_ERR_DEVICE_GONE;
    scan.sb = target->super;
    scan.fs = (fat32_fs_t *)target->super->private_data;
    scan.target = fat32_state(target);
    if (!scan.fs->version_confident || !scan.target ||
        !scan.target->version_valid) return VFS_ERR_UNCERTAIN;
    if (!scan.target->alive) return VFS_ERR_NOT_FOUND;
    if (target == scan.sb->root_node) {
        if (scan.target->first_cluster != scan.fs->root_cluster) {
            scan.fs->version_confident = false;
            return VFS_ERR_BAD_FORMAT;
        }
        scan.found_target = true;
    }
    if (!scan.fs->cluster_size_bytes || scan.fs->cluster_size_bytes > 65536U ||
        !scan.fs->fat_count || scan.fs->fat_count > 4U ||
        scan.fs->total_clusters <= 2U || scan.fs->total_clusters >= FAT32_EOC_MIN)
        return VFS_ERR_BAD_FORMAT;
    scan.chunk_count = (scan.fs->total_clusters - 1U) /
                        FAT32_SCAN_CHUNK_CLUSTERS + 1U;
    /* FAT32's 28-bit cluster range bounds this index to 64 KiB. No volume-
     * sized contiguous bitmap or file-sized allocation is needed. */
    index_bytes = (usize)scan.chunk_count * sizeof(*scan.owned);
    scan.owned = fat32_scan_alloc(&scan, index_bytes);
    if (!scan.owned) { result = VFS_ERR_NO_MEM; goto done; }
    memset(scan.owned, 0, index_bytes);
    scan.directory_buffer = fat32_scan_alloc(&scan, scan.fs->cluster_size_bytes);
    if (!scan.directory_buffer) { result = VFS_ERR_NO_MEM; goto done; }
    result = fat32_scan_enqueue(&scan, scan.fs->root_cluster,
                                scan.fs->root_cluster);
    if (result != VFS_OK) goto done;
    fat32_scan_directory_t job;
    while (fat32_scan_dequeue(&scan, &job)) {
        scan.current = job.first;
        scan.parent = job.parent;
        result = fat32_dir_walk_limit(scan.sb, job.first, true, true,
            fat32_ownership_visit, &scan, &scan.view,
            scan.fs->total_clusters - 2U, scan.directory_buffer);
        if (result != VFS_OK) break;
    }
    if (result == VFS_OK && !scan.found_target) result = VFS_ERR_NOT_FOUND;
    if (result == VFS_ERR_BAD_FORMAT) scan.fs->version_confident = false;
done:
    fat32_scan_free(&scan);
    return result;
}

static int fat32_file_facts(vfs_node_t *node, vfs_file_facts_t *out)
{
    fat32_node_state_t *state = fat32_state(node);
    fat32_fs_t *fs;
    fat32_on_disk_entry_t *entry;
    u8 *cluster_buf;
    int result = VFS_OK;
    u32 permissions;

    if (!node || !out || node->type != VFS_TYPE_FILE || !node->super ||
        !node->super->private_data) return VFS_ERR_UNSUPPORTED;
    fs = (fat32_fs_t *)node->super->private_data;
    if (!fs->version_confident || !state || !state->version_valid)
        return VFS_ERR_UNCERTAIN;
    if (!state->alive) return VFS_ERR_NOT_FOUND;
    if (state->parent_cluster < 2U ||
        state->parent_cluster >= fs->total_clusters ||
        state->entry_index >= fs->cluster_size_bytes / sizeof(*entry))
        return VFS_ERR_BAD_FORMAT;
    cluster_buf = (u8 *)kmalloc(fs->cluster_size_bytes);
    if (!cluster_buf) return VFS_ERR_NO_MEM;
    if (!fat32_mounted_read(node->super,
                            fat32_cluster_to_lba(fs, state->parent_cluster),
                            fs->sectors_per_cluster, cluster_buf)) {
        kfree(cluster_buf);
        return VFS_ERR_IO;
    }
    entry = &((fat32_on_disk_entry_t *)cluster_buf)[state->entry_index];
    if ((u8)entry->name[0] == 0U || (u8)entry->name[0] == 0xE5U)
        result = VFS_ERR_NOT_FOUND;
    else if ((entry->attr & 0x18U) != 0U ||
             (((u32)entry->first_cluster_high << 16U) |
              entry->first_cluster_low) != state->first_cluster ||
             entry->file_size != node->size) {
        state->version_valid = false;
        result = VFS_ERR_UNCERTAIN;
    }
    if (result == VFS_OK) {
        result = fat32_mapping_check(node, state->first_cluster,
                                     entry->file_size);
        if (result == VFS_ERR_BAD_FORMAT) {
            state->version_valid = false;
            fs->version_confident = false;
        }
    }
    if (result == VFS_OK) {
        permissions = node->permissions;
        if (entry->attr & 1U)
            permissions &= ~(VFS_PERMISSION_OWNER_WRITE |
                             VFS_PERMISSION_OTHER_WRITE);
        *out = (vfs_file_facts_t){
            .record_id = node->inode,
            .generation = state->generation,
            .size = entry->file_size,
            .owner_uid = node->owner_uid,
            .permissions = permissions,
        };
    }
    kfree(cluster_buf);
    return result;
}

static int fat32_snapshot_stream(vfs_node_t *node, usize capacity,
                                  vfs_snapshot_sink_t sink, void *context,
                                  mg_file_lines_state_t *state)
{
    vfs_file_facts_t facts;
    u8 chunk[4096];
    int result;

    if (!sink || !state || capacity > MG_FILE_CONTENT_MAX_BYTES)
        return VFS_ERR_INVALID_PARAM;
    result = fat32_file_facts(node, &facts);
    if (result != VFS_OK) return result;
    if (facts.size > capacity) return VFS_ERR_BUFFER_TOO_SMALL;
    for (u64 offset = 0; offset < facts.size;) {
        usize count = facts.size - offset > sizeof(chunk)
            ? sizeof(chunk) : (usize)(facts.size - offset);
        if (fat32_read(node, offset, count, chunk) != count)
            return VFS_ERR_IO;
        result = sink(context, offset, chunk, count);
        if (result != VFS_OK) return result;
        offset += count;
    }
    *state = (mg_file_lines_state_t){facts.record_id, facts.generation,
                                      facts.size, 0};
    return VFS_OK;
}

typedef enum {
    FAT32_REPLACE_STAGING,
    FAT32_REPLACE_STAGING_FLUSHING,
    FAT32_REPLACE_CANCELLING,
    FAT32_REPLACE_CANCELLATION_FLUSHING,
    FAT32_REPLACE_DIR_ALLOCATING,
    FAT32_REPLACE_DIR_INITIALIZING,
    FAT32_REPLACE_DIR_LINKING,
    FAT32_REPLACE_DIR_FLUSHING,
    FAT32_REPLACE_PUBLISHING,
    FAT32_REPLACE_PUBLICATION_FLUSHING,
    FAT32_REPLACE_DIR_RELOCATING,
    FAT32_REPLACE_UNLINK_PUBLISHING,
    FAT32_REPLACE_UNLINK_CLEANUP,
    FAT32_REPLACE_RECLAIMING,
    FAT32_REPLACE_FSINFO,
    FAT32_REPLACE_COMPLETION_FLUSHING,
    FAT32_REPLACE_HANDOFF_PUBLISHED,
    FAT32_REPLACE_HANDOFF_CANCELLED,
} fat32_replace_phase_t;

#define FAT32_REPLACE_FLUSH_STAGED    0x01U
#define FAT32_REPLACE_FLUSH_PUBLISHED 0x02U
#define FAT32_REPLACE_FLUSH_COMPLETED 0x04U
#define FAT32_REPLACE_FLUSH_CANCELLED 0x08U

typedef struct {
    u64 sector[2][4];
    u8 before[2][4][512], proposed[2][4][512];
} fat32_growth_second_images_t;

/* Namespace publication uses this inventory. The second pair of FAT images is
 * allocated only when a 21-entry set requires two appended clusters. */
typedef struct {
    u64 marker_sector;
    u64 fat_sector[2][4];
    u32 marker_cluster, marker_slot, tail_cluster, candidate;
    u32 second_candidate;
    u32 target_cluster, target_slot;
    u32 chain_clusters, zero_sectors_completed, fat_copies_completed[4];
    u8 marker_before[512], marker_proposed[512];
    u8 fat_before[2][4][512], fat_proposed[2][4][512];
    fat32_growth_second_images_t *second_images;
    bool has_marker, append, candidate_recorded, candidate_allocated;
    bool second_candidate_recorded, second_candidate_allocated;
    bool zero_attempted;
    bool link_attempted, link_confirmed, entry_written, marker_attempted;
    u8 append_count, links_confirmed;
    /* 0/1 record the first allocation/link; 2/3 record the second.
     * 4 disables capture for ordinary file allocation and cancellation. */
    u8 fat_step;
} fat32_directory_growth_t;

/* A FAT32 LFN has at most 20 entries. With its short entry, the contiguous
 * set touches at most three 512-byte sectors, even across cluster boundaries.
 * Sector zero contains the short entry and is the deletion boundary. */
#define FAT32_UNLINK_MAX_SECTORS 3U
typedef struct {
    u32 count, write_attempted, writes_confirmed;
    u64 sector[FAT32_UNLINK_MAX_SECTORS];
    u8 before[FAT32_UNLINK_MAX_SECTORS][512];
    u8 proposed[FAT32_UNLINK_MAX_SECTORS][512];
    bool short_confirmed;
} fat32_lfn_retirement_t;

typedef struct {
    u32 count, short_image, visibility_image;
    u32 attempted_mask, confirmed_mask;
    u8 fresh_index[FAT32_UNLINK_MAX_SECTORS];
    u32 fresh_part[FAT32_UNLINK_MAX_SECTORS];
    u64 sector[FAT32_UNLINK_MAX_SECTORS];
    u8 before[FAT32_UNLINK_MAX_SECTORS][512];
    u8 proposed[FAT32_UNLINK_MAX_SECTORS][512];
    bool marker_visibility, preparation_flushed, short_flushed;
} fat32_lfn_creation_t;

/* Original source images stay immutable. These predecessors retain the
 * confirmed destination changes when retirement shares their sectors. */
typedef struct {
    u64 parent_sector;
    u8 parent_before[512], parent_proposed[512];
    u8 retirement_predecessor[FAT32_UNLINK_MAX_SECTORS][512];
    u32 destination_cluster, destination_slot;
    bool parent_attempted, parent_confirmed, parent_flushed;
    bool destination_attempted, destination_confirmed, source_retired, inplace;
} fat32_rename_state_t;

/* Ordinary I/O retains one pending data sector, a private fresh chain and
 * a candidate not yet linked to that chain. No file-sized inventories. */
typedef struct {
    u64 data_sector, confirmed_user_bytes, data_sectors_confirmed;
    u64 write_offset, requested_length, zero_bytes_confirmed;
    u32 old_first, old_size, old_count, old_tail;
    u32 fresh_first, fresh_tail, fresh_count, candidate, zero_progress;
    u32 io_cluster, io_index, kept_tail, removed_first, removed_count;
    bool candidate_allocated, candidate_link_attempted, old_tail_link_attempted;
    bool old_tail_linked, data_pending, data_before_valid, mutation_attempted;
    bool directory_attempted, directory_confirmed, io_valid;
    bool staging_flushed, publication_flushed, cleanup_flushed;
    bool allocations_changed, fat_attempted, generation_advanced;
    u8 fat_role;
    u8 data_before[512], data_proposed[512];
} fat32_ordinary_t;

/* All inventory and both publication images survive an uncertain outcome.
 * The pin retains the superblock and this mount-owned object after removal. */
typedef struct fat32_replace_transaction {
    u32 *old_clusters;
    u32 *fresh_clusters;
    u8 *data_buffer;
    fat32_directory_growth_t *growth;
    fat32_lfn_retirement_t *retirement;
    fat32_lfn_creation_t *lfn_creation;
    fat32_ordinary_t *ordinary;
    fat32_rename_state_t *rename;
    u32 old_count, fresh_capacity, fresh_count;
    u32 cancellation_cursor, reclamation_cursor, fsinfo_cursor;
    /* File retirement follows the proved chain without a file-sized ledger.
     * Retain the successor before clearing any configured FAT copy. */
    u32 reclaim_cluster, reclaim_successor;
    bool reclaim_streaming, reclaim_successor_valid;
    u32 new_first, new_size;
    u64 pin_id;
    u64 record_id, expected_generation;
    u64 directory_sector;
    u64 fsinfo_sectors[2];
    u32 fsinfo_count;
    u64 metadata_sector;
    u32 metadata_cluster, metadata_partner, metadata_copy, metadata_copies_completed;
    fat32_replace_phase_t phase;
    bool handoff_pending;
    bool metadata_write_pending;
    bool creating;
    bool conditional_replacement;
    bool publication_attempted, publication_confirmed;
    u8 replacement_flush_attempted, replacement_flush_confirmed;
    fat32_node_state_t *created_state;
    u8 old_sector[512], proposed_sector[512];
    u8 metadata_before[512], metadata_proposed[512];
} fat32_replace_transaction_t;

static void fat32_replace_free(fat32_replace_transaction_t *tx)
{
    if (!tx) return;
    kfree(tx->data_buffer);
    if (tx->growth) kfree(tx->growth->second_images);
    kfree(tx->growth);
    kfree(tx->retirement);
    kfree(tx->lfn_creation);
    kfree(tx->ordinary);
    kfree(tx->rename);
    kfree(tx->fresh_clusters);
    kfree(tx->old_clusters);
    kfree(tx->created_state);
    kfree(tx);
}

/* Read every configured FAT copy before writing any. A failed callback can
 * have changed that copy; it is never retried or treated as a clean error.
 * A partner is the previous fresh EOC in this same sector. Combining its
 * link with allocation preserves complete images for the pending copy;
 * both clusters already belong to the transaction's fresh ledger. */
static int fat32_replace_fat_update_pair(vfs_super_t *sb, fat32_fs_t *fs,
                                     fat32_replace_transaction_t *tx,
                                     u32 cluster, u32 expected, u32 value,
                                     u32 partner)
{
    u8 sectors[4][512];
    u32 original[4];
    u32 partner_original[4];
    u32 offset = cluster * 4U;
    u32 sector_index = offset / 512U;
    u32 at = offset % 512U;
    bool mirrored = !(fs->bpb.ext_flags & 0x80U);
    u32 active = fs->bpb.ext_flags & 0x0fU;
    u32 copies = mirrored ? fs->fat_count : 1U;

    if (cluster < 2U || cluster >= fs->total_clusters ||
        sector_index >= fs->fat_size_sectors || !copies || copies > 4U ||
        (!mirrored && active >= fs->fat_count) ||
        (partner && (partner < 2U || partner >= fs->total_clusters ||
         partner == cluster || partner / 128U != cluster / 128U)))
        return VFS_ERR_BAD_FORMAT;
    for (u32 i = 0; i < copies; i++) {
        u32 selected = mirrored ? i : active;
        u64 sector = (u64)fs->fat_start_lba +
                     (u64)selected * fs->fat_size_sectors + sector_index;
        if (!fat32_mounted_read(sb, sector, 1U, sectors[i])) return VFS_ERR_IO;
        u32 raw = (u32)sectors[i][at] |
                  ((u32)sectors[i][at + 1U] << 8U) |
                  ((u32)sectors[i][at + 2U] << 16U) |
                  ((u32)sectors[i][at + 3U] << 24U);
        u32 found = raw & 0x0fffffffU;
        if (found >= FAT32_EOC_MIN) found = FAT32_EOC_MIN;
        if (found != expected) return VFS_ERR_BAD_FORMAT;
        original[i] = raw;
        fat32_store_le32(sectors[i] + at,
                          (raw & 0xf0000000U) | (value & 0x0fffffffU));
        if (partner) {
            u32 p = (partner % 128U) * 4U;
            u32 prior = (u32)sectors[i][p] | ((u32)sectors[i][p + 1U] << 8U) |
                        ((u32)sectors[i][p + 2U] << 16U) |
                        ((u32)sectors[i][p + 3U] << 24U);
            if ((prior & 0x0fffffffU) < FAT32_EOC_MIN) return VFS_ERR_BAD_FORMAT;
            partner_original[i] = prior;
            fat32_store_le32(sectors[i] + p, (prior & 0xf0000000U) | cluster);
        }
    }
    for (u32 i = 0; i < copies; i++) {
        u32 selected = mirrored ? i : active;
        u64 sector = (u64)fs->fat_start_lba +
                     (u64)selected * fs->fat_size_sectors + sector_index;
        tx->metadata_sector = sector;
        tx->metadata_cluster = cluster;
        tx->metadata_partner = partner;
        tx->metadata_copy = selected;
        tx->metadata_copies_completed = i;
        memcpy(tx->metadata_before, sectors[i], 512U);
        fat32_store_le32(tx->metadata_before + at, original[i]);
        if (partner) fat32_store_le32(tx->metadata_before +
                                     (partner % 128U) * 4U, partner_original[i]);
        memcpy(tx->metadata_proposed, sectors[i], 512U);
        if (tx->growth && tx->growth->fat_step < 4U) {
            fat32_directory_growth_t *growth = tx->growth;
            u32 step = growth->fat_step;
            if (step < 2U) {
                growth->fat_sector[step][i] = sector;
                memcpy(growth->fat_before[step][i], tx->metadata_before, 512U);
                memcpy(growth->fat_proposed[step][i], sectors[i], 512U);
            } else {
                fat32_growth_second_images_t *images = growth->second_images;
                if (!images) return VFS_ERR_BAD_FORMAT;
                images->sector[step - 2U][i] = sector;
                memcpy(images->before[step - 2U][i], tx->metadata_before, 512U);
                memcpy(images->proposed[step - 2U][i], sectors[i], 512U);
            }
        }
        tx->metadata_write_pending = true;
        if (tx->ordinary) {
            fat32_ordinary_t *ordinary = tx->ordinary;
            ordinary->mutation_attempted = ordinary->fat_attempted = true;
            ordinary->allocations_changed = true;
            if (ordinary->fat_role == 1U) ordinary->candidate_link_attempted = true;
            if (ordinary->fat_role == 2U) ordinary->old_tail_link_attempted = true;
        }
        if (tx->growth && (tx->growth->fat_step == 1U ||
                           tx->growth->fat_step == 3U))
            tx->growth->link_attempted = true;
        if (!fat32_mounted_write(sb, sector, 1U, sectors[i]))
            return VFS_ERR_UNCERTAIN;
        tx->metadata_write_pending = false;
        tx->metadata_copies_completed = i + 1U;
        if (tx->growth && tx->growth->fat_step < 4U)
            tx->growth->fat_copies_completed[tx->growth->fat_step] = i + 1U;
    }
    return VFS_OK;
}

static int fat32_replace_fat_update(vfs_super_t *sb, fat32_fs_t *fs,
    fat32_replace_transaction_t *tx, u32 cluster, u32 expected, u32 value)
{
    return fat32_replace_fat_update_pair(sb, fs, tx, cluster, expected, value, 0U);
}

static int fat32_replace_old_inventory(vfs_node_t *node,
                                        fat32_replace_transaction_t *tx)
{
    fat32_read_view_t view = {0};
    fat32_fs_t *fs = (fat32_fs_t *)node->super->private_data;
    fat32_node_state_t *state = fat32_state(node);
    u32 limit = FAT32_REPLACE_MAX_BYTES / fs->cluster_size_bytes;
    u32 current = state->first_cluster;
    u32 count = 0;

    if (!current) return node->size ? VFS_ERR_BAD_FORMAT : VFS_OK;
    if (!limit) return VFS_ERR_NO_SPACE;
    /* Count the entire allocation, including clusters beyond logical EOF. */
    for (;;) {
        u32 next;
        if (count == limit) return VFS_ERR_NO_SPACE;
        int result = fat32_get_next_checked_view(node->super, fs, current, &next, &view);
        if (result != VFS_OK) return result;
        count++;
        if (next == FAT32_EOC_MIN) break;
        if (next < 2U || next >= fs->total_clusters) return VFS_ERR_BAD_FORMAT;
        current = next;
    }
    if ((u64)count * fs->cluster_size_bytes < node->size)
        return VFS_ERR_BAD_FORMAT;
    tx->old_clusters = (u32 *)kmalloc((usize)count * sizeof(u32));
    if (!tx->old_clusters) return VFS_ERR_NO_MEM;
    tx->old_count = count;
    current = state->first_cluster;
    for (u32 i = 0; i < count; i++) {
        u32 next;
        int result = fat32_get_next_checked_view(node->super, fs, current, &next, &view);
        if (result != VFS_OK) return result;
        tx->old_clusters[i] = current;
        if ((i + 1U == count && next != FAT32_EOC_MIN) ||
            (i + 1U < count &&
             (next < 2U || next >= fs->total_clusters)))
            return VFS_ERR_BAD_FORMAT;
        if (i + 1U < count) current = next;
    }
    return VFS_OK;
}

/* Called after complete namespace ownership validation, under the same mount
 * operation lock. Count the entire allocation, including storage past EOF,
 * without imposing a content-size limit or allocating a cluster inventory. */
static int fat32_retirement_chain_prepare(vfs_super_t *sb, fat32_fs_t *fs,
    u32 first, u32 size, fat32_replace_transaction_t *tx)
{
    fat32_read_view_t view = {0};
    u32 current = first, count = 0U;
    tx->reclaim_streaming = true;
    tx->reclaim_cluster = first;
    if (!first) return size ? VFS_ERR_BAD_FORMAT : VFS_OK;
    for (;;) {
        u32 next;
        if (count >= fs->total_clusters - 2U) return VFS_ERR_BAD_FORMAT;
        int result = fat32_get_next_checked_view(sb, fs, current, &next, &view);
        if (result != VFS_OK) return result;
        count++;
        if (next == FAT32_EOC_MIN) break;
        if (next < 2U || next >= fs->total_clusters) return VFS_ERR_BAD_FORMAT;
        current = next;
    }
    if ((u64)count * fs->cluster_size_bytes < size) return VFS_ERR_BAD_FORMAT;
    tx->old_count = count;
    return VFS_OK;
}

/* Directory chains are bounded by the namespace walk, independently of the
 * 2 MiB file-content limit. Inventory the complete chain before deletion. */
static int fat32_directory_old_inventory(vfs_super_t *sb, fat32_fs_t *fs,
    u32 first, fat32_replace_transaction_t *tx)
{
    fat32_read_view_t view = {0};
    u32 current = first, count = 0U;
    if (first < 2U || first >= fs->total_clusters) return VFS_ERR_BAD_FORMAT;
    for (;;) {
        u32 next;
        if (count == FAT32_WALK_MAX_DIR_CLUSTERS)
            return VFS_ERR_BUFFER_TOO_SMALL;
        int result = fat32_get_next_checked_view(sb, fs, current, &next, &view);
        if (result != VFS_OK) return result;
        count++;
        if (next == FAT32_EOC_MIN) break;
        if (next < 2U || next >= fs->total_clusters)
            return VFS_ERR_BAD_FORMAT;
        current = next;
    }
    tx->old_clusters = (u32 *)kmalloc((usize)count * sizeof(u32));
    if (!tx->old_clusters) return VFS_ERR_NO_MEM;
    tx->old_count = count;
    current = first;
    view.valid = 0U;
    for (u32 i = 0U; i < count; i++) {
        u32 next;
        int result = fat32_get_next_checked_view(sb, fs, current, &next, &view);
        if (result != VFS_OK) return result;
        tx->old_clusters[i] = current;
        if (i + 1U == count) {
            if (next != FAT32_EOC_MIN) return VFS_ERR_BAD_FORMAT;
        } else {
            if (next < 2U || next >= fs->total_clusters)
                return VFS_ERR_BAD_FORMAT;
            current = next;
        }
    }
    return VFS_OK;
}

static int fat32_replace_next_free(vfs_super_t *sb, fat32_fs_t *fs,
                                    u32 *out)
{
    fat32_read_view_t view;
    view.valid = 0;
    u32 start = fs->next_free_cluster;
    if (start < 2U || start >= fs->total_clusters) start = 2U;
    u32 cluster = start;
    do {
        u32 value;
        int result = fat32_get_next_checked_view(sb, fs, cluster, &value, &view);
        if (result != VFS_OK) return result;
        if (value == 0U) { *out = cluster; return VFS_OK; }
        cluster++;
        if (cluster >= fs->total_clusters) cluster = 2U;
    } while (cluster != start);
    return VFS_ERR_NO_SPACE;
}

/* Shared fresh-chain staging for replacement and exclusive creation. The
 * candidate enters the ledger before any FAT copy can be written. */
static int fat32_stage_fresh(vfs_super_t *sb, fat32_fs_t *fs,
                              fat32_replace_transaction_t *tx, u64 length,
                              vfs_replace_source_t source, void *context)
{
    for (u32 i = 0; i < tx->fresh_capacity; i++) {
        u32 candidate;
        int result = fat32_replace_next_free(sb, fs, &candidate);
        if (result != VFS_OK) return result;
        tx->fresh_clusters[tx->fresh_count++] = candidate;
        u32 partner = i && tx->fresh_clusters[i - 1U] / 128U == candidate / 128U ?
                      tx->fresh_clusters[i - 1U] : 0U;
        result = fat32_replace_fat_update_pair(
            sb, fs, tx, candidate, 0U, FAT32_EOC_MIN, partner);
        if (result == VFS_ERR_IO) {
            tx->fresh_count--;
            return result;
        }
        if (result != VFS_OK) return result;
        fs->next_free_cluster = candidate + 1U;
        if (fs->next_free_cluster >= fs->total_clusters)
            fs->next_free_cluster = 2U;
        if (i && !partner) {
            result = fat32_replace_fat_update(
                sb, fs, tx, tx->fresh_clusters[i - 1U], FAT32_EOC_MIN,
                candidate);
            if (result != VFS_OK) return result;
        } else if (!i) tx->new_first = candidate;
        memset(tx->data_buffer, 0, fs->cluster_size_bytes);
        u64 offset = (u64)i * fs->cluster_size_bytes;
        u32 remaining = (u32)(length - offset);
        u32 used = remaining > fs->cluster_size_bytes ?
                   fs->cluster_size_bytes : remaining;
        for (u32 copied = 0; copied < used;) {
            u32 chunk = used - copied > 4096U ? 4096U : used - copied;
            if (source(context, offset + copied,
                       tx->data_buffer + copied, chunk) != chunk)
                return VFS_ERR_IO;
            copied += chunk;
        }
        if (!fat32_mounted_write(sb, fat32_cluster_to_lba(fs, candidate),
                                 fs->sectors_per_cluster, tx->data_buffer))
            return VFS_ERR_UNCERTAIN;
    }
    return VFS_OK;
}

static int fat32_create_stage_directory(vfs_super_t *sb, fat32_fs_t *fs,
                                         fat32_replace_transaction_t *tx,
                                         u32 *out_cluster)
{
    fat32_directory_growth_t *growth = tx->growth;
    int result;
    if (!growth || !growth->append) return VFS_OK;
    u32 count = growth->append_count ? growth->append_count : 1U;
    if (count > 2U) return VFS_ERR_BAD_FORMAT;
    for (u32 index = 0U; index < count; index++) {
        u32 *candidate = index ? &growth->second_candidate : &growth->candidate;
        bool *recorded = index ? &growth->second_candidate_recorded :
                                 &growth->candidate_recorded;
        bool *allocated = index ? &growth->second_candidate_allocated :
                                  &growth->candidate_allocated;
        result = fat32_replace_next_free(sb, fs, candidate);
        if (result != VFS_OK) return result;
        *recorded = true;
        if (tx->lfn_creation) {
            for (u32 image = 0U; image < tx->lfn_creation->count; image++) {
                if (tx->lfn_creation->fresh_index[image] == index + 1U)
                    tx->lfn_creation->sector[image] =
                        fat32_cluster_to_lba(fs, *candidate) +
                        tx->lfn_creation->fresh_part[image];
            }
        }
        growth->fat_step = (u8)(index * 2U);
        tx->phase = FAT32_REPLACE_DIR_ALLOCATING;
        result = fat32_replace_fat_update(sb, fs, tx, *candidate,
                                          0U, FAT32_EOC_MIN);
        growth->fat_step = 4U;
        if (result == VFS_ERR_IO) {
            *recorded = false; /* No FAT write was attempted. */
            return result;
        }
        if (result != VFS_OK) return result;
        *allocated = true;
        tx->phase = FAT32_REPLACE_DIR_INITIALIZING;
        memset(tx->data_buffer, 0, fs->cluster_size_bytes);
        growth->zero_attempted = true;
        if (!fat32_mounted_write(sb, fat32_cluster_to_lba(fs, *candidate),
                                 fs->sectors_per_cluster, tx->data_buffer))
            return VFS_ERR_UNCERTAIN;
        growth->zero_sectors_completed += fs->sectors_per_cluster;
        if (!block_flush_for_id(sb->dev, fs->device_id)) return VFS_ERR_UNCERTAIN;
        if (!index) {
            tx->directory_sector = fat32_cluster_to_lba(fs, *candidate);
            memset(tx->old_sector, 0, sizeof(tx->old_sector));
            growth->target_cluster = *candidate;
            *out_cluster = *candidate;
        }
        growth->fat_step = (u8)(index * 2U + 1U);
        tx->phase = FAT32_REPLACE_DIR_LINKING;
        result = fat32_replace_fat_update(sb, fs, tx, growth->tail_cluster,
                                          FAT32_EOC_MIN, *candidate);
        growth->fat_step = 4U;
        if (result != VFS_OK) return result;
        growth->link_confirmed = true;
        growth->links_confirmed++;
        growth->tail_cluster = *candidate;
        tx->phase = FAT32_REPLACE_DIR_FLUSHING;
        if (!block_flush_for_id(sb->dev, fs->device_id))
            return VFS_ERR_UNCERTAIN;
    }
    return VFS_OK;
}

static int fat32_create_cancel_directory(vfs_super_t *sb, fat32_fs_t *fs,
                                          fat32_replace_transaction_t *tx)
{
    fat32_directory_growth_t *growth = tx->growth;
    if (!growth || !growth->candidate_allocated) return VFS_OK;
    if (growth->link_attempted || growth->link_confirmed)
        return VFS_ERR_UNCERTAIN;
    growth->fat_step = 4U;
    int result = fat32_replace_fat_update(sb, fs, tx, growth->candidate,
                                           FAT32_EOC_MIN, 0U);
    if (result == VFS_OK) growth->candidate_allocated = false;
    return result;
}

static int fat32_replace_cancel(vfs_super_t *sb, fat32_fs_t *fs,
                                 fat32_replace_transaction_t *tx)
{
    tx->phase = FAT32_REPLACE_CANCELLING;
    while (tx->cancellation_cursor < tx->fresh_count) {
        u32 i = tx->cancellation_cursor;
        u32 next = i + 1U < tx->fresh_count ?
                   tx->fresh_clusters[i + 1U] : FAT32_EOC_MIN;
        int result = fat32_replace_fat_update(
            sb, fs, tx, tx->fresh_clusters[i], next, 0U);
        if (result != VFS_OK) return result;
        tx->cancellation_cursor++;
    }
    return VFS_OK;
}

static int fat32_replace_reclaim(vfs_super_t *sb, fat32_fs_t *fs,
                                  fat32_replace_transaction_t *tx)
{
    tx->phase = FAT32_REPLACE_RECLAIMING;
    while (tx->reclamation_cursor < tx->old_count) {
        u32 i = tx->reclamation_cursor;
        if (tx->reclaim_streaming) {
            /* Never reconstruct the successor from a FAT entry after it has
             * been freed, or advance across an ambiguous mirror update. */
            if (!tx->reclaim_successor_valid) {
                u32 next;
                int result = fat32_get_next_checked(sb, fs,
                    tx->reclaim_cluster, &next);
                if (result != VFS_OK) return result;
                if ((i + 1U == tx->old_count && next != FAT32_EOC_MIN) ||
                    (i + 1U < tx->old_count &&
                     (next < 2U || next >= fs->total_clusters)))
                    return VFS_ERR_BAD_FORMAT;
                tx->reclaim_successor = next;
                tx->reclaim_successor_valid = true;
            }
            int result = fat32_replace_fat_update(sb, fs, tx,
                tx->reclaim_cluster, tx->reclaim_successor, 0U);
            if (result != VFS_OK) return result;
            tx->reclamation_cursor++;
            tx->reclaim_cluster = tx->reclaim_successor;
            tx->reclaim_successor_valid = false;
            continue;
        }
        u32 next = i + 1U < tx->old_count ?
                   tx->old_clusters[i + 1U] : FAT32_EOC_MIN;
        int result = fat32_replace_fat_update(
            sb, fs, tx, tx->old_clusters[i], next, 0U);
        if (result != VFS_OK) return result;
        tx->reclamation_cursor++;
    }
    return VFS_OK;
}

/* Validate optional FSInfo locations before any persistent transaction write.
 * An invalid pointer or signature cannot be treated as an advisory hint. */
static int fat32_replace_prepare_fsinfo(vfs_super_t *sb, fat32_fs_t *fs,
                                         fat32_replace_transaction_t *tx)
{
    u8 sector[512];
    if (!fs->fs_info_sector) return VFS_OK;
    if (fs->fs_info_sector >= fs->reserved_sector_count)
        return VFS_ERR_BAD_FORMAT;
    tx->fsinfo_sectors[tx->fsinfo_count++] = fs->fs_info_sector;
    if (fs->backup_boot_sector &&
        fs->backup_boot_sector <= 0xffffffffU - fs->fs_info_sector &&
        fs->backup_boot_sector + fs->fs_info_sector <
            fs->reserved_sector_count)
        tx->fsinfo_sectors[tx->fsinfo_count++] =
            fs->backup_boot_sector + fs->fs_info_sector;
    for (u32 i = 0; i < tx->fsinfo_count; i++) {
        if (!fat32_mounted_read(sb, tx->fsinfo_sectors[i], 1U, sector))
            return VFS_ERR_IO;
        if (fat32_load_le32(sector) != 0x41615252U ||
            fat32_load_le32(sector + 484U) != 0x61417272U ||
            fat32_load_le32(sector + 508U) != 0xaa550000U)
            return VFS_ERR_BAD_FORMAT;
    }
    return VFS_OK;
}

static int fat32_replace_fsinfo(vfs_super_t *sb,
                                 fat32_replace_transaction_t *tx)
{
    u8 buffer[512];
    tx->phase = FAT32_REPLACE_FSINFO;
    while (tx->fsinfo_cursor < tx->fsinfo_count) {
        u64 sector = tx->fsinfo_sectors[tx->fsinfo_cursor];
        if (!fat32_mounted_read(sb, sector, 1U, buffer)) return VFS_ERR_IO;
        if (fat32_load_le32(buffer) != 0x41615252U ||
            fat32_load_le32(buffer + 484U) != 0x61417272U ||
            fat32_load_le32(buffer + 508U) != 0xaa550000U)
            return VFS_ERR_BAD_FORMAT;
        tx->metadata_sector = sector;
        tx->metadata_cluster = 0U;
        tx->metadata_copy = tx->fsinfo_cursor;
        tx->metadata_copies_completed = tx->fsinfo_cursor;
        memcpy(tx->metadata_before, buffer, 512U);
        fat32_store_le32(buffer + 488U, 0xffffffffU);
        fat32_store_le32(buffer + 492U, 0xffffffffU);
        memcpy(tx->metadata_proposed, buffer, 512U);
        tx->metadata_write_pending = true;
        if (tx->ordinary) tx->ordinary->mutation_attempted = true;
        if (!fat32_mounted_write(sb, sector, 1U, buffer))
            return VFS_ERR_UNCERTAIN;
        tx->metadata_write_pending = false;
        tx->fsinfo_cursor++;
    }
    return VFS_OK;
}

static bool fat32_replace_finish_handoff(vfs_super_t *sb, void *context)
{
    fat32_fs_t *fs = sb ? (fat32_fs_t *)sb->private_data : NULL;
    fat32_replace_transaction_t *tx =
        (fat32_replace_transaction_t *)context;
    if (!fs || fs->replace_transaction != tx || !tx->handoff_pending ||
        (tx->phase != FAT32_REPLACE_HANDOFF_PUBLISHED &&
         tx->phase != FAT32_REPLACE_HANDOFF_CANCELLED))
        return false;
    if (tx->conditional_replacement) {
        if (tx->metadata_write_pending) return false;
        if (tx->phase == FAT32_REPLACE_HANDOFF_PUBLISHED) {
            u8 required = FAT32_REPLACE_FLUSH_STAGED |
                          FAT32_REPLACE_FLUSH_PUBLISHED |
                          FAT32_REPLACE_FLUSH_COMPLETED;
            if (!tx->publication_attempted || !tx->publication_confirmed ||
                (tx->replacement_flush_confirmed & required) != required ||
                tx->reclamation_cursor != tx->old_count ||
                tx->fsinfo_cursor != tx->fsinfo_count) return false;
        } else if (tx->publication_attempted ||
                   !(tx->replacement_flush_confirmed & FAT32_REPLACE_FLUSH_CANCELLED) ||
                   tx->cancellation_cursor != tx->fresh_count) return false;
    }
    fs->replace_transaction = NULL;
    fs->version_confident = true;
    fs->namespace_hidden = false;
    if (tx->ordinary) fs->ownership_proven = true;
    fat32_replace_free(tx);
    return true;
}

/* All calls are under operation_lock. Ownership certificates are mount-local:
 * every mounted write invalidates them, and only a confirmed ordinary
 * transaction, whose allocation transitions preserve that proof, restores it
 * during resolved pin handoff. They do not detect out-of-band media changes. */
static int fat32_ordinary_entry(vfs_node_t *node, u8 sector[512], u64 *lba)
{
    fat32_node_state_t *state = fat32_state(node);
    fat32_fs_t *fs = node->super->private_data;
    if (!state || state->node != node || !state->alive)
        return VFS_ERR_NOT_FOUND;
    if (!state->version_valid || !fs->version_confident || fs->replace_transaction)
        return VFS_ERR_UNCERTAIN;
    if (!node->super->dev || node->super->dev->id != fs->device_id ||
        !block_device_id_is_live(fs->device_id)) return VFS_ERR_DEVICE_GONE;
    if (state->parent_cluster < 2U || state->parent_cluster >= fs->total_clusters ||
        state->entry_index >= fs->cluster_size_bytes / 32U ||
        fat32_find_state(fs, state->parent_cluster, state->entry_index) != state)
        return VFS_ERR_BAD_FORMAT;
    *lba = fat32_cluster_to_lba(fs, state->parent_cluster) + state->entry_index / 16U;
    if (!fat32_mounted_read(node->super, *lba, 1U, sector)) return VFS_ERR_IO;
    const fat32_on_disk_entry_t *entry = (const void *)(sector +
        (state->entry_index % 16U) * 32U);
    if ((u8)entry->name[0] == 0U || (u8)entry->name[0] == 0xe5U ||
        (entry->attr & 0x18U) || fat32_entry_first(entry) != state->first_cluster ||
        entry->file_size != node->size) {
        state->version_valid = fs->version_confident = false;
        return VFS_ERR_UNCERTAIN;
    }
    return entry->attr & 1U ? VFS_ERR_ACCESS_DENIED : VFS_OK;
}

static int fat32_ordinary_chain(vfs_node_t *node, bool certified,
                                u32 *count, u32 *tail)
{
    fat32_fs_t *fs = node->super->private_data;
    fat32_node_state_t *state = fat32_state(node);
    *count = *tail = 0U;
    if (certified && state->allocation_generation == state->generation) {
        *count = state->allocation_count; *tail = state->allocation_tail;
        return VFS_OK;
    }
    u32 current = state->first_cluster;
    if (!current) return node->size ? VFS_ERR_BAD_FORMAT : VFS_OK;
    fat32_read_view_t view = {0};
    for (;;) {
        u32 next;
        if (*count >= fs->total_clusters - 2U) return VFS_ERR_BAD_FORMAT;
        int result = fat32_get_next_checked_view(node->super, fs, current, &next, &view);
        if (result != VFS_OK) return result;
        (*count)++; *tail = current;
        if (next == FAT32_EOC_MIN) break;
        if (next < 2U || next >= fs->total_clusters) return VFS_ERR_BAD_FORMAT;
        current = next;
    }
    return (u64)*count * fs->cluster_size_bytes < node->size
        ? VFS_ERR_BAD_FORMAT : VFS_OK;
}

/* A read failure before a FAT write attempt is definite. A failed callback
 * after any copy has been attempted cannot be converted to a partial write. */
static int fat32_ordinary_fat(vfs_super_t *sb, fat32_fs_t *fs,
    fat32_replace_transaction_t *tx, u32 cluster, u32 expected, u32 value, u8 role)
{
    tx->ordinary->fat_role = role;
    tx->ordinary->fat_attempted = false;
    int result = fat32_replace_fat_update(sb, fs, tx, cluster, expected, value);
    if (result != VFS_OK && tx->ordinary->fat_attempted) return VFS_ERR_UNCERTAIN;
    return result;
}

static int fat32_ordinary_allocate(vfs_node_t *node, fat32_replace_transaction_t *tx)
{
    fat32_fs_t *fs = node->super->private_data;
    fat32_ordinary_t *o = tx->ordinary;
    int result;
    tx->phase = FAT32_REPLACE_STAGING;
    result = fat32_replace_next_free(node->super, fs, &o->candidate);
    if (result != VFS_OK) return result;
    o->candidate_allocated = o->candidate_link_attempted = false;
    o->zero_progress = 0U;
    result = fat32_ordinary_fat(node->super, fs, tx, o->candidate, 0U, FAT32_EOC_MIN, 0U);
    if (result != VFS_OK) return result;
    o->candidate_allocated = true;
    memset(o->data_proposed, 0, 512U);
    o->data_before_valid = false;
    for (u32 i = 0U; i < fs->sectors_per_cluster; i++) {
        o->data_sector = fat32_cluster_to_lba(fs, o->candidate) + i;
        o->data_pending = o->mutation_attempted = true;
        if (!fat32_mounted_write(node->super, o->data_sector, 1U, o->data_proposed))
            return VFS_ERR_UNCERTAIN;
        o->data_pending = false; o->zero_progress++; o->data_sectors_confirmed++;
    }
    if (o->fresh_count) {
        result = fat32_ordinary_fat(node->super, fs, tx, o->fresh_tail,
                                      FAT32_EOC_MIN, o->candidate, 1U);
        if (result != VFS_OK) return result;
    } else o->fresh_first = o->candidate;
    o->fresh_tail = o->candidate; o->fresh_count++;
    o->candidate = 0U; o->candidate_allocated = false;
    o->candidate_link_attempted = false;
    fs->next_free_cluster = o->fresh_tail + 1U;
    if (fs->next_free_cluster >= fs->total_clusters) fs->next_free_cluster = 2U;
    return VFS_OK;
}

static int fat32_ordinary_cluster(vfs_node_t *node,
    fat32_replace_transaction_t *tx, u32 index, u32 *out)
{
    fat32_fs_t *fs = node->super->private_data;
    fat32_ordinary_t *o = tx->ordinary;
    u32 current, at;
    int result;
    if (o->io_valid && index == o->io_index) { *out = o->io_cluster; return VFS_OK; }
    if (index >= o->old_count) {
        u32 fresh_index = index - o->old_count;
        while (o->fresh_count <= fresh_index) {
            result = fat32_ordinary_allocate(node, tx);
            if (result != VFS_OK) return result;
        }
        if (fresh_index + 1U == o->fresh_count) current = o->fresh_tail;
        else {
            current = o->fresh_first;
            for (at = 0U; at < fresh_index; at++) {
                result = fat32_get_next_checked(node->super, fs, current, &current);
                if (result != VFS_OK) return result;
            }
        }
    } else if (index + 1U == o->old_count) current = o->old_tail;
    else {
        current = o->old_first; at = 0U;
        if (o->io_valid && o->io_index < index) {
            current = o->io_cluster; at = o->io_index;
        }
        fat32_read_view_t view = {0};
        while (at++ < index) {
            result = fat32_get_next_checked_view(node->super, fs, current, &current, &view);
            if (result != VFS_OK) return result;
        }
    }
    if (current < 2U || current >= fs->total_clusters) return VFS_ERR_BAD_FORMAT;
    o->io_cluster = *out = current; o->io_index = index; o->io_valid = true;
    return VFS_OK;
}

static int fat32_ordinary_bytes(vfs_node_t *node, fat32_replace_transaction_t *tx,
    u64 offset, u64 length, const u8 *input)
{
    fat32_fs_t *fs = node->super->private_data;
    fat32_ordinary_t *o = tx->ordinary;
    while (length) {
        u32 cluster;
        int result = fat32_ordinary_cluster(node, tx,
            (u32)(offset / fs->cluster_size_bytes), &cluster);
        if (result != VFS_OK) return result;
        u32 at = (u32)(offset % 512U), amount = 512U - at;
        if (amount > length) amount = (u32)length;
        o->data_sector = fat32_cluster_to_lba(fs, cluster) +
            (offset % fs->cluster_size_bytes) / 512U;
        if (!fat32_mounted_read(node->super, o->data_sector, 1U, o->data_before))
            return VFS_ERR_IO;
        o->data_before_valid = true;
        memcpy(o->data_proposed, o->data_before, 512U);
        if (input) memcpy(o->data_proposed + at, input, amount);
        else memset(o->data_proposed + at, 0, amount);
        o->data_pending = o->mutation_attempted = true;
        if (!fat32_mounted_write(node->super, o->data_sector, 1U, o->data_proposed))
            return VFS_ERR_UNCERTAIN;
        o->data_pending = false; o->data_sectors_confirmed++;
        if (input) { o->confirmed_user_bytes += amount; input += amount; }
        else o->zero_bytes_confirmed += amount;
        offset += amount; length -= amount;
    }
    return VFS_OK;
}

static int fat32_ordinary_trim_plan(vfs_node_t *node,
    fat32_replace_transaction_t *tx, u32 size)
{
    fat32_fs_t *fs = node->super->private_data;
    fat32_ordinary_t *o = tx->ordinary;
    u32 kept = size / fs->cluster_size_bytes + (size % fs->cluster_size_bytes != 0U);
    if (kept >= o->old_count) return VFS_OK;
    o->removed_count = o->old_count - kept;
    if (!kept) { o->removed_first = o->old_first; return VFS_OK; }
    u32 current = o->old_first;
    fat32_read_view_t view = {0};
    for (u32 at = 0U; at < kept; at++) {
        u32 next;
        int result = fat32_get_next_checked_view(node->super, fs, current, &next, &view);
        if (result != VFS_OK) return result;
        if (next < 2U || next >= fs->total_clusters) return VFS_ERR_BAD_FORMAT;
        if (at + 1U == kept) { o->kept_tail = current; o->removed_first = next; }
        else current = next;
    }
    return VFS_OK;
}

static int fat32_ordinary_publish(vfs_node_t *node, fat32_replace_transaction_t *tx,
                                   u32 first, u32 size)
{
    fat32_ordinary_t *o = tx->ordinary;
    fat32_node_state_t *state = fat32_state(node);
    if (first == o->old_first && size == o->old_size) return VFS_OK;
    if (!fat32_mounted_read(node->super, tx->directory_sector, 1U, tx->proposed_sector))
        return VFS_ERR_IO;
    if (memcmp(tx->proposed_sector, tx->old_sector, 512U)) return VFS_ERR_BAD_FORMAT;
    fat32_on_disk_entry_t *entry = (void *)(tx->proposed_sector +
        (state->entry_index % 16U) * 32U);
    entry->first_cluster_high = (u16)(first >> 16U);
    entry->first_cluster_low = (u16)first; entry->file_size = size;
    tx->phase = FAT32_REPLACE_PUBLISHING;
    o->directory_attempted = o->mutation_attempted = true;
    if (!fat32_mounted_write(node->super, tx->directory_sector, 1U, tx->proposed_sector))
        return VFS_ERR_UNCERTAIN;
    o->directory_confirmed = true;
    state->first_cluster = first;
    vfs_nodes_update_size(node->super, node->inode, size);
    fat32_version_advance(state); o->generation_advanced = true;
    return VFS_OK;
}

static int fat32_ordinary_run(vfs_node_t *node, u64 offset, u64 size,
    const void *buffer, bool resize, u64 *out_bytes, u64 *out_pin)
{
    fat32_fs_t *fs;
    fat32_node_state_t *state;
    fat32_replace_transaction_t *tx;
    fat32_ordinary_t *o;
    u8 directory[512];
    u64 lba, final_size;
    u32 count, tail;
    int result, completion = VFS_OK;
    bool certified;
    if (!out_bytes || !out_pin) return VFS_ERR_INVALID_PARAM;
    *out_bytes = *out_pin = 0U;
    if (!node || !node->super || !node->super->private_data ||
        node->type != VFS_TYPE_FILE || (!resize && size && !buffer) ||
        offset > 0xffffffffULL || size > 0xffffffffULL - offset)
        return VFS_ERR_INVALID_PARAM;
    if (node->super->read_only) return VFS_ERR_ACCESS_DENIED;
    if (!resize && !size) return VFS_OK;
    fs = node->super->private_data; state = fat32_state(node);
    result = fat32_ordinary_entry(node, directory, &lba);
    if (result != VFS_OK) return result;
    certified = fs->ownership_proven;
    if (!certified) {
        result = fat32_ownership_preflight_locked(node);
        if (result != VFS_OK) return result;
        /* A fresh proof cannot revive mapping summaries from the previous
         * certificate, including summaries of other live files. */
        for (fat32_node_state_t *live = fs->nodes; live; live = live->next)
            live->allocation_generation = 0U;
        fs->ownership_proven = true;
    }
    result = fat32_ordinary_chain(node, certified, &count, &tail);
    if (result != VFS_OK) {
        if (result == VFS_ERR_BAD_FORMAT) fs->version_confident = false;
        return result;
    }
    state->allocation_count = count;
    state->allocation_tail = tail;
    state->allocation_generation = state->generation;
    final_size = resize ? size : (offset + size > node->size ? offset + size : node->size);
    u64 capacity = (u64)count * fs->cluster_size_bytes;
    u64 possible = (u64)(fs->total_clusters - 2U) * fs->cluster_size_bytes;
    if ((resize && final_size > possible) || (!resize && offset >= possible))
        return VFS_ERR_NO_SPACE;
    if (resize && final_size == node->size &&
        count == size / fs->cluster_size_bytes + (size % fs->cluster_size_bytes != 0U))
        return VFS_OK;
    if (state->generation == ~(u64)0) return VFS_ERR_NO_SPACE;
    tx = kmalloc(sizeof(*tx));
    if (!tx) return VFS_ERR_NO_MEM;
    memset(tx, 0, sizeof(*tx));
    tx->ordinary = kmalloc(sizeof(*o));
    if (!tx->ordinary) { fat32_replace_free(tx); return VFS_ERR_NO_MEM; }
    o = tx->ordinary; memset(o, 0, sizeof(*o));
    o->old_first = state->first_cluster; o->old_size = (u32)node->size;
    o->old_count = count; o->old_tail = tail;
    o->write_offset = offset; o->requested_length = size;
    tx->directory_sector = lba; tx->record_id = node->inode;
    tx->expected_generation = state->generation; tx->phase = FAT32_REPLACE_STAGING;
    memcpy(tx->old_sector, directory, 512U);
    if (resize && final_size < capacity) {
        result = fat32_ordinary_trim_plan(node, tx, (u32)final_size);
        if (result != VFS_OK) goto before_pin;
    }
    if (final_size > capacity || o->removed_count) {
        result = fat32_replace_prepare_fsinfo(node->super, fs, tx);
        if (result != VFS_OK) goto before_pin;
    }
    fs->replace_transaction = tx;
    result = vfs_recovery_pin_begin_handoff(node->super,
        fat32_replace_finish_handoff, tx, &tx->pin_id);
    if (result != VFS_OK) { fs->replace_transaction = NULL; goto before_pin; }
    fs->version_confident = false;
    if (resize) {
        if (final_size > o->old_size)
            result = fat32_ordinary_bytes(node, tx, o->old_size, final_size - o->old_size, NULL);
        else result = VFS_OK;
    } else {
        if (offset > o->old_size)
            result = fat32_ordinary_bytes(node, tx, o->old_size, offset - o->old_size, NULL);
        if (result == VFS_OK)
            result = fat32_ordinary_bytes(node, tx, offset, size, buffer);
    }
    if (result == VFS_ERR_UNCERTAIN || result == VFS_ERR_BAD_FORMAT) goto uncertain;
    /* An unlinked, confirmed candidate is safely cancellable on a read failure.
     * A candidate whose link was attempted remains accounted for by quarantine. */
    if (o->candidate_allocated) {
        if (o->candidate_link_attempted) goto uncertain;
        int clean = fat32_ordinary_fat(node->super, fs, tx, o->candidate,
                                         FAT32_EOC_MIN, 0U, 3U);
        if (clean != VFS_OK) goto uncertain;
        o->candidate_allocated = false; o->candidate = 0U;
    }
    if (result != VFS_OK && (resize || !o->confirmed_user_bytes)) {
        completion = result;
        tx->phase = FAT32_REPLACE_CANCELLING;
        tx->reclaim_streaming = true; tx->reclaim_cluster = o->fresh_first;
        tx->old_count = o->fresh_count; o->fat_role = 3U;
        if (fat32_replace_reclaim(node->super, fs, tx) != VFS_OK) goto uncertain;
        if (o->allocations_changed && fat32_replace_fsinfo(node->super, tx) != VFS_OK)
            goto uncertain;
        if (o->mutation_attempted && !block_flush_for_id(node->super->dev, fs->device_id))
            goto uncertain;
        tx->phase = FAT32_REPLACE_HANDOFF_CANCELLED;
        goto handoff;
    }
    completion = result;
    if (!resize) final_size = offset + o->confirmed_user_bytes > o->old_size
        ? offset + o->confirmed_user_bytes : o->old_size;
    if (o->mutation_attempted && !block_flush_for_id(node->super->dev, fs->device_id))
        goto uncertain;
    o->staging_flushed = true;
    if (o->fresh_count && o->old_count) {
        result = fat32_ordinary_fat(node->super, fs, tx, o->old_tail,
                                      FAT32_EOC_MIN, o->fresh_first, 2U);
        if (result != VFS_OK) goto uncertain;
        o->old_tail_linked = true;
        if (!block_flush_for_id(node->super->dev, fs->device_id)) goto uncertain;
    }
    u32 first = resize && !final_size ? 0U :
        (o->old_first ? o->old_first : o->fresh_first);
    result = fat32_ordinary_publish(node, tx, first, (u32)final_size);
    if (result != VFS_OK) goto uncertain;
    if (o->directory_confirmed &&
        !block_flush_for_id(node->super->dev, fs->device_id)) goto uncertain;
    o->publication_flushed = true;
    if (o->removed_count) {
        if (o->kept_tail) {
            result = fat32_ordinary_fat(node->super, fs, tx, o->kept_tail,
                                          o->removed_first, FAT32_EOC_MIN, 3U);
            if (result != VFS_OK || !block_flush_for_id(node->super->dev, fs->device_id))
                goto uncertain;
        }
        tx->reclaim_streaming = true; tx->reclaim_cluster = o->removed_first;
        tx->old_count = o->removed_count; o->fat_role = 3U;
        result = fat32_replace_reclaim(node->super, fs, tx);
        if (result != VFS_OK) goto uncertain;
    }
    if (o->allocations_changed && fat32_replace_fsinfo(node->super, tx) != VFS_OK)
        goto uncertain;
    if (o->allocations_changed &&
        !block_flush_for_id(node->super->dev, fs->device_id)) goto uncertain;
    o->cleanup_flushed = true;
    if (!o->generation_advanced) fat32_version_advance(state);
    state->allocation_count = o->old_count + o->fresh_count - o->removed_count;
    state->allocation_tail = o->removed_count ? o->kept_tail :
        (o->fresh_count ? o->fresh_tail : o->old_tail);
    state->allocation_generation = state->generation;
    *out_bytes = o->confirmed_user_bytes;
    tx->phase = FAT32_REPLACE_HANDOFF_PUBLISHED;
handoff:
    tx->handoff_pending = true; *out_pin = tx->pin_id;
    return completion;
uncertain:
    fs->ownership_proven = false;
    (void)vfs_recovery_pin_mark_unresolved(tx->pin_id);
    return VFS_ERR_UNCERTAIN;
before_pin:
    if (result == VFS_ERR_BAD_FORMAT) fs->version_confident = false;
    fat32_replace_free(tx);
    return result;
}

static int fat32_write_pinned(vfs_node_t *node, u64 offset, u64 size,
    const void *buffer, u64 *out_bytes, u64 *out_pin)
{
    return fat32_ordinary_run(node, offset, size, buffer, false, out_bytes, out_pin);
}

static int fat32_resize_pinned(vfs_node_t *node, u64 length, u64 *out_pin)
{
    u64 bytes;
    return fat32_ordinary_run(node, 0U, length, NULL, true, &bytes, out_pin);
}

static int fat32_truncate_pinned(vfs_node_t *node, u64 *out_pin)
{
    return fat32_resize_pinned(node, 0U, out_pin);
}

/* One attempt per ordering barrier. A failed backend flush may have completed
 * work; retain progress and quarantine rather than retrying or rolling back. */
static int fat32_replace_flush(vfs_super_t *sb, fat32_fs_t *fs,
    fat32_replace_transaction_t *tx, u8 checkpoint)
{
    if (tx->replacement_flush_attempted & checkpoint) return VFS_ERR_UNCERTAIN;
    tx->replacement_flush_attempted |= checkpoint;
    if (!block_flush_for_id(sb->dev, fs->device_id)) return VFS_ERR_UNCERTAIN;
    tx->replacement_flush_confirmed |= checkpoint;
    return VFS_OK;
}

/* VFS has already retained the opened inode and holds operation_lock. The
 * recovery pin stays discoverable until its callback completes after unlock. */
static int fat32_replace_locked(vfs_super_t *sb, u64 record_id,
                                 u64 expected_generation, u64 length,
                                 vfs_replace_source_t source, void *context,
                                 u64 *out_pin)
{
    fat32_fs_t *fs = sb ? (fat32_fs_t *)sb->private_data : NULL;
    fat32_node_state_t *state = NULL;
    fat32_replace_transaction_t *tx = NULL;
    vfs_file_facts_t facts;
    u32 new_count;
    int result;

    if (!fs || !out_pin || (length && !source)) return VFS_ERR_INVALID_PARAM;
    *out_pin = 0;
    if (sb->read_only) return VFS_ERR_ACCESS_DENIED;
    if (length > FAT32_REPLACE_MAX_BYTES) return VFS_ERR_NO_SPACE;
    if (fs->bytes_per_sector != 512U || !fs->sectors_per_cluster ||
        fs->sectors_per_cluster > 128U ||
        fs->cluster_size_bytes != fs->sectors_per_cluster * 512U ||
        !fs->fat_count || fs->fat_count > 4U)
        return VFS_ERR_BAD_FORMAT;
    if (!fs->version_confident || fs->replace_transaction)
        return VFS_ERR_UNCERTAIN;
    for (state = fs->nodes; state; state = state->next)
        if (state->alive && state->node->inode == record_id) break;
    if (!state || state->node->type != VFS_TYPE_FILE)
        return VFS_ERR_NOT_FOUND;
    result = fat32_file_facts(state->node, &facts);
    if (result != VFS_OK) return result;
    if (facts.record_id != record_id) return VFS_ERR_BAD_FORMAT;
    if (facts.generation != expected_generation)
        return VFS_ERR_STALE_GENERATION;
    if (expected_generation == ~(u64)0) return VFS_ERR_NO_SPACE;
    result = fat32_ownership_preflight_locked(state->node);
    if (result != VFS_OK) return result;
    new_count = (u32)(length / fs->cluster_size_bytes +
                     (length % fs->cluster_size_bytes != 0U));
    tx = (fat32_replace_transaction_t *)kmalloc(sizeof(*tx));
    if (!tx) return VFS_ERR_NO_MEM;
    memset(tx, 0, sizeof(*tx));
    tx->conditional_replacement = true;
    tx->record_id = record_id;
    tx->expected_generation = expected_generation;
    tx->new_size = (u32)length;
    tx->fresh_capacity = new_count;
    tx->phase = FAT32_REPLACE_STAGING;
    result = fat32_replace_old_inventory(state->node, tx);
    if (result != VFS_OK) {
        if (result == VFS_ERR_BAD_FORMAT) fs->version_confident = false;
        goto before_pin;
    }
    if (new_count) {
        tx->fresh_clusters = (u32 *)kmalloc((usize)new_count * sizeof(u32));
        tx->data_buffer = (u8 *)kmalloc(fs->cluster_size_bytes);
        if (!tx->fresh_clusters || !tx->data_buffer) {
            result = VFS_ERR_NO_MEM;
            goto before_pin;
        }
    }
    u32 entries_per_sector = 512U / sizeof(fat32_on_disk_entry_t);
    u32 sector_in_cluster = state->entry_index / entries_per_sector;
    u32 entry_in_sector = state->entry_index % entries_per_sector;
    if (state->parent_cluster < 2U ||
        state->parent_cluster >= fs->total_clusters ||
        sector_in_cluster >= fs->sectors_per_cluster) {
        result = VFS_ERR_BAD_FORMAT;
        goto before_pin;
    }
    tx->directory_sector = fat32_cluster_to_lba(fs, state->parent_cluster) +
                           sector_in_cluster;
    if (!fat32_mounted_read(sb, tx->directory_sector, 1U, tx->old_sector)) {
        result = VFS_ERR_IO;
        goto before_pin;
    }
    fat32_on_disk_entry_t *old_entry =
        &((fat32_on_disk_entry_t *)tx->old_sector)[entry_in_sector];
    if ((u8)old_entry->name[0] == 0U ||
        (u8)old_entry->name[0] == 0xe5U ||
        (old_entry->attr & 0x18U) ||
        fat32_entry_first(old_entry) != state->first_cluster ||
        old_entry->file_size != facts.size) {
        result = VFS_ERR_BAD_FORMAT;
        fs->version_confident = false;
        goto before_pin;
    }
    memcpy(tx->proposed_sector, tx->old_sector, 512U);
    result = fat32_replace_prepare_fsinfo(sb, fs, tx);
    if (result != VFS_OK) {
        if (result == VFS_ERR_BAD_FORMAT) fs->version_confident = false;
        goto before_pin;
    }
    fs->replace_transaction = tx;
    result = vfs_recovery_pin_begin_handoff(
        sb, fat32_replace_finish_handoff, tx, &tx->pin_id);
    if (result != VFS_OK) {
        fs->replace_transaction = NULL;
        goto before_pin;
    }
    fs->version_confident = false;
    result = fat32_stage_fresh(sb, fs, tx, length, source, context);
    if (result == VFS_ERR_BAD_FORMAT || result == VFS_ERR_UNCERTAIN)
        goto uncertain;
    if (result != VFS_OK) goto cancel;
    tx->phase = FAT32_REPLACE_STAGING_FLUSHING;
    if (tx->fresh_count) {
        if (fat32_replace_flush(sb, fs, tx, FAT32_REPLACE_FLUSH_STAGED) != VFS_OK)
            goto uncertain;
    } else tx->replacement_flush_confirmed |= FAT32_REPLACE_FLUSH_STAGED;
    fat32_on_disk_entry_t *new_entry =
        &((fat32_on_disk_entry_t *)tx->proposed_sector)[entry_in_sector];
    new_entry->first_cluster_high = (u16)(tx->new_first >> 16U);
    new_entry->first_cluster_low = (u16)tx->new_first;
    new_entry->file_size = (u32)length;
    tx->phase = FAT32_REPLACE_PUBLISHING;
    tx->publication_attempted = true;
    if (!fat32_mounted_write(sb, tx->directory_sector, 1U,
                             tx->proposed_sector)) {
        result = VFS_ERR_UNCERTAIN;
        goto uncertain;
    }
    tx->publication_confirmed = true;
    state->first_cluster = tx->new_first;
    fat32_version_advance(state);
    vfs_nodes_update_size(sb, record_id, length);
    tx->phase = FAT32_REPLACE_PUBLICATION_FLUSHING;
    if (fat32_replace_flush(sb, fs, tx, FAT32_REPLACE_FLUSH_PUBLISHED) != VFS_OK)
        goto uncertain;
    result = fat32_replace_reclaim(sb, fs, tx);
    if (result != VFS_OK) goto uncertain;
    result = fat32_replace_fsinfo(sb, tx);
    if (result != VFS_OK) goto uncertain;
    tx->phase = FAT32_REPLACE_COMPLETION_FLUSHING;
    if (tx->old_count || tx->fsinfo_count) {
        if (fat32_replace_flush(sb, fs, tx, FAT32_REPLACE_FLUSH_COMPLETED) != VFS_OK)
            goto uncertain;
    } else tx->replacement_flush_confirmed |= FAT32_REPLACE_FLUSH_COMPLETED;
    kfree(tx->data_buffer);
    tx->data_buffer = NULL;
    tx->phase = FAT32_REPLACE_HANDOFF_PUBLISHED;
    tx->handoff_pending = true;
    *out_pin = tx->pin_id;
    return VFS_OK;

cancel:
    if (fat32_replace_cancel(sb, fs, tx) != VFS_OK) goto uncertain;
    tx->phase = FAT32_REPLACE_CANCELLATION_FLUSHING;
    if (tx->fresh_count) {
        if (fat32_replace_flush(sb, fs, tx, FAT32_REPLACE_FLUSH_CANCELLED) != VFS_OK)
            goto uncertain;
    } else tx->replacement_flush_confirmed |= FAT32_REPLACE_FLUSH_CANCELLED;
    kfree(tx->data_buffer);
    tx->data_buffer = NULL;
    tx->phase = FAT32_REPLACE_HANDOFF_CANCELLED;
    tx->handoff_pending = true;
    *out_pin = tx->pin_id;
    return result;
uncertain:
    /* The streamed input scratch is not recovery metadata. */
    kfree(tx->data_buffer);
    tx->data_buffer = NULL;
    (void)vfs_recovery_pin_mark_unresolved(tx->pin_id);
    return VFS_ERR_UNCERTAIN;
before_pin:
    fat32_replace_free(tx);
    return result;
}

static void fat32_create_83_name(const char *name, char *dst_11) {
    memset(dst_11, ' ', 11);

    const char *dot = NULL;
    for (const char *p = name; *p; p++) {
        if (*p == '.') {
            dot = p;
            break;
        }
    }

    u32 name_len = dot ? (u32)(dot - name) : strlen(name);
    if (name_len > 8) name_len = 8;

    for (u32 i = 0; i < name_len; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        dst_11[i] = c;
    }

    if (dot) {
        const char *ext = dot + 1;
        u32 ext_len = strlen(ext);
        if (ext_len > 3) ext_len = 3;

        for (u32 i = 0; i < ext_len; i++) {
            char c = ext[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            dst_11[8 + i] = c;
        }
    }
}

/* The short-name encoder is also used by the existing rename path. */
static bool fat32_make_83_name(const char *name, char *dst_11) {
    usize base_length = 0;
    usize extension_length = 0;
    bool in_extension = false;

    if (!name || !*name || !dst_11) return false;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '.') {
            if (in_extension || base_length == 0U) return false;
            in_extension = true;
            continue;
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
        if (in_extension) {
            if (++extension_length > 3U) return false;
        } else if (++base_length > 8U) {
            return false;
        }
    }
    if (base_length == 0U || (in_extension && extension_length == 0U))
        return false;
    fat32_create_83_name(name, dst_11);
    return true;
}

/* The public name is UTF-8, but VFAT stores 13 UTF-16 units per long entry.
 * Reject spellings that cannot be published unchanged, including FAT's
 * reserved characters and boundary forms whose comparisons collapse. */
static bool fat32_create_lfn_units(const char *name, u16 units[255],
                                   u32 *out_count)
{
    const char *cursor = name;
    u32 count = 0U;
    if (!name || !*name || strlen(name) > 255U || name[0] == ' ') return false;
    while (*cursor) {
        u32 scalar;
        if (!fat32_utf8_next(&cursor, &scalar) || scalar < 0x20U ||
            scalar == 0x7fU || scalar == 0xffffU ||
            scalar == '"' || scalar == '*' || scalar == '/' ||
            scalar == ':' || scalar == '<' || scalar == '>' ||
            scalar == '?' || scalar == '\\' || scalar == '|') return false;
        if (scalar < 0x10000U) {
            if (count == 255U) return false;
            units[count++] = (u16)scalar;
        } else {
            if (count > 253U) return false;
            scalar -= 0x10000U;
            units[count++] = (u16)(0xd800U | (scalar >> 10U));
            units[count++] = (u16)(0xdc00U | (scalar & 0x3ffU));
        }
    }
    if (!count || units[count - 1U] == ' ' || units[count - 1U] == '.')
        return false;
    *out_count = count;
    return true;
}

/* Conventional, deterministic ~n aliases. The complete strict namespace
 * query below tests both existing short aliases and LFNs for each candidate.
 * Exhausting the bounded 99 candidates refuses creation without mutation. */
static int fat32_lfn_alias(vfs_node_t *dir, const char *name,
                           char out_short[11], const fat32_walk_entry_t *exclude)
{
    char base[6] = {'_', '_', '_', '_', '_', '_'};
    char extension[3] = {' ', ' ', ' '};
    const char *dot = NULL;
    u32 used = 0U, ext_used = 0U;
    for (const char *p = name; *p; p++) if (*p == '.' && p != name) dot = p;
    for (const char *p = name; *p && p != dot && used < 6U; p++) {
        u8 c = (u8)*p;
        if (c == ' ' || c == '.') continue;
        if (c < 0x80U) {
            if (c >= 'a' && c <= 'z') c -= 32U;
            base[used++] = ((c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '-')
                               ? (char)c : '_';
        } else {
            base[used++] = '_';
            while ((p[1] & 0xc0U) == 0x80U) p++;
        }
    }
    if (dot && dot[1]) {
        for (const char *p = dot + 1U; *p && ext_used < 3U; p++) {
            u8 c = (u8)*p;
            if (c == ' ' || c == '.') continue;
            if (c < 0x80U) {
                if (c >= 'a' && c <= 'z') c -= 32U;
                extension[ext_used++] = ((c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-')
                    ? (char)c : '_';
            } else {
                extension[ext_used++] = '_';
                while ((p[1] & 0xc0U) == 0x80U) p++;
            }
        }
    }
    for (u32 number = 1U; number <= 99U; number++) {
        fat32_on_disk_entry_t candidate = {0};
        char spelling[13];
        u32 digits = number < 10U ? 1U : 2U;
        memset(candidate.name, ' ', sizeof(candidate.name));
        memcpy(candidate.name, base, 7U - digits);
        candidate.name[7U - digits] = '~';
        if (digits == 2U) candidate.name[6U] = (char)('0' + number / 10U);
        candidate.name[7U] = (char)('0' + number % 10U);
        memcpy(candidate.name + 8U, extension, sizeof(extension));
        if (!fat32_format_83_name(&candidate, spelling)) return VFS_ERR_BAD_FORMAT;
        fat32_read_view_t view = {0};
        fat32_query_t query = {.mode = FAT32_QUERY_CREATE, .name = spelling,
                                .exclude = exclude};
        int result = fat32_dir_query_view(dir, &query, &view);
        if (result != VFS_OK) return result;
        if (query.found) continue;
        memcpy(out_short, candidate.name, sizeof(candidate.name));
        return VFS_OK;
    }
    return VFS_ERR_NO_SPACE;
}

static int fat32_create_lfn_alias(vfs_node_t *dir, const char *name,
                                  char out_short[11])
{
    return fat32_lfn_alias(dir, name, out_short, NULL);
}

/* Scan only validated existing directory sectors. A long entry set is
 * published in one sector; consuming an end marker requires another zero
 * marker after the set in that same sector. */
static int fat32_create_lfn_slot(vfs_node_t *dir, u32 needed,
                                 fat32_replace_transaction_t *tx,
                                 u32 *out_cluster, u32 *out_short_slot)
{
    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    fat32_read_view_t view = {0};
    u8 sector[512];
    u32 current = fat32_node_cluster(dir);
    if (!needed || needed > 16U) return VFS_ERR_UNSUPPORTED;
    for (u32 traversed = 0U; traversed < FAT32_WALK_MAX_DIR_CLUSTERS;
         traversed++) {
        if (current < 2U || current >= fs->total_clusters)
            return VFS_ERR_BAD_FORMAT;
        for (u32 part = 0U; part < fs->sectors_per_cluster; part++) {
            u64 lba = fat32_cluster_to_lba(fs, current) + part;
            if (!fat32_mounted_read(dir->super, lba, 1U, sector)) return VFS_ERR_IO;
            u32 run = 0U, start = 0U;
            bool end_seen = false;
            for (u32 slot = 0U; slot < 16U; slot++) {
                u8 first = sector[slot * 32U];
                if (first != 0U && first != 0xe5U) { run = 0U; continue; }
                if (!run) start = slot;
                if (first == 0U) end_seen = true;
                if (++run == needed) {
                    if (end_seen && (slot == 15U || sector[(slot + 1U) * 32U]))
                        return VFS_ERR_NO_SPACE;
                    memcpy(tx->old_sector, sector, 512U);
                    memcpy(tx->proposed_sector, sector, 512U);
                    tx->directory_sector = lba;
                    *out_cluster = current;
                    *out_short_slot = part * 16U + start + needed - 1U;
                    return VFS_OK;
                }
            }
            if (end_seen) return VFS_ERR_NO_SPACE;
        }
        u32 next;
        int result = fat32_get_next_checked_view(dir->super, fs, current,
                                                   &next, &view);
        if (result != VFS_OK) return result;
        if (next == FAT32_EOC_MIN) return VFS_ERR_NO_SPACE;
        current = next;
    }
    return VFS_ERR_BUFFER_TOO_SMALL;
}

static int fat32_growth_visit(void *context, u32 cluster,
                              const fat32_walk_entry_t *entry)
{
    fat32_directory_growth_t *growth = (fat32_directory_growth_t *)context;
    if (!entry) {
        growth->tail_cluster = cluster;
        growth->chain_clusters++;
        if (growth->has_marker && !growth->target_cluster &&
            cluster != growth->marker_cluster)
            growth->target_cluster = cluster;
    } else if (entry->unused && !growth->has_marker) {
        growth->has_marker = true;
        growth->marker_cluster = cluster;
        growth->marker_slot = entry->slot;
    }
    return 0;
}

/* The strict ownership proof has already validated all directory mappings.
 * Put a complete name set in the sector immediately after the first end
 * marker. Clearing that marker's remaining sector slots is a separate, final
 * visibility write. If no following sector exists, append one cluster. */
static int fat32_create_prepare_growth(vfs_node_t *dir, u32 needed,
                                        fat32_replace_transaction_t *tx,
                                        u32 *out_cluster, u32 *out_short_slot)
{
    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    fat32_directory_growth_t *growth = tx->growth;
    fat32_read_view_t view = {0};
    int result = fat32_dir_walk_view(dir->super, fat32_node_cluster(dir),
                                    true, true, fat32_growth_visit,
                                    growth, &view);
    if (result != VFS_OK) return result;
    if (!growth->chain_clusters || !growth->tail_cluster)
        return VFS_ERR_BAD_FORMAT;
    if (growth->has_marker) {
        u32 part = growth->marker_slot / 16U;
        growth->marker_sector = fat32_cluster_to_lba(fs, growth->marker_cluster) + part;
        if (!fat32_mounted_read(dir->super, growth->marker_sector, 1U,
                                growth->marker_before)) return VFS_ERR_IO;
        memcpy(growth->marker_proposed, growth->marker_before, 512U);
        for (u32 slot = growth->marker_slot % 16U; slot < 16U; slot++) {
            if (growth->marker_before[slot * 32U] != 0U)
                return VFS_ERR_BAD_FORMAT;
            growth->marker_proposed[slot * 32U] = 0xe5U;
        }
        if (part + 1U < fs->sectors_per_cluster) {
            growth->target_cluster = growth->marker_cluster;
            growth->target_slot = (part + 1U) * 16U + needed - 1U;
        } else if (growth->target_cluster) {
            growth->target_slot = needed - 1U;
        }
    }
    if (!growth->target_cluster) {
        u32 slots = fs->cluster_size_bytes / 32U;
        if (growth->chain_clusters >= FAT32_WALK_MAX_DIR_CLUSTERS ||
            growth->chain_clusters > FAT32_SCAN_MAX_ENTRIES / slots - 1U)
            return VFS_ERR_BUFFER_TOO_SMALL;
        growth->append = true;
        growth->target_slot = needed - 1U;
        memset(tx->old_sector, 0, sizeof(tx->old_sector));
        memset(tx->proposed_sector, 0, sizeof(tx->proposed_sector));
    } else {
        u32 part = growth->target_slot / 16U;
        tx->directory_sector = fat32_cluster_to_lba(fs, growth->target_cluster) + part;
        if (!fat32_mounted_read(dir->super, tx->directory_sector, 1U,
                                tx->old_sector)) return VFS_ERR_IO;
        for (u32 i = 0; i < 512U; i++)
            if (tx->old_sector[i] != 0U) return VFS_ERR_BAD_FORMAT;
        memcpy(tx->proposed_sector, tx->old_sector, 512U);
    }
    *out_cluster = growth->target_cluster;
    *out_short_slot = growth->target_slot;
    return VFS_OK;
}

static fat32_lfn_entry_t fat32_create_lfn_entry(const u16 *units,
    u32 unit_count, u32 sequence, u32 total, const char short_name[11])
{
    u16 words[13];
    fat32_lfn_entry_t entry = {0};
    for (u32 i = 0U; i < 13U; i++) {
        u32 offset = (sequence - 1U) * 13U + i;
        words[i] = offset < unit_count ? units[offset] :
                   offset == unit_count ? 0U : 0xffffU;
    }
    entry.order = (u8)sequence | (sequence == total ? 0x40U : 0U);
    entry.attr = 0x0fU;
    entry.checksum = fat32_lfn_checksum(short_name);
    memcpy(entry.name1, words, 5U * sizeof(u16));
    memcpy(entry.name2, words + 5U, 6U * sizeof(u16));
    memcpy(entry.name3, words + 11U, 2U * sizeof(u16));
    return entry;
}

static void fat32_create_lfn_sector(fat32_replace_transaction_t *tx,
                                     const u16 *units, u32 unit_count,
                                     u32 short_slot, u32 lfn_count,
                                     const char short_name[11])
{
    u32 first_slot = short_slot % 16U - lfn_count;
    for (u32 index = 0U; index < lfn_count; index++) {
        fat32_lfn_entry_t entry = fat32_create_lfn_entry(units, unit_count,
            lfn_count - index, lfn_count, short_name);
        memcpy(tx->proposed_sector + (first_slot + index) * 32U,
               &entry, sizeof(entry));
    }
}

/* A 255-byte VFS name occupies at most 20 LFNs and one short slot. Keep only
 * those positions while streaming the validated directory chain; never build
 * a directory-sized cache. A zero marker becomes the final visibility sector.
 * Without a marker, the short-entry sector is the visibility boundary. */
static int fat32_create_cross_sector_plan(vfs_node_t *dir,
    fat32_replace_transaction_t *tx, const u16 *units, u32 unit_count,
    u32 lfn_count, const char short_name[11],
    u32 *out_cluster, u32 *out_slot)
{
    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    fat32_lfn_creation_t *set = tx->lfn_creation;
    fat32_directory_growth_t *growth = tx->growth;
    fat32_read_view_t view = {0};
    u32 positions_cluster[21], positions_slot[21];
    u8 positions_fresh[21], image_index[21];
    u32 needed = lfn_count + 1U, found = 0U;
    u32 current = fat32_node_cluster(dir), chain_count = 0U;
    bool marker = false, complete = false;
    u8 sector[512];

    if (!set || !growth || needed < 2U || needed > 21U)
        return VFS_ERR_INVALID_PARAM;
    while (chain_count < FAT32_WALK_MAX_DIR_CLUSTERS) {
        if (current < 2U || current >= fs->total_clusters)
            return VFS_ERR_BAD_FORMAT;
        chain_count++;
        growth->tail_cluster = current;
        for (u32 part = 0U; part < fs->sectors_per_cluster; part++) {
            u64 lba = fat32_cluster_to_lba(fs, current) + part;
            if (!fat32_mounted_read(dir->super, lba, 1U, sector))
                return VFS_ERR_IO;
            for (u32 index = 0U; index < 16U; index++) {
                u8 first = sector[index * 32U];
                if (marker && first != 0U) return VFS_ERR_BAD_FORMAT;
                if (complete) continue;
                if (!marker && first == 0U) {
                    marker = true;
                    found = 0U;
                } else if (!marker && first != 0xe5U) {
                    found = 0U;
                    continue;
                }
                positions_cluster[found] = current;
                positions_slot[found] = part * 16U + index;
                positions_fresh[found++] = 0U;
                if (found == needed) complete = true;
            }
        }
        u32 next;
        int result = fat32_get_next_checked_view(dir->super, fs, current,
                                                  &next, &view);
        if (result != VFS_OK) return result;
        if (next == FAT32_EOC_MIN) break;
        if (next < 2U || next >= fs->total_clusters)
            return VFS_ERR_BAD_FORMAT;
        current = next;
    }
    if (chain_count == FAT32_WALK_MAX_DIR_CLUSTERS && !complete)
        return VFS_ERR_BUFFER_TOO_SMALL;
    /* The established one-sector growth path keeps its existing publication
     * protocol for names that fit there. */
    if (needed <= 16U && (marker || !complete))
        return VFS_ERR_NO_SPACE;
    if (!complete && !marker) found = 0U;
    if (!complete) {
        u32 slots = fs->cluster_size_bytes / 32U;
        u32 missing = needed - found;
        u32 append = missing / slots + (missing % slots != 0U);
        if (!append || append > 2U ||
            chain_count > FAT32_WALK_MAX_DIR_CLUSTERS - append ||
            chain_count > FAT32_SCAN_MAX_ENTRIES / slots - append)
            return VFS_ERR_BUFFER_TOO_SMALL;
        growth->append = true;
        growth->append_count = (u8)append;
        for (u32 i = 0U; i < missing; i++) {
            positions_cluster[found] = 0U;
            positions_fresh[found] = (u8)(i / slots + 1U);
            positions_slot[found++] = i % slots;
        }
    }
    if (found != needed) return VFS_ERR_BAD_FORMAT;
    set->marker_visibility = marker;
    for (u32 i = 0U; i < needed; i++) {
        u8 fresh = positions_fresh[i];
        u32 part = positions_slot[i] / 16U;
        u64 lba = fresh ? 0U :
            fat32_cluster_to_lba(fs, positions_cluster[i]) + part;
        u32 image = 0U;
        while (image < set->count &&
               (set->fresh_index[image] != fresh ||
                set->sector[image] != lba ||
                set->fresh_part[image] != part)) image++;
        if (image == set->count) {
            if (image == FAT32_UNLINK_MAX_SECTORS)
                return VFS_ERR_BAD_FORMAT;
            set->fresh_index[image] = fresh;
            set->fresh_part[image] = part;
            set->sector[image] = lba;
            if (!fresh && !fat32_mounted_read(dir->super, lba, 1U,
                                               set->before[image]))
                return VFS_ERR_IO;
            memcpy(set->proposed[image], set->before[image], 512U);
            set->count++;
        }
        u32 offset = positions_slot[i] % 16U * 32U;
        u8 first = set->before[image][offset];
        if (fresh ? first != 0U : marker ? first != 0U : first != 0xe5U)
            return VFS_ERR_BAD_FORMAT;
        image_index[i] = (u8)image;
        if (i < lfn_count) {
            fat32_lfn_entry_t entry = fat32_create_lfn_entry(units,
                unit_count, lfn_count - i, lfn_count, short_name);
            memcpy(set->proposed[image] + offset, &entry, sizeof(entry));
        }
    }
    set->short_image = image_index[lfn_count];
    set->visibility_image = marker ? image_index[0] : set->short_image;
    *out_cluster = positions_cluster[lfn_count];
    *out_slot = positions_slot[lfn_count];
    return set->count > 1U ? VFS_OK : VFS_ERR_BAD_FORMAT;
}

static int fat32_create_cross_sector_publish(vfs_node_t *dir,
    fat32_replace_transaction_t *tx)
{
    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    fat32_lfn_creation_t *set = tx->lfn_creation;
    if (!set || !set->count) return VFS_ERR_BAD_FORMAT;
    for (u32 i = 0U; i < set->count; i++) {
        if (i == set->short_image || i == set->visibility_image) continue;
        set->attempted_mask |= 1U << i;
        tx->phase = FAT32_REPLACE_PUBLISHING;
        if (!fat32_mounted_write(dir->super, set->sector[i], 1U,
                                 set->proposed[i])) return VFS_ERR_UNCERTAIN;
        set->confirmed_mask |= 1U << i;
    }
    if (!block_flush_for_id(dir->super->dev, fs->device_id))
        return VFS_ERR_UNCERTAIN;
    set->preparation_flushed = true;
    if (set->short_image != set->visibility_image) {
        u32 i = set->short_image;
        set->attempted_mask |= 1U << i;
        if (!fat32_mounted_write(dir->super, set->sector[i], 1U,
                                 set->proposed[i])) return VFS_ERR_UNCERTAIN;
        set->confirmed_mask |= 1U << i;
        if (!block_flush_for_id(dir->super->dev, fs->device_id))
            return VFS_ERR_UNCERTAIN;
        set->short_flushed = true;
    }
    u32 visible = set->visibility_image;
    set->attempted_mask |= 1U << visible;
    tx->phase = FAT32_REPLACE_DIR_RELOCATING;
    if (!fat32_mounted_write(dir->super, set->sector[visible], 1U,
                             set->proposed[visible])) return VFS_ERR_UNCERTAIN;
    set->confirmed_mask |= 1U << visible;
    if (!block_flush_for_id(dir->super->dev, fs->device_id))
        return VFS_ERR_UNCERTAIN;
    return VFS_OK;
}

/* Called only for a VFS-created node that was never returned to a caller.
 * This occurs while the mount is detached or its transaction still gates
 * access, so no FAT32 lookup can observe the node being removed. */
static void fat32_discard_created_node(vfs_node_t *node, vfs_super_t *sb)
{
    if (!node || !sb || !sb->private_data || !node->fs_data) return;
    fat32_fs_t *fs = (fat32_fs_t *)sb->private_data;
    fat32_node_state_t *state = fat32_state(node);
    fat32_node_state_t **link = &fs->nodes;
    while (*link && *link != state) link = &(*link)->next;
    if (*link == state) *link = state->next;
    node->fs_data = NULL;
    kfree(state);
}

/* Directory contents are generated only after their private cluster has been
 * allocated. This source is staging input, not retained recovery evidence. */
typedef struct {
    fat32_replace_transaction_t *tx;
    u32 parent;
} fat32_directory_source_t;

static u32 fat32_directory_source(void *context, u64 offset,
                                  u8 *buffer, u32 length)
{
    fat32_directory_source_t *source = context;
    memset(buffer, 0, length);
    if (offset == 0U) {
        if (length < 64U || !source->tx->new_first) return 0U;
        fat32_on_disk_entry_t *entry = (fat32_on_disk_entry_t *)buffer;
        for (u32 i = 0U; i < 2U; i++) {
            memset(entry[i].name, ' ', 11U);
            entry[i].name[0] = '.';
            if (i) entry[i].name[1] = '.';
            entry[i].attr = 0x10U;
            u32 first = i ? source->parent : source->tx->new_first;
            entry[i].first_cluster_high = (u16)(first >> 16U);
            entry[i].first_cluster_low = (u16)first;
        }
    }
    return length;
}

/* Creation and rename share placement, growth and bounded LFN images. The
 * caller has already validated ownership and namespace collision policy. */
static int fat32_namespace_placement_prepare(vfs_node_t *dir,
    fat32_replace_transaction_t *tx, fat32_walk_entry_t *slot,
    bool long_name, bool has_free_slot, const u16 *lfn_units,
    u32 lfn_units_count, u32 lfn_count, const char short_name[11],
    bool *out_cross_sector)
{
    fat32_fs_t *fs = dir->super->private_data;
    bool cross_sector = false;
    int result = VFS_OK;
    if (long_name && lfn_count + 1U <= 16U) {
        result = fat32_create_lfn_slot(dir, lfn_count + 1U, tx,
                                        &slot->cluster, &slot->slot);
        if (result != VFS_OK && result != VFS_ERR_NO_SPACE) {
            if (result == VFS_ERR_BAD_FORMAT) fs->version_confident = false;
            return result;
        }
    } else if (!long_name && has_free_slot) {
        if (slot->cluster < 2U || slot->cluster >= fs->total_clusters ||
            slot->slot >= fs->cluster_size_bytes / sizeof(slot->disk)) {
            result = VFS_ERR_BAD_FORMAT;
            return result;
        }
        tx->directory_sector = fat32_cluster_to_lba(fs, slot->cluster) +
            slot->slot / (512U / sizeof(slot->disk));
        if (!fat32_mounted_read(dir->super, tx->directory_sector, 1U,
                                tx->old_sector)) {
            result = VFS_ERR_IO;
            return result;
        }
        fat32_on_disk_entry_t *old_entry =
            &((fat32_on_disk_entry_t *)tx->old_sector)[slot->slot % 16U];
        if (memcmp(old_entry, &slot->disk, sizeof(*old_entry)) != 0 ||
            ((u8)old_entry->name[0] != 0U &&
             (u8)old_entry->name[0] != 0xe5U)) {
            fs->version_confident = false;
            result = VFS_ERR_BAD_FORMAT;
            return result;
        }
        memcpy(tx->proposed_sector, tx->old_sector, 512U);
    }
    if (long_name && (lfn_count + 1U > 16U || result == VFS_ERR_NO_SPACE)) {
        tx->growth = (fat32_directory_growth_t *)kmalloc(sizeof(*tx->growth));
        if (!tx->growth) return VFS_ERR_NO_MEM;
        /* Cleanup inspects second_images even if the next allocation fails. */
        memset(tx->growth, 0, sizeof(*tx->growth));
        tx->lfn_creation = (fat32_lfn_creation_t *)kmalloc(sizeof(*tx->lfn_creation));
        if (!tx->lfn_creation) return VFS_ERR_NO_MEM;
        memset(tx->lfn_creation, 0, sizeof(*tx->lfn_creation));
        tx->growth->fat_step = 4U;
        result = fat32_create_cross_sector_plan(dir, tx, lfn_units,
            lfn_units_count, lfn_count, short_name, &slot->cluster, &slot->slot);
        if (result == VFS_OK) {
            cross_sector = true;
            if (!tx->growth->append) {
                kfree(tx->growth);
                tx->growth = NULL;
            } else if (tx->growth->append_count == 2U) {
                tx->growth->second_images =
                    (fat32_growth_second_images_t *)kmalloc(
                        sizeof(*tx->growth->second_images));
                if (!tx->growth->second_images) {
                    result = VFS_ERR_NO_MEM;
                    return result;
                }
                memset(tx->growth->second_images, 0,
                       sizeof(*tx->growth->second_images));
            }
            if (tx->growth && tx->growth->append && !tx->data_buffer) {
                tx->data_buffer = (u8 *)kmalloc(fs->cluster_size_bytes);
                if (!tx->data_buffer) { result = VFS_ERR_NO_MEM; return result; }
            }
        }
        else if (result == VFS_ERR_NO_SPACE) {
            kfree(tx->lfn_creation);
            tx->lfn_creation = NULL;
            memset(tx->growth, 0, sizeof(*tx->growth));
            tx->growth->fat_step = 4U;
        } else {
            if (result == VFS_ERR_BAD_FORMAT) fs->version_confident = false;
            return result;
        }
    }
    if (!cross_sector && ((long_name && result == VFS_ERR_NO_SPACE) ||
        (!long_name && !has_free_slot))) {
        if (!tx->growth) {
            tx->growth = (fat32_directory_growth_t *)kmalloc(sizeof(*tx->growth));
            if (!tx->growth) { result = VFS_ERR_NO_MEM; return result; }
            memset(tx->growth, 0, sizeof(*tx->growth));
            tx->growth->fat_step = 4U;
        }
        result = fat32_create_prepare_growth(dir, lfn_count + 1U, tx,
                                              &slot->cluster, &slot->slot);
        if (result != VFS_OK) {
            if (result == VFS_ERR_BAD_FORMAT) fs->version_confident = false;
            return result;
        }
        if (tx->growth->append && !tx->data_buffer) {
            tx->data_buffer = (u8 *)kmalloc(fs->cluster_size_bytes);
            if (!tx->data_buffer) { result = VFS_ERR_NO_MEM; return result; }
        }
    }
    *out_cross_sector = cross_sector;
    return VFS_OK;
}

/* The caller owns operation_lock and a private, preallocated VFS node. No
 * path lookup or VFS allocation follows the visibility write. */
static int fat32_create_entry_locked(vfs_node_t *dir,
                                      vfs_node_t *prepared_node,
                                      const char *name, u64 length,
                                      vfs_replace_source_t source,
                                      void *context,
                                      vfs_file_facts_t *out_facts,
                                      u64 *out_pin, bool directory)
{
    fat32_fs_t *fs;
    fat32_replace_transaction_t *tx = NULL;
    fat32_walk_entry_t slot;
    fat32_node_state_t *parent;
    char short_name[11];
    u16 lfn_units[255];
    u32 lfn_units_count = 0U, lfn_count = 0U;
    bool long_name;
    bool cross_sector = false;
    u32 new_count, entry_in_sector;
    bool has_free_slot;
    int result;

    if (!dir || !prepared_node || !name || !out_facts || !out_pin ||
        !dir->super || !dir->super->private_data ||
        (length && !source && !directory)) return VFS_ERR_INVALID_PARAM;
    *out_pin = 0;
    if (dir->super->read_only) return VFS_ERR_ACCESS_DENIED;
    memset(out_facts, 0, sizeof(*out_facts));
    if (length > FAT32_REPLACE_MAX_BYTES) return VFS_ERR_NO_SPACE;
    long_name = !fat32_make_83_name(name, short_name);
    if (long_name) {
        if (!fat32_create_lfn_units(name, lfn_units, &lfn_units_count))
            return VFS_ERR_INVALID_PARAM;
        lfn_count = (lfn_units_count + 12U) / 13U;
        if (lfn_count > 20U) return VFS_ERR_INVALID_PARAM;
    }
    fs = (fat32_fs_t *)dir->super->private_data;
    if (fs->bytes_per_sector != 512U || !fs->sectors_per_cluster ||
        fs->sectors_per_cluster > 128U ||
        fs->cluster_size_bytes != fs->sectors_per_cluster * 512U ||
        !fs->fat_count || fs->fat_count > 4U)
        return VFS_ERR_BAD_FORMAT;
    if (!fs->version_confident || fs->replace_transaction)
        return VFS_ERR_UNCERTAIN;
    parent = fat32_state(dir);
    if (dir->type != VFS_TYPE_DIRECTORY || !parent || !parent->alive ||
        !parent->version_valid || parent->node != dir ||
        fat32_node_cluster(dir) == FAT32_CLUSTER_ERR)
        return VFS_ERR_NOT_DIRECTORY;
    if (!vfs_check_access(dir, VFS_ACCESS_WRITE))
        return VFS_ERR_ACCESS_DENIED;
    if (parent->generation == ~(u64)0 || fs->next_inode == ~(u64)0)
        return VFS_ERR_NO_SPACE;
    result = fat32_check_name_available_locked_view(
        dir, name, long_name ? NULL : &slot);
    if (result == VFS_ERR_INVALID_PARAM) return VFS_ERR_ALREADY_EXISTS;
    has_free_slot = result == VFS_OK;
    if (result != VFS_OK && result != VFS_ERR_NO_SPACE) return result;
    result = fat32_ownership_preflight_locked(dir);
    if (result != VFS_OK) return result;
    if (long_name) {
        result = fat32_create_lfn_alias(dir, name, short_name);
        if (result != VFS_OK) return result;
    }

    if (directory) length = fs->cluster_size_bytes;
    new_count = (u32)(length / fs->cluster_size_bytes +
                      (length % fs->cluster_size_bytes != 0U));
    tx = (fat32_replace_transaction_t *)kmalloc(sizeof(*tx));
    if (!tx) return VFS_ERR_NO_MEM;
    memset(tx, 0, sizeof(*tx));
    tx->new_size = directory ? 0U : (u32)length;
    tx->fresh_capacity = new_count;
    tx->record_id = fs->next_inode + 1U;
    tx->creating = true;
    tx->phase = FAT32_REPLACE_STAGING;
    tx->created_state = (fat32_node_state_t *)kmalloc(sizeof(*tx->created_state));
    if (!tx->created_state) { result = VFS_ERR_NO_MEM; goto before_pin; }
    memset(tx->created_state, 0, sizeof(*tx->created_state));
    if (new_count) {
        tx->fresh_clusters = (u32 *)kmalloc((usize)new_count * sizeof(u32));
        tx->data_buffer = (u8 *)kmalloc(fs->cluster_size_bytes);
        if (!tx->fresh_clusters || !tx->data_buffer) {
            result = VFS_ERR_NO_MEM;
            goto before_pin;
        }
    }
    result = fat32_namespace_placement_prepare(dir, tx, &slot, long_name,
        has_free_slot, lfn_units, lfn_units_count, lfn_count, short_name,
        &cross_sector);
    if (result != VFS_OK) goto before_pin;
    entry_in_sector = slot.slot % 16U;
    if (long_name && !cross_sector)
        fat32_create_lfn_sector(tx, lfn_units, lfn_units_count,
                                 slot.slot, lfn_count, short_name);
    fat32_on_disk_entry_t *entry =
        &((fat32_on_disk_entry_t *)(cross_sector ?
            tx->lfn_creation->proposed[tx->lfn_creation->short_image] :
            tx->proposed_sector))[entry_in_sector];
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->name, short_name, sizeof(entry->name));
    entry->attr = directory ? 0x10U : 0x20U;
    result = fat32_replace_prepare_fsinfo(dir->super, fs, tx);
    if (result != VFS_OK) {
        if (result == VFS_ERR_BAD_FORMAT) fs->version_confident = false;
        goto before_pin;
    }
    fs->replace_transaction = tx;
    result = vfs_recovery_pin_begin_handoff(
        dir->super, fat32_replace_finish_handoff, tx, &tx->pin_id);
    if (result != VFS_OK) {
        fs->replace_transaction = NULL;
        goto before_pin;
    }
    fs->version_confident = false;
    if (cross_sector) fs->namespace_hidden = true;
    fat32_directory_source_t directory_source = {
        .tx = tx,
        .parent = parent->first_cluster == fs->root_cluster ?
                  0U : parent->first_cluster,
    };
    if (directory) {
        source = fat32_directory_source;
        context = &directory_source;
    }
    result = fat32_stage_fresh(dir->super, fs, tx, length, source, context);
    /* Contents (including dot entries) and allocation must precede any
     * namespace visibility write, even without parent-directory growth. */
    if (result == VFS_OK && tx->fresh_count &&
        !block_flush_for_id(dir->super->dev, fs->device_id))
        goto uncertain;
    if (result == VFS_ERR_BAD_FORMAT || result == VFS_ERR_UNCERTAIN)
        goto uncertain;
    if (result != VFS_OK) goto cancel;
    if (tx->growth) {
        if (tx->growth->append) {
            result = fat32_create_stage_directory(dir->super, fs, tx,
                                                   &slot.cluster);
            if (result == VFS_ERR_BAD_FORMAT || result == VFS_ERR_UNCERTAIN)
                goto uncertain;
            if (result != VFS_OK) goto cancel;
        }
    }
    if (cross_sector) {
        fat32_lfn_creation_t *set = tx->lfn_creation;
        for (u32 i = 0U; i < set->count; i++) {
            if (set->fresh_index[i]) {
                u32 cluster = set->fresh_index[i] == 1U ?
                    tx->growth->candidate : tx->growth->second_candidate;
                if (cluster < 2U || cluster >= fs->total_clusters)
                    goto uncertain;
                set->sector[i] = fat32_cluster_to_lba(fs, cluster) +
                                 set->fresh_part[i];
            }
        }
        u8 fresh = set->fresh_index[set->short_image];
        if (fresh)
            slot.cluster = fresh == 1U ? tx->growth->candidate :
                           tx->growth->second_candidate;
        tx->directory_sector = set->sector[set->short_image];
        memcpy(tx->old_sector, set->before[set->short_image], 512U);
        memcpy(tx->proposed_sector, set->proposed[set->short_image], 512U);
    }
    entry = &((fat32_on_disk_entry_t *)(cross_sector ?
        tx->lfn_creation->proposed[tx->lfn_creation->short_image] :
        tx->proposed_sector))[entry_in_sector];
    entry->first_cluster_high = (u16)(tx->new_first >> 16U);
    entry->first_cluster_low = (u16)tx->new_first;
    entry->file_size = directory ? 0U : (u32)length;
    if (cross_sector)
        memcpy(tx->proposed_sector,
               tx->lfn_creation->proposed[tx->lfn_creation->short_image],
               512U);
    tx->phase = FAT32_REPLACE_PUBLISHING;
    if (cross_sector) {
        result = fat32_create_cross_sector_publish(dir, tx);
        if (result != VFS_OK) goto uncertain;
    } else if (!fat32_mounted_write(dir->super, tx->directory_sector, 1U,
                                   tx->proposed_sector)) goto uncertain;
    if (tx->growth && !cross_sector) {
        tx->growth->entry_written = true;
        if (!block_flush_for_id(dir->super->dev, fs->device_id))
            goto uncertain;
        if (tx->growth->has_marker) {
            tx->phase = FAT32_REPLACE_DIR_RELOCATING;
            tx->growth->marker_attempted = true;
            if (!fat32_mounted_write(dir->super,
                    tx->growth->marker_sector, 1U,
                    tx->growth->marker_proposed) ||
                !block_flush_for_id(dir->super->dev, fs->device_id))
                goto uncertain;
        }
    }
    if (cross_sector && tx->growth) tx->growth->entry_written = true;
    result = fat32_replace_fsinfo(dir->super, tx);
    if (result != VFS_OK) goto uncertain;
    if (!block_flush_for_id(dir->super->dev, fs->device_id)) goto uncertain;

    fat32_node_state_t *state = tx->created_state;
    state->node = prepared_node;
    state->first_cluster = tx->new_first;
    state->parent_cluster = slot.cluster;
    state->entry_index = slot.slot;
    state->generation = 1U;
    state->alive = true;
    state->version_valid = true;
    state->next = fs->nodes;
    fs->nodes = state;
    prepared_node->fs_data = state;
    tx->created_state = NULL;
    fs->next_inode = tx->record_id;
    fat32_version_advance(parent);
    *out_facts = (vfs_file_facts_t){
        .record_id = tx->record_id,
        .generation = 1U,
        .size = directory ? 0U : length,
        .owner_uid = VFS_UID_SYSTEM,
        .permissions = FAT32_VOLUME_PERMISSIONS,
    };
    kfree(tx->data_buffer);
    tx->data_buffer = NULL;
    tx->phase = FAT32_REPLACE_HANDOFF_PUBLISHED;
    tx->handoff_pending = true;
    *out_pin = tx->pin_id;
    return VFS_OK;

cancel:
    if (fat32_create_cancel_directory(dir->super, fs, tx) != VFS_OK)
        goto uncertain;
    if (fat32_replace_cancel(dir->super, fs, tx) != VFS_OK) goto uncertain;
    if ((tx->fresh_count || (tx->growth && tx->growth->candidate_recorded)) &&
        !block_flush_for_id(dir->super->dev, fs->device_id)) goto uncertain;
    kfree(tx->data_buffer);
    tx->data_buffer = NULL;
    tx->phase = FAT32_REPLACE_HANDOFF_CANCELLED;
    tx->handoff_pending = true;
    *out_pin = tx->pin_id;
    return result;
uncertain:
    kfree(tx->data_buffer);
    tx->data_buffer = NULL;
    (void)vfs_recovery_pin_mark_unresolved(tx->pin_id);
    return VFS_ERR_UNCERTAIN;
before_pin:
    fat32_replace_free(tx);
    return result;
}

static int fat32_create_contents_locked(vfs_node_t *dir,
    vfs_node_t *prepared_node, const char *name, u64 length,
    vfs_replace_source_t source, void *context,
    vfs_file_facts_t *facts, u64 *pin)
{
    return fat32_create_entry_locked(dir, prepared_node, name, length,
                                     source, context, facts, pin, false);
}

static int fat32_create_node_pinned(vfs_node_t *dir, vfs_node_t *node,
    const char *name, u64 *pin, bool directory)
{
    vfs_file_facts_t facts;
    u32 owner;
    if (!vfs_current_uid(&owner)) return VFS_ERR_ACCESS_DENIED;
    int result = fat32_create_entry_locked(dir, node, name, 0U, NULL, NULL,
                                           &facts, pin, directory);
    if (result != VFS_OK) return result;
    node->inode = facts.record_id;
    node->type = directory ? VFS_TYPE_DIRECTORY : VFS_TYPE_FILE;
    node->size = 0U;
    vfs_node_set_security(node, owner, directory ?
        VFS_DEFAULT_DIRECTORY_PERMISSIONS : VFS_DEFAULT_FILE_PERMISSIONS);
    node->child_mutation_policy = VFS_CHILD_MUTATION_OPEN;
    node->ref_count = 1U;
    node->super = dir->super;
    node->ops = &fat32_node_ops;
    return VFS_OK;
}

static int fat32_create_pinned(vfs_node_t *dir, vfs_node_t *node,
                               const char *name, u64 *pin)
{
    return fat32_create_node_pinned(dir, node, name, pin, false);
}

static int fat32_mkdir_pinned(vfs_node_t *dir, vfs_node_t *node,
                              const char *name, u64 *pin)
{
    return fat32_create_node_pinned(dir, node, name, pin, true);
}

/* Legacy callbacks cannot transfer recovery ownership. VFS selects the
 * pinned hooks; direct legacy dispatch never mutates the filesystem. */
static int fat32_create(vfs_node_t *dir, const char *name, vfs_node_t **out_node)
{
    (void)dir; (void)name;
    if (out_node) *out_node = NULL;
    return VFS_ERR_UNSUPPORTED;
}

static int fat32_mkdir(vfs_node_t *dir, const char *name, vfs_node_t **out_node)
{
    return fat32_create(dir, name, out_node);
}

static int fat32_retirement_prepare(vfs_node_t *dir,
    const fat32_walk_entry_t *match, fat32_replace_transaction_t *tx)
{
    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    fat32_lfn_retirement_t *ret = tx->retirement;
    u32 length;
    if (!ret || (match->has_lfn &&
        (!fat32_lfn_length(&match->lfn, &match->disk, &length) ||
         !match->lfn.total || match->lfn.total > 20U)) ||
        (!match->has_lfn && match->lfn.total))
        return VFS_ERR_BAD_FORMAT;
    (void)length;
    for (u32 index = 0U; index <= match->lfn.total; index++) {
        u32 cluster = index ? match->lfn.cluster[index - 1U] : match->cluster;
        u32 slot = index ? match->lfn.slot[index - 1U] : match->slot;
        if (cluster < 2U || cluster >= fs->total_clusters ||
            slot >= fs->cluster_size_bytes / 32U) return VFS_ERR_BAD_FORMAT;
        u64 sector = fat32_cluster_to_lba(fs, cluster) + slot / 16U;
        u32 image = 0U;
        while (image < ret->count && ret->sector[image] != sector) image++;
        if (image == ret->count) {
            if (ret->count == FAT32_UNLINK_MAX_SECTORS)
                return VFS_ERR_BAD_FORMAT;
            ret->sector[image] = sector;
            if (!fat32_mounted_read(dir->super, sector, 1U,
                                    ret->before[image])) return VFS_ERR_IO;
            memcpy(ret->proposed[image], ret->before[image], 512U);
            ret->count++;
        }
        u8 *entry = ret->before[image] + (slot % 16U) * 32U;
        if (!index) {
            if (memcmp(entry, &match->disk, 32U)) return VFS_ERR_BAD_FORMAT;
        } else {
            const fat32_lfn_entry_t *lfn = (const fat32_lfn_entry_t *)entry;
            u32 ordinal = index;
            const u16 *units = match->lfn.units + (ordinal - 1U) * 13U;
            if (lfn->order != (u8)(ordinal |
                    (ordinal == match->lfn.total ? 0x40U : 0U)) ||
                lfn->attr != 0x0fU || lfn->type || lfn->first_cluster ||
                lfn->checksum != fat32_lfn_checksum(match->disk.name) ||
                memcmp(lfn->name1, units, 5U * sizeof(u16)) ||
                memcmp(lfn->name2, units + 5U, 6U * sizeof(u16)) ||
                memcmp(lfn->name3, units + 11U, 2U * sizeof(u16)))
                return VFS_ERR_BAD_FORMAT;
        }
        ret->proposed[image][(slot % 16U) * 32U] = 0xe5U;
    }
    return VFS_OK;
}

typedef struct { bool occupied; } fat32_empty_check_t;
static int fat32_empty_visit(void *context, u32 cluster,
                               const fat32_walk_entry_t *entry)
{
    fat32_empty_check_t *check = (fat32_empty_check_t *)context;
    (void)cluster;
    if (entry && !entry->unused && !entry->deleted && !entry->dot) {
        check->occupied = true;
        return 1;
    }
    return 0;
}

/* VFS finishes the pin after unlocking. The confirmed short-sector write
 * removes the name; remaining LFN sectors and the old chain are cleanup.
 * File cleanup uses a bounded cursor; directory chains retain their existing
 * bounded inventory. Namespace reads stay hidden until resolved handoff. */
static int fat32_retire_pinned(vfs_node_t *dir, const char *name,
                               bool directory, u64 *out_pin)
{
    fat32_walk_entry_t match;
    fat32_fs_t *fs;
    fat32_replace_transaction_t *tx = NULL;
    fat32_node_state_t *state;
    int result;
    if (!dir || !name || !out_pin || dir->type != VFS_TYPE_DIRECTORY ||
        !dir->super || !dir->super->private_data) return VFS_ERR_INVALID_PARAM;
    *out_pin = 0U;
    if (dir->super->read_only) return VFS_ERR_ACCESS_DENIED;
    fs = (fat32_fs_t *)dir->super->private_data;
    if (!fs->version_confident || fs->replace_transaction)
        return VFS_ERR_UNCERTAIN;
    result = fat32_dir_find(dir, name, &match);
    if (result != VFS_OK) return result;
    if (!!(match.disk.attr & 0x10U) != directory)
        return VFS_ERR_INVALID_PARAM;
    result = fat32_ownership_preflight_locked(dir);
    if (result != VFS_OK) return result;
    u32 first = fat32_entry_first(&match.disk);
    if (directory) {
        fat32_empty_check_t check = {0};
        result = fat32_dir_walk(dir->super, first, true, true,
                                fat32_empty_visit, &check);
        if (result != VFS_OK) {
            if (result == VFS_ERR_BAD_FORMAT) fs->version_confident = false;
            return result;
        }
        if (check.occupied) return VFS_ERR_NOT_EMPTY;
    }
    state = fat32_find_state(fs, match.cluster, match.slot);
    if (state &&
        (!state->version_valid || !state->node ||
         state->node->type != (directory ? VFS_TYPE_DIRECTORY : VFS_TYPE_FILE) ||
         state->first_cluster != first ||
         (!directory && state->node->size != match.disk.file_size))) {
        fs->version_confident = false;
        return VFS_ERR_BAD_FORMAT;
    }
    tx = (fat32_replace_transaction_t *)kmalloc(sizeof(*tx));
    if (!tx) return VFS_ERR_NO_MEM;
    memset(tx, 0, sizeof(*tx));
    tx->retirement = (fat32_lfn_retirement_t *)kmalloc(sizeof(*tx->retirement));
    if (!tx->retirement) { result = VFS_ERR_NO_MEM; goto before_pin; }
    memset(tx->retirement, 0, sizeof(*tx->retirement));
    result = fat32_retirement_prepare(dir, &match, tx);
    if (result != VFS_OK) {
        if (result == VFS_ERR_BAD_FORMAT) fs->version_confident = false;
        goto before_pin;
    }
    if (directory) {
        result = fat32_directory_old_inventory(dir->super, fs, first, tx);
    } else {
        result = fat32_retirement_chain_prepare(dir->super, fs, first,
                                                match.disk.file_size, tx);
    }
    if (result != VFS_OK) {
        if (result == VFS_ERR_BAD_FORMAT) fs->version_confident = false;
        goto before_pin;
    }
    result = fat32_replace_prepare_fsinfo(dir->super, fs, tx);
    if (result != VFS_OK) {
        if (result == VFS_ERR_BAD_FORMAT) fs->version_confident = false;
        goto before_pin;
    }
    tx->record_id = state ? state->node->inode : 0U;
    tx->phase = FAT32_REPLACE_UNLINK_PUBLISHING;
    fs->replace_transaction = tx;
    result = vfs_recovery_pin_begin_handoff(dir->super,
        fat32_replace_finish_handoff, tx, &tx->pin_id);
    if (result != VFS_OK) {
        fs->replace_transaction = NULL;
        goto before_pin;
    }
    fs->version_confident = false;
    fs->namespace_hidden = true;
    for (u32 i = 0U; i < tx->retirement->count; i++) {
        tx->retirement->write_attempted = i + 1U;
        if (!fat32_mounted_write(dir->super, tx->retirement->sector[i],
                                 1U, tx->retirement->proposed[i]))
            goto uncertain;
        tx->retirement->writes_confirmed = i + 1U;
        if (!i) {
            tx->retirement->short_confirmed = true;
            if (!block_flush_for_id(dir->super->dev, fs->device_id))
                goto uncertain;
            if (state) state->alive = false;
            tx->phase = FAT32_REPLACE_UNLINK_CLEANUP;
        }
    }
    if (!block_flush_for_id(dir->super->dev, fs->device_id)) goto uncertain;
    result = fat32_replace_reclaim(dir->super, fs, tx);
    if (result != VFS_OK) goto uncertain;
    result = fat32_replace_fsinfo(dir->super, tx);
    if (result != VFS_OK ||
        !block_flush_for_id(dir->super->dev, fs->device_id)) goto uncertain;
    fat32_version_advance(fat32_state(dir));
    tx->phase = FAT32_REPLACE_HANDOFF_PUBLISHED;
    tx->handoff_pending = true;
    *out_pin = tx->pin_id;
    return VFS_OK;
uncertain:
    (void)vfs_recovery_pin_mark_unresolved(tx->pin_id);
    return VFS_ERR_UNCERTAIN;
before_pin:
    fat32_replace_free(tx);
    return result;
}

static int fat32_unlink_pinned(vfs_node_t *dir, const char *name, u64 *out_pin)
{
    return fat32_retire_pinned(dir, name, false, out_pin);
}

static int fat32_rmdir_pinned(vfs_node_t *dir, const char *name, u64 *out_pin)
{
    return fat32_retire_pinned(dir, name, true, out_pin);
}

/* Mounted VFS dispatch selects the pinned callbacks. A direct legacy call
 * cannot transfer recovery ownership, so it must never mutate the namespace. */
static int fat32_unlink(vfs_node_t *dir, const char *name)
{
    (void)dir;
    (void)name;
    return VFS_ERR_UNSUPPORTED;
}

static int fat32_rmdir(vfs_node_t *dir, const char *name)
{
    (void)dir;
    (void)name;
    return VFS_ERR_UNSUPPORTED;
}

/* Operation-local views keep the retained superblock even if forced detach
 * retargets public nodes. Driver states themselves remain pin/reference owned. */
static int fat32_rename_directory_view(vfs_super_t *sb, vfs_node_t *node,
    vfs_node_t *view, fat32_node_state_t *state)
{
    fat32_fs_t *fs = sb->private_data;
    fat32_node_state_t *current = fat32_state(node);
    fat32_node_state_t *member = fs->nodes;
    while (member && member != current) member = member->next;
    if (!member || current->node != node || !current->alive ||
        !current->version_valid || node->type != VFS_TYPE_DIRECTORY)
        return VFS_ERR_NOT_DIRECTORY;
    *state = *current;
    /* Do not copy retargetable super/list fields from the public node. */
    *view = (vfs_node_t){.inode = node->inode, .type = node->type,
        .size = node->size, .super = sb, .fs_data = state,
        .ops = &fat32_node_ops};
    state->node = view;
    if (state->first_cluster < 2U || state->first_cluster >= fs->total_clusters)
        return VFS_ERR_BAD_FORMAT;
    if (node == sb->root_node && state->first_cluster != fs->root_cluster)
        return VFS_ERR_BAD_FORMAT;
    if (node != sb->root_node) {
        u8 sector[512];
        if (state->parent_cluster < 2U || state->parent_cluster >= fs->total_clusters ||
            state->entry_index >= fs->cluster_size_bytes / 32U)
            return VFS_ERR_BAD_FORMAT;
        if (!fat32_mounted_read(sb, fat32_cluster_to_lba(fs, state->parent_cluster) +
            state->entry_index / 16U, 1U, sector)) return VFS_ERR_IO;
        const fat32_on_disk_entry_t *disk = (const void *)(sector +
            state->entry_index % 16U * 32U);
        if (!(disk->attr & 0x10U) || (disk->attr & 0x08U) ||
            (u8)disk->name[0] == 0U || (u8)disk->name[0] == 0xe5U ||
            fat32_entry_first(disk) != state->first_cluster || disk->file_size)
            return VFS_ERR_BAD_FORMAT;
    }
    return VFS_OK;
}

/* The namespace proof has validated parent relationships. Follow '..' with
 * a volume-derived bound, require standard dot slots, and reject descendants
 * before allocating any transaction or changing media. */
static int fat32_rename_ancestry(vfs_super_t *sb, u32 moved, u32 destination)
{
    fat32_fs_t *fs = sb->private_data;
    u8 sector[512];
    for (u32 count = 0U; count < fs->total_clusters - 2U; count++) {
        if (destination == moved) return VFS_ERR_INVALID_PARAM;
        if (destination == fs->root_cluster) return VFS_OK;
        if (destination < 2U || destination >= fs->total_clusters)
            return VFS_ERR_BAD_FORMAT;
        if (!fat32_mounted_read(sb, fat32_cluster_to_lba(fs, destination),
                                1U, sector)) return VFS_ERR_IO;
        const fat32_on_disk_entry_t *dot = (const void *)sector;
        if (memcmp(dot[0].name, ".          ", 11U) ||
            memcmp(dot[1].name, "..         ", 11U) ||
            (dot[0].attr & 0x18U) != 0x10U || (dot[1].attr & 0x18U) != 0x10U ||
            fat32_entry_first(&dot[0]) != destination ||
            dot[0].file_size || dot[1].file_size) return VFS_ERR_BAD_FORMAT;
        destination = fat32_entry_first(&dot[1]);
        if (!destination) destination = fs->root_cluster;
    }
    return VFS_ERR_BAD_FORMAT;
}

static int fat32_rename_parent_prepare(vfs_super_t *sb,
    fat32_replace_transaction_t *tx, u32 moved, u32 old_parent, u32 new_parent)
{
    fat32_fs_t *fs = sb->private_data;
    fat32_rename_state_t *rename = tx->rename;
    rename->parent_sector = fat32_cluster_to_lba(fs, moved);
    if (!fat32_mounted_read(sb, rename->parent_sector, 1U,
                            rename->parent_before)) return VFS_ERR_IO;
    const fat32_on_disk_entry_t *dot = (const void *)rename->parent_before;
    if (memcmp(dot[0].name, ".          ", 11U) ||
        memcmp(dot[1].name, "..         ", 11U) ||
        (dot[0].attr & 0x18U) != 0x10U || (dot[1].attr & 0x18U) != 0x10U ||
        fat32_entry_first(&dot[0]) != moved ||
        (fat32_entry_first(&dot[1]) != old_parent &&
         !(old_parent == fs->root_cluster && !fat32_entry_first(&dot[1]))) ||
        dot[0].file_size || dot[1].file_size) return VFS_ERR_BAD_FORMAT;
    memcpy(rename->parent_proposed, rename->parent_before, 512U);
    fat32_on_disk_entry_t *parent = (void *)(rename->parent_proposed + 32U);
    if (new_parent == fs->root_cluster) new_parent = 0U;
    parent->first_cluster_high = (u16)(new_parent >> 16U);
    parent->first_cluster_low = (u16)new_parent;
    return VFS_OK;
}

static void fat32_rename_inplace_prepare(vfs_super_t *sb,
    fat32_replace_transaction_t *tx, const fat32_walk_entry_t *source,
    const u16 *units, u32 unit_count, u32 lfn_count, const char short_name[11])
{
    fat32_fs_t *fs = sb->private_data;
    fat32_lfn_creation_t *set = tx->lfn_creation;
    fat32_lfn_retirement_t *ret = tx->retirement;
    set->count = ret->count;
    for (u32 image = 0U; image < ret->count; image++) {
        set->sector[image] = ret->sector[image];
        memcpy(set->before[image], ret->before[image], 512U);
        memcpy(set->proposed[image], ret->proposed[image], 512U);
    }
    for (u32 ordinal = 1U; ordinal <= lfn_count; ordinal++) {
        u32 slot = source->lfn.slot[ordinal - 1U];
        u64 lba = fat32_cluster_to_lba(fs, source->lfn.cluster[ordinal - 1U]) + slot / 16U;
        fat32_lfn_entry_t lfn = fat32_create_lfn_entry(units, unit_count,
            ordinal, lfn_count, short_name);
        for (u32 image = 0U; image < ret->count; image++)
            if (set->sector[image] == lba)
                memcpy(set->proposed[image] + slot % 16U * 32U, &lfn, 32U);
    }
    /* Retirement inventories the short-entry sector first. */
    set->short_image = set->visibility_image = 0U;
    tx->directory_sector = set->sector[0];
    memcpy(tx->old_sector, set->before[0], 512U);
}

/* Refresh only the proposed retirement image. Keep the pretransaction image
 * and the immediately confirmed predecessor separately, including destination
 * entries that now share this sector. No allocation follows publication. */
static int fat32_rename_retirement_refresh(vfs_super_t *sb,
    fat32_replace_transaction_t *tx, const fat32_walk_entry_t *source)
{
    fat32_fs_t *fs = sb->private_data;
    fat32_lfn_retirement_t *ret = tx->retirement;
    for (u32 image = 0U; image < ret->count; image++) {
        u8 *predecessor = tx->rename->retirement_predecessor[image];
        if (!fat32_mounted_read(sb, ret->sector[image], 1U, predecessor))
            return VFS_ERR_IO;
        memcpy(ret->proposed[image], predecessor, 512U);
        for (u32 index = 0U; index <= source->lfn.total; index++) {
            u32 cluster = index ? source->lfn.cluster[index - 1U] : source->cluster;
            u32 slot = index ? source->lfn.slot[index - 1U] : source->slot;
            if (fat32_cluster_to_lba(fs, cluster) + slot / 16U != ret->sector[image])
                continue;
            u32 at = slot % 16U * 32U;
            if (memcmp(predecessor + at, ret->before[image] + at, 32U))
                return VFS_ERR_BAD_FORMAT;
            ret->proposed[image][at] = 0xe5U;
        }
    }
    return VFS_OK;
}

static int fat32_rename_pinned(vfs_super_t *sb, vfs_node_t *src_dir,
    const char *src_name, vfs_node_t *dst_dir, const char *dst_name, u64 *out_pin)
{
    fat32_fs_t *fs;
    fat32_walk_entry_t source, slot = {0};
    vfs_node_t src_view, dst_view, target;
    fat32_node_state_t src_state, dst_state, target_state;
    fat32_node_state_t *tracked, *src_parent, *dst_parent;
    fat32_replace_transaction_t *tx = NULL;
    u16 units[255];
    u32 unit_count = 0U, lfn_count = 0U;
    char short_name[11];
    bool long_name, cross_sector = false, inplace, same_parent;
    int result;

    if (!sb || !sb->private_data || !src_dir || !dst_dir || !out_pin ||
        !fat32_name_valid(src_name) || !fat32_create_lfn_units(dst_name, units, &unit_count))
        return VFS_ERR_INVALID_PARAM;
    *out_pin = 0U;
    if (sb->read_only) return VFS_ERR_ACCESS_DENIED;
    fs = sb->private_data;
    if (!fs->version_confident || fs->replace_transaction) return VFS_ERR_UNCERTAIN;
    if (!block_device_id_is_live(fs->device_id)) return VFS_ERR_DEVICE_GONE;
    if (fs->bytes_per_sector != 512U || !fs->sectors_per_cluster ||
        fs->sectors_per_cluster > 128U ||
        fs->cluster_size_bytes != fs->sectors_per_cluster * 512U ||
        !fs->fat_count || fs->fat_count > 4U) return VFS_ERR_BAD_FORMAT;
    result = fat32_rename_directory_view(sb, src_dir, &src_view, &src_state);
    if (result != VFS_OK) goto validation_failure;
    result = fat32_rename_directory_view(sb, dst_dir, &dst_view, &dst_state);
    if (result != VFS_OK) goto validation_failure;
    src_parent = fat32_state(src_dir);
    dst_parent = fat32_state(dst_dir);
    same_parent = src_state.first_cluster == dst_state.first_cluster;
    result = fat32_dir_find(&src_view, src_name, &source);
    if (result != VFS_OK) return result;
    u32 first = fat32_entry_first(&source.disk);
    vfs_node_type_t type = source.disk.attr & 0x10U ?
                          VFS_TYPE_DIRECTORY : VFS_TYPE_FILE;
    if ((first && (first < 2U || first >= fs->total_clusters)) ||
        (!first && (type == VFS_TYPE_DIRECTORY || source.disk.file_size))) {
        result = VFS_ERR_BAD_FORMAT;
        goto validation_failure;
    }
    tracked = fat32_find_state(fs, source.cluster, source.slot);
    if (tracked && !tracked->version_valid) return VFS_ERR_UNCERTAIN;
    if (tracked && (!tracked->node || tracked->first_cluster != first ||
        tracked->node->type != type || tracked->node->size != source.disk.file_size)) {
        result = VFS_ERR_BAD_FORMAT;
        goto validation_failure;
    }
    if (!tracked && fs->next_inode == ~(u64)0) return VFS_ERR_NO_SPACE;
    /* Bind observed identity before mutation; retained handles keep this node
     * after moving the physical directory slot. Tombstones are never rebound. */
    vfs_node_t *node = fat32_bind_node(sb, fs, source.cluster, source.slot,
        first, source.disk.file_size, type);
    if (!node) return fs->version_confident ? VFS_ERR_NO_MEM : VFS_ERR_UNCERTAIN;
    tracked = fat32_state(node);
    if (tracked->generation == ~(u64)0 || src_state.generation == ~(u64)0 ||
        dst_state.generation == ~(u64)0) return VFS_ERR_NO_SPACE;
    target_state = *tracked;
    target = (vfs_node_t){.inode = node->inode, .type = node->type,
        .size = node->size, .super = sb, .fs_data = &target_state,
        .ops = &fat32_node_ops};
    target_state.node = &target;
    result = fat32_ownership_preflight_locked(&target);
    if (result != VFS_OK) return result;
    fat32_query_t collision = {.mode = FAT32_QUERY_CREATE, .name = dst_name,
                               .exclude = same_parent ? &source : NULL};
    result = fat32_dir_query(&dst_view, &collision);
    if (result != VFS_OK) return result;
    if (collision.found) return VFS_ERR_ALREADY_EXISTS;
    if (source.disk.attr & 0x10U) {
        result = fat32_rename_ancestry(sb, tracked->first_cluster, dst_state.first_cluster);
        if (result != VFS_OK) goto validation_failure;
    }
    char current_name[256];
    if (same_parent &&
        (fat32_lfn_encode(&source.lfn, &source.disk, current_name, sizeof(current_name)) ||
         fat32_format_83_name(&source.disk, current_name)) && !strcmp(current_name, dst_name))
        return VFS_OK;
    long_name = !fat32_make_83_name(dst_name, short_name);
    /* Preserve requested case. Existing short-name creation keeps its current
     * convention; rename uses an LFN when uppercase 8.3 cannot spell it. */
    for (const char *p = dst_name; *p; p++)
        if (*p >= 'a' && *p <= 'z') long_name = true;
    if (long_name) {
        lfn_count = (unit_count + 12U) / 13U;
        result = fat32_lfn_alias(&dst_view, dst_name, short_name,
                                same_parent ? &source : NULL);
        if (result != VFS_OK) return result;
    }
    inplace = same_parent && lfn_count <= source.lfn.total;
    tx = kmalloc(sizeof(*tx));
    if (!tx) return VFS_ERR_NO_MEM;
    memset(tx, 0, sizeof(*tx));
    tx->rename = kmalloc(sizeof(*tx->rename));
    tx->retirement = kmalloc(sizeof(*tx->retirement));
    if (!tx->rename || !tx->retirement) { result = VFS_ERR_NO_MEM; goto before_pin; }
    memset(tx->rename, 0, sizeof(*tx->rename));
    memset(tx->retirement, 0, sizeof(*tx->retirement));
    tx->rename->inplace = inplace;
    tx->record_id = node->inode;
    tx->expected_generation = tracked->generation;
    result = fat32_retirement_prepare(&src_view, &source, tx);
    if (result != VFS_OK) goto before_pin;
    if (inplace) {
        tx->lfn_creation = kmalloc(sizeof(*tx->lfn_creation));
        if (!tx->lfn_creation) { result = VFS_ERR_NO_MEM; goto before_pin; }
        memset(tx->lfn_creation, 0, sizeof(*tx->lfn_creation));
        fat32_rename_inplace_prepare(sb, tx, &source, units, unit_count, lfn_count, short_name);
        slot = source;
        cross_sector = true;
    } else {
        if (collision.free_found) slot = collision.free_slot;
        result = fat32_namespace_placement_prepare(&dst_view, tx, &slot,
            long_name, collision.free_found, units, unit_count, lfn_count,
            short_name, &cross_sector);
        if (result != VFS_OK) goto before_pin;
        if (long_name && !cross_sector)
            fat32_create_lfn_sector(tx, units, unit_count, slot.slot, lfn_count, short_name);
    }
    tx->rename->destination_cluster = slot.cluster;
    tx->rename->destination_slot = slot.slot;
    fat32_on_disk_entry_t *entry = (void *)((cross_sector ?
        tx->lfn_creation->proposed[tx->lfn_creation->short_image] : tx->proposed_sector) +
        slot.slot % 16U * 32U);
    *entry = source.disk;
    /* Namespace authorization remains the VFS parent policy. Preserve the
     * read-only attribute; content mutations enforce it independently. */
    memcpy(entry->name, short_name, 11U);
    entry->nt_reserved &= (u8)~0x18U;
    if (cross_sector) memcpy(tx->proposed_sector,
        tx->lfn_creation->proposed[tx->lfn_creation->short_image], 512U);
    if ((source.disk.attr & 0x10U) && !same_parent) {
        result = fat32_rename_parent_prepare(sb, tx, tracked->first_cluster,
            src_state.first_cluster, dst_state.first_cluster);
        if (result != VFS_OK) goto before_pin;
    }
    result = fat32_replace_prepare_fsinfo(sb, fs, tx);
    if (result != VFS_OK) goto before_pin;
    fs->replace_transaction = tx;
    result = vfs_recovery_pin_begin_handoff(sb, fat32_replace_finish_handoff, tx, &tx->pin_id);
    if (result != VFS_OK) { fs->replace_transaction = NULL; goto before_pin; }
    fs->version_confident = false;
    fs->namespace_hidden = true;
    if (tx->growth && tx->growth->append) {
        result = fat32_create_stage_directory(sb, fs, tx, &slot.cluster);
        if (result == VFS_ERR_UNCERTAIN || result == VFS_ERR_BAD_FORMAT) goto uncertain;
        if (result != VFS_OK) goto cancel;
    }
    if (cross_sector) {
        fat32_lfn_creation_t *set = tx->lfn_creation;
        u8 fresh = set->fresh_index[set->short_image];
        if (fresh) slot.cluster = fresh == 1U ? tx->growth->candidate : tx->growth->second_candidate;
        tx->directory_sector = set->sector[set->short_image];
        tx->rename->destination_attempted = true;
        result = fat32_create_cross_sector_publish(&dst_view, tx);
        if (result != VFS_OK) goto uncertain;
    } else {
        tx->phase = FAT32_REPLACE_PUBLISHING;
        tx->rename->destination_attempted = true;
        if (!fat32_mounted_write(sb, tx->directory_sector, 1U, tx->proposed_sector) ||
            !block_flush_for_id(sb->dev, fs->device_id)) goto uncertain;
        if (tx->growth) {
            tx->growth->entry_written = true;
            if (tx->growth->has_marker) {
                tx->phase = FAT32_REPLACE_DIR_RELOCATING;
                tx->growth->marker_attempted = true;
                if (!fat32_mounted_write(sb, tx->growth->marker_sector, 1U,
                    tx->growth->marker_proposed) || !block_flush_for_id(sb->dev, fs->device_id))
                    goto uncertain;
            }
        }
    }
    if (tx->growth) tx->growth->entry_written = true;
    tx->rename->destination_confirmed = true;
    tx->rename->destination_cluster = slot.cluster;
    if (tx->rename->parent_sector) {
        tx->rename->parent_attempted = true;
        if (!fat32_mounted_write(sb, tx->rename->parent_sector, 1U,
                                 tx->rename->parent_proposed)) goto uncertain;
        tx->rename->parent_confirmed = true;
        if (!block_flush_for_id(sb->dev, fs->device_id)) goto uncertain;
        tx->rename->parent_flushed = true;
    }
    if (!inplace) {
        result = fat32_rename_retirement_refresh(sb, tx, &source);
        if (result != VFS_OK) goto uncertain;
        tx->phase = FAT32_REPLACE_UNLINK_PUBLISHING;
        for (u32 i = 0U; i < tx->retirement->count; i++) {
            tx->retirement->write_attempted = i + 1U;
            if (!fat32_mounted_write(sb, tx->retirement->sector[i], 1U,
                                     tx->retirement->proposed[i])) goto uncertain;
            tx->retirement->writes_confirmed = i + 1U;
            if (!i) {
                tx->retirement->short_confirmed = true;
                if (!block_flush_for_id(sb->dev, fs->device_id)) goto uncertain;
            }
        }
        if (!block_flush_for_id(sb->dev, fs->device_id)) goto uncertain;
    }
    tx->rename->source_retired = true;
    result = fat32_replace_fsinfo(sb, tx);
    if (result != VFS_OK || !block_flush_for_id(sb->dev, fs->device_id)) goto uncertain;
    tracked->parent_cluster = slot.cluster;
    tracked->entry_index = slot.slot;
    fat32_version_advance(tracked);
    fat32_version_advance(src_parent);
    if (dst_parent != src_parent) fat32_version_advance(dst_parent);
    kfree(tx->data_buffer); tx->data_buffer = NULL;
    tx->phase = FAT32_REPLACE_HANDOFF_PUBLISHED;
    tx->handoff_pending = true;
    *out_pin = tx->pin_id;
    return VFS_OK;
cancel:
    if (fat32_create_cancel_directory(sb, fs, tx) != VFS_OK ||
        !block_flush_for_id(sb->dev, fs->device_id)) goto uncertain;
    kfree(tx->data_buffer); tx->data_buffer = NULL;
    tx->phase = FAT32_REPLACE_HANDOFF_CANCELLED;
    tx->handoff_pending = true;
    *out_pin = tx->pin_id;
    return result;
uncertain:
    kfree(tx->data_buffer); tx->data_buffer = NULL;
    (void)vfs_recovery_pin_mark_unresolved(tx->pin_id);
    return VFS_ERR_UNCERTAIN;
before_pin:
    fat32_replace_free(tx);
validation_failure:
    if (result == VFS_ERR_BAD_FORMAT) fs->version_confident = false;
    return result;
}

/* A legacy callback cannot transfer recovery ownership. */
static int fat32_rename(vfs_node_t *src_dir, const char *src_name,
    vfs_node_t *dst_dir, const char *dst_name)
{
    (void)src_dir; (void)src_name; (void)dst_dir; (void)dst_name;
    return VFS_ERR_UNSUPPORTED;
}

static const vfs_ops_t fat32_node_ops = {
    .read = fat32_read,
    .read_checked = fat32_read_checked,
    .write = fat32_write,
    .write_pinned = fat32_write_pinned,
    .file_facts = fat32_file_facts,
    .replace = fat32_replace_locked,
    .replacement_max_bytes = FAT32_REPLACE_MAX_BYTES,
    .create_contents = fat32_create_contents_locked,
    .create_contents_max_bytes = FAT32_REPLACE_MAX_BYTES,
    .discard_created_node = fat32_discard_created_node,
    .snapshot_stream = fat32_snapshot_stream,
    .finddir = fat32_finddir,
    .readdir = fat32_readdir,
    .readdir_checked = fat32_readdir_checked,
    .readdir_open = NULL,
    .readdir_next = NULL,
    .readdir_close = NULL,
    .create = fat32_create,
    .mkdir = fat32_mkdir,
    .create_pinned = fat32_create_pinned,
    .mkdir_pinned = fat32_mkdir_pinned,
    .unlink = fat32_unlink,
    .unlink_pinned = fat32_unlink_pinned,
    .rmdir = fat32_rmdir,
    .rmdir_pinned = fat32_rmdir_pinned,
    .rename = fat32_rename,
    .rename_pinned = fat32_rename_pinned,
    .truncate = fat32_truncate,
    .truncate_pinned = fat32_truncate_pinned,
    .resize_pinned = fat32_resize_pinned,
};

static int fat32_unmount(vfs_super_t *sb) {
    if (!sb) return VFS_ERR_INVALID_PARAM;
    if (sb->private_data) {
        fat32_fs_t *fs = (fat32_fs_t *)sb->private_data;
        fat32_node_state_t *state = fs->nodes;
        while (state) {
            fat32_node_state_t *next = state->next;
            kfree(state);
            state = next;
        }
        kfree(sb->private_data);
        sb->private_data = NULL;
    }
    return VFS_OK;
}

/* VFS retains this superblock and holds operation_lock, including during clean
 * detach. Sync is not a recovery attempt and cannot resolve an attached pin. */
static int fat32_sync(vfs_super_t *sb)
{
    if (!sb || !sb->dev) return VFS_ERR_DEVICE_GONE;
    fat32_fs_t *fs = (fat32_fs_t *)sb->private_data;
    if (!fs) return VFS_ERR_IO;
    if (fs->replace_transaction || !fs->version_confident) return VFS_ERR_UNCERTAIN;
    if (sb->dev->id != fs->device_id || !block_device_id_is_live(fs->device_id))
        return VFS_ERR_DEVICE_GONE;
    if (!block_flush_for_id(sb->dev, fs->device_id)) {
        fs->version_confident = false;
        fs->ownership_proven = false;
        return sb->dev->id != fs->device_id || !block_device_id_is_live(fs->device_id)
            ? VFS_ERR_DEVICE_GONE : VFS_ERR_IO;
    }
    return VFS_OK;
}

static const vfs_super_ops_t fat32_super_ops = {
    .unmount = fat32_unmount,
    .sync = fat32_sync,
};

static bool fat32_probe_for_id(block_device_t *dev, u64 device_id) {
    u64 total_sectors;
    u64 fat_sectors;
    u64 data_start;
    u64 data_sectors;
    u64 cluster_count;
    u64 fat_entries;

    /* This driver uses fixed 512-byte I/O scratch sectors throughout.  Do
     * not pretend to support another logical-sector geometry until that
     * implementation is made explicit. */
    if (!dev || dev->sector_size != 512U || !dev->sector_count) return false;

    u8 sector_buf[512];
    if (!block_read_for_id(dev, device_id, 0, 1, sector_buf)) {
        return false;
    }

    if (sector_buf[510] != 0x55 || sector_buf[511] != 0xAA) {
        return false;
    }

    fat32_bpb_t *bpb = (fat32_bpb_t *)sector_buf;

    if (bpb->bytes_per_sector != 512U) return false;

    if (bpb->sectors_per_cluster == 0U || bpb->sectors_per_cluster > 128U ||
        (bpb->sectors_per_cluster & (bpb->sectors_per_cluster - 1U)) != 0U) {
        return false;
    }

    if (bpb->reserved_sector_count == 0U || bpb->fat_count == 0U ||
        bpb->sectors_per_fat_16 != 0U || bpb->sectors_per_fat_32 == 0U) {
        return false;
    }
    if ((bpb->ext_flags & 0x80U) &&
        (bpb->ext_flags & 0x0fU) >= bpb->fat_count) return false;

    total_sectors = bpb->total_sectors_32 ? bpb->total_sectors_32 :
                    bpb->total_sectors_16;
    if (!total_sectors || total_sectors > dev->sector_count) return false;
    fat_sectors = (u64)bpb->fat_count * bpb->sectors_per_fat_32;
    if (!fat_sectors || fat_sectors > ~(u64)0 - bpb->reserved_sector_count)
        return false;
    data_start = (u64)bpb->reserved_sector_count + fat_sectors;
    if (data_start >= total_sectors) return false;
    data_sectors = total_sectors - data_start;
    cluster_count = data_sectors / bpb->sectors_per_cluster;
    fat_entries = (u64)bpb->sectors_per_fat_32 * (512U / 4U);
    /* FAT32 cluster numbers reserve the top range for end-of-chain and bad
     * markers.  Keep the in-memory u32 cluster model and every later FAT
     * traversal away from that reserved range. */
    if (cluster_count < 65525ULL || cluster_count > 0x0ffffff5ULL ||
        cluster_count + 2ULL > fat_entries ||
        bpb->root_cluster < 2U || bpb->root_cluster >= cluster_count + 2ULL)
        return false;

    return true;
}

static bool fat32_probe(block_device_t *dev) {
    return dev && fat32_probe_for_id(dev, dev->id);
}

static bool fat32_label_text_valid(const char *label, usize *out_length)
{
    usize length;

    if (!label || (length = strlen(label)) > 11U) return false;
    if (length && label[length - 1U] == ' ') return false;
    if (length == 7U) {
        bool sentinel = true;
        for (usize i = 0; i < 7U; i++) {
            u8 c = (u8)label[i];
            if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
            if (c != (u8)"NO NAME"[i]) sentinel = false;
        }
        if (sentinel) return false;
    }
    for (usize index = 0; index < length; index++) {
        u8 value = (u8)label[index];
        if (value < 0x20U || value > 0x7eU || value == '"' ||
            value == '*' || value == '/' || value == ':' || value == '<' ||
            value == '>' || value == '?' || value == '\\' || value == '|' ||
            value == '+' || value == ',' || value == ';' || value == '=' ||
            value == '[' || value == ']') return false;
    }
    if (out_length) *out_length = length;
    return true;
}

bool fat32_label_valid(const char *label)
{
    return fat32_label_text_valid(label, NULL);
}

static bool fat32_label_load(block_device_t *dev, fat32_fs_t *fs,
                             fat32_bpb_t *out_bpb)
{
    u8 sector[512];
    const fat32_bpb_t *bpb;
    u64 total_sectors;
    u64 data_start;
    u64 data_sectors;

    if (!dev || !fs || !out_bpb || !fat32_probe(dev) ||
        !block_read(dev, 0U, 1U, sector)) return false;
    bpb = (const fat32_bpb_t *)sector;
    total_sectors = bpb->total_sectors_32 ? bpb->total_sectors_32 :
                    bpb->total_sectors_16;
    data_start = (u64)bpb->reserved_sector_count +
                 (u64)bpb->fat_count * bpb->sectors_per_fat_32;
    if (data_start >= total_sectors) return false;
    data_sectors = total_sectors - data_start;
    memset(fs, 0, sizeof(*fs));
    memcpy(&fs->bpb, bpb, sizeof(*bpb));
    fs->bytes_per_sector = 512U;
    fs->sectors_per_cluster = bpb->sectors_per_cluster;
    fs->cluster_size_bytes = fs->sectors_per_cluster * 512U;
    fs->reserved_sector_count = bpb->reserved_sector_count;
    fs->fat_count = bpb->fat_count;
    fs->fat_size_sectors = bpb->sectors_per_fat_32;
    fs->fs_info_sector = bpb->fs_info_sector;
    fs->backup_boot_sector = bpb->backup_boot_sector;
    fs->fat_start_lba = fs->reserved_sector_count;
    fs->data_start_lba = (u32)data_start;
    fs->root_cluster = bpb->root_cluster;
    fs->total_clusters = (u32)(data_sectors / fs->sectors_per_cluster) + 2U;
    fs->next_free_cluster = 2U;
    *out_bpb = *bpb;
    return fs->cluster_size_bytes && fs->root_cluster >= 2U &&
           fs->root_cluster < fs->total_clusters;
}

static bool fat32_label_cluster_lba(const fat32_fs_t *fs, u32 cluster,
                                    u64 *out_lba)
{
    u64 relative;
    u64 lba;

    if (!fs || !out_lba || cluster < 2U || cluster >= fs->total_clusters ||
        (u64)(cluster - 2U) > ~(u64)0 / fs->sectors_per_cluster)
        return false;
    relative = (u64)(cluster - 2U) * fs->sectors_per_cluster;
    lba = (u64)fs->data_start_lba + relative;
    if (lba > ~(u64)0 - fs->sectors_per_cluster) return false;
    *out_lba = lba;
    return true;
}

static bool fat32_label_next_cluster(block_device_t *dev,
                                     const fat32_fs_t *fs, u32 cluster,
                                     u32 *out_next)
{
    u8 sector[512];
    u64 byte_offset;
    u64 lba;
    u32 value;

    if (!dev || !fs || !out_next || cluster < 2U ||
        cluster >= fs->total_clusters ||
        (u64)cluster > ~(u64)0 / 4ULL) return false;
    byte_offset = (u64)cluster * 4ULL;
    lba = (u64)fs->fat_start_lba + byte_offset / 512ULL;
    if (lba >= dev->sector_count || !block_read(dev, lba, 1U, sector))
        return false;
    /* FAT sectors are little-endian byte streams.  Decode explicitly rather
     * than relying on an aligned native u32 load from an arbitrary entry. */
    usize offset = (usize)(byte_offset % 512ULL);
    value = (u32)sector[offset] |
            ((u32)sector[offset + 1U] << 8U) |
            ((u32)sector[offset + 2U] << 16U) |
            ((u32)sector[offset + 3U] << 24U);
    value &= 0x0fffffffU;
    if (value >= FAT32_EOC_MIN) {
        *out_next = FAT32_EOC_MIN;
        return true;
    }
    if (value < 2U || value >= fs->total_clusters) return false;
    *out_next = value;
    return true;
}

static bool fat32_label_from_entry(const fat32_on_disk_entry_t *entry,
                                   char *out, usize capacity)
{
    usize length = sizeof(entry->name);

    if (!entry || !out || capacity < 2U || entry->attr != 0x08U ||
        (u8)entry->name[0] == 0xe5U || entry->name[0] == '\0') return false;
    while (length && entry->name[length - 1U] == ' ') length--;
    if (!length || (length == 7U && !memcmp(entry->name, "NO NAME", 7U)) ||
        length >= capacity) return false;
    for (usize index = 0; index < length; index++) {
        u8 value = (u8)entry->name[index];
        if (value < 0x20U || value > 0x7eU) return false;
        out[index] = (char)value;
    }
    out[length] = '\0';
    return true;
}

/* Walk only the finite root cluster chain.  A volume-label entry is one
 * ordinary 32-byte root record, but it must not be confused with an LFN
 * record (0x0f includes the volume bit). */
static bool fat32_root_label(block_device_t *dev, const fat32_fs_t *fs,
                             char *out, usize capacity)
{
    u8 *cluster_data;
    u32 cluster;
    bool found = false;

    if (!dev || !fs || !out || capacity < 2U) return false;
    cluster_data = (u8 *)kmalloc(fs->cluster_size_bytes);
    if (!cluster_data) return false;
    cluster = fs->root_cluster;
    for (u32 step = 0; step < fs->total_clusters - 2U; step++) {
        u64 lba;
        fat32_on_disk_entry_t *entries;
        u32 count;
        u32 next;

        if (!fat32_label_cluster_lba(fs, cluster, &lba) ||
            lba > dev->sector_count ||
            fs->sectors_per_cluster > dev->sector_count - lba ||
            !block_read(dev, lba, fs->sectors_per_cluster, cluster_data))
            break;
        entries = (fat32_on_disk_entry_t *)cluster_data;
        count = fs->cluster_size_bytes / sizeof(*entries);
        for (u32 index = 0; index < count; index++) {
            if ((u8)entries[index].name[0] == 0x00U) goto done;
            if (fat32_label_from_entry(&entries[index], out, capacity)) {
                found = true;
                goto done;
            }
        }
        if (!fat32_label_next_cluster(dev, fs, cluster, &next) ||
            next >= FAT32_EOC_MIN) break;
        cluster = next;
    }
done:
    kfree(cluster_data);
    return found;
}

typedef enum {
    FAT32_LABEL_PREFLIGHT, FAT32_LABEL_DIRECTORY, FAT32_LABEL_BOOT,
    FAT32_LABEL_VERIFYING, FAT32_LABEL_VERIFIED, FAT32_LABEL_CANCELLED
} fat32_label_phase_t;
typedef struct { u8 before[2][512], proposed[2][512]; } fat32_label_fsinfo_t;
typedef struct {
    /* First member: the existing transaction disposer owns this allocation. */
    fat32_replace_transaction_t transaction;
    fat32_label_fsinfo_t *fsinfo;
    u64 boot_sector[2];
    u8 boot_before[2][512], boot_proposed[2][512];
    char requested[12];
    u8 boot_count, boot_attempted, boot_written;
    bool directory_attempted, directory_flush_attempted, directory_flushed;
    bool completion_flush_attempted, completion_flushed, cancellation_flushed;
    fat32_label_phase_t phase;
} fat32_label_transaction_t;

typedef struct {
    fat32_query_t query;
    fat32_walk_entry_t label;
    char alias[13];
    bool found;
} fat32_label_scan_t;

static int fat32_label_visit(void *context, u32 cluster,
                              const fat32_walk_entry_t *entry)
{
    fat32_label_scan_t *scan = context;
    if (entry && !entry->unused && !entry->deleted &&
        (entry->disk.attr & 0x08U)) {
        if (scan->found || entry->disk.attr != 0x08U || entry->has_lfn ||
            fat32_entry_first(&entry->disk) || entry->disk.file_size)
            return VFS_ERR_BAD_FORMAT;
        scan->label = *entry;
        scan->found = true;
        return 0;
    }
    int result = fat32_query_visit(&scan->query, cluster, entry);
    if (result || !entry || entry->unused || entry->deleted || entry->dot)
        return result;
    if (*scan->alias && fat32_match_entry(&entry->disk, &entry->lfn, scan->alias))
        scan->query.found = true;
    return VFS_OK;
}

static int fat32_label_owner_unmount(vfs_super_t *sb)
{
    kfree(sb->root_node);
    sb->root_node = NULL;
    return fat32_unmount(sb);
}
static const vfs_super_ops_t fat32_label_owner_ops = {
    .unmount = fat32_label_owner_unmount,
};
static int fat32_mount(vfs_fs_type_t *, block_device_t *, vfs_super_t **, bool);
static vfs_fs_type_t fat32_fs_type;

static bool fat32_label_finish(vfs_super_t *sb, void *context)
{
    fat32_label_transaction_t *label = context;
    fat32_replace_transaction_t *tx = &label->transaction;
    fat32_fs_t *fs = sb->private_data;
    if (!tx->handoff_pending || (fs && fs->replace_transaction && fs->replace_transaction != tx)) return false;
    if (label->phase == FAT32_LABEL_VERIFIED) {
        if (!fs || fs->replace_transaction != tx || tx->phase != FAT32_REPLACE_HANDOFF_PUBLISHED ||
            tx->metadata_write_pending || tx->fsinfo_cursor != tx->fsinfo_count) return false;
        if (!label->boot_count || label->boot_count > 2U) return false;
        u8 complete = (u8)((1U << label->boot_count) - 1U);
        if (!label->completion_flushed ||
            label->boot_attempted != complete || label->boot_written != complete ||
            (label->directory_attempted && !label->directory_flushed)) return false;
    } else if (label->phase != FAT32_LABEL_CANCELLED || tx->phase != FAT32_REPLACE_HANDOFF_CANCELLED ||
               label->directory_attempted || label->boot_attempted || (tx->growth &&
               (tx->growth->link_attempted || tx->growth->candidate_allocated ||
                (tx->growth->candidate_recorded && !label->cancellation_flushed)))) return false;
    kfree(label->fsinfo); label->fsinfo = NULL;
    if (fs && fs->replace_transaction == tx)
        return fat32_replace_finish_handoff(sb, tx);
    fat32_replace_free(tx); /* Preflight: nothing was attached or written. */
    return true;
}

static int fat32_label_verify(vfs_super_t *sb, fat32_label_transaction_t *label)
{
    fat32_fs_t *fs = sb->private_data;
    fat32_label_scan_t scan = {.query = {.mode = FAT32_QUERY_CREATE, .name = ""}};
    int result = fat32_dir_walk_limit(sb, fs->root_cluster, true, true, fat32_label_visit,
        &scan, NULL, FAT32_WALK_MAX_DIR_CLUSTERS, label->transaction.data_buffer);
    if (result != VFS_OK) return result;
    if (*label->requested) {
        char observed[12];
        if (!scan.found || !fat32_label_from_entry(&scan.label.disk, observed, sizeof(observed)) ||
            strcmp(observed, label->requested)) return VFS_ERR_IO;
    } else if (scan.found) return VFS_ERR_IO;
    for (u32 i = 0; i < label->boot_count; i++) {
        u8 observed[512];
        if (!fat32_mounted_read(sb, label->boot_sector[i], 1U, observed) ||
            memcmp(observed, label->boot_proposed[i], 512U)) return VFS_ERR_IO;
    }
    return VFS_OK;
}

static int fat32_set_label_guarded(block_device_t *dev, u64 expected_id,
                                    const char *requested)
{
    block_device_t snapshot;
    if (!block_device_snapshot(dev, &snapshot) || snapshot.id != expected_id)
        return VFS_ERR_DEVICE_GONE;
    int admission = storage_management_range_preflight(dev, expected_id, 0,
                                                       snapshot.sector_count);
    if (admission != VFS_OK) return admission;
    usize length = strlen(requested);
    fat32_label_transaction_t *label = kmalloc(sizeof(*label));
    if (!label) return VFS_ERR_NO_MEM;
    memset(label, 0, sizeof(*label));
    strcpy(label->requested, requested);
    fat32_replace_transaction_t *tx = &label->transaction;
    vfs_super_t *sb, *loaded;
    u64 pin;
    int result = vfs_recovery_private_begin(dev, expected_id, &fat32_label_owner_ops,
        fat32_label_finish, label, &sb, &pin);
    if (result != VFS_OK) { fat32_replace_free(tx); return result; }
    tx->pin_id = pin;
    if (!mutex_lock(&sb->operation_lock)) { result = VFS_ERR_IO; goto finish; }
    result = fat32_mount(&fat32_fs_type, dev, &loaded, false);
    if (result != VFS_OK) goto cancel_preflight;
    sb->private_data = loaded->private_data;
    sb->root_node = loaded->root_node;
    sb->root_node->super = sb;
    kfree(loaded);
    fat32_fs_t *fs = sb->private_data;
    if (fs->device_id != expected_id || !block_device_id_is_live(expected_id)) {
        result = VFS_ERR_DEVICE_GONE; goto cancel_preflight;
    }
    result = fat32_ownership_preflight_locked(sb->root_node);
    if (result != VFS_OK) goto cancel_preflight;
    fat32_label_scan_t scan = {.query = {.mode = FAT32_QUERY_CREATE, .name = requested}};
    fat32_on_disk_entry_t proposed = {.attr = 0x08U};
    memset(proposed.name, ' ', 11U); memcpy(proposed.name, requested, length);
    if (length) (void)fat32_format_83_name(&proposed, scan.alias);
    result = fat32_dir_walk(sb, fs->root_cluster, true, true, fat32_label_visit, &scan);
    if (result != VFS_OK) goto cancel_preflight;
    if (length && scan.query.found) { result = VFS_ERR_ALREADY_EXISTS; goto cancel_preflight; }
    fat32_walk_entry_t slot = scan.found ? scan.label : scan.query.free_slot;
    bool directory_update = scan.found || length;
    if (directory_update && !scan.found) {
        bool cross_sector;
        result = fat32_namespace_placement_prepare(sb->root_node, tx, &slot, false,
            scan.query.free_found, NULL, 0U, 0U, proposed.name, &cross_sector);
        if (result != VFS_OK) goto cancel_preflight;
    } else if (directory_update) {
        tx->directory_sector = fat32_cluster_to_lba(fs, slot.cluster) + slot.slot / 16U;
        if (!fat32_mounted_read(sb, tx->directory_sector, 1U, tx->old_sector)) {
            result = VFS_ERR_IO; goto cancel_preflight;
        }
        memcpy(tx->proposed_sector, tx->old_sector, 512U);
    }
    if (directory_update) {
        fat32_on_disk_entry_t *entry = (void *)(tx->proposed_sector + slot.slot % 16U * 32U);
        if (length) {
            if (!scan.found) memset(entry, 0, sizeof(*entry));
            memcpy(entry->name, proposed.name, 11U); entry->attr = 0x08U;
        } else entry->name[0] = (char)0xe5U;
    }
    label->boot_count = 1U;
    if (fs->backup_boot_sector && fs->backup_boot_sector != 0xffffU) {
        if (fs->backup_boot_sector >= fs->reserved_sector_count) {
            result = VFS_ERR_BAD_FORMAT; goto cancel_preflight;
        }
        label->boot_sector[label->boot_count++] = fs->backup_boot_sector;
    }
    for (u32 i = 0; i < label->boot_count; i++) {
        if (!fat32_mounted_read(sb, label->boot_sector[i], 1U, label->boot_before[i])) {
            result = VFS_ERR_IO; goto cancel_preflight;
        }
        if (label->boot_before[i][510] != 0x55U || label->boot_before[i][511] != 0xaaU ||
            (i && memcmp(label->boot_before[0] + 11U, label->boot_before[i] + 11U, 60U))) {
            result = VFS_ERR_BAD_FORMAT; goto cancel_preflight;
        }
        memcpy(label->boot_proposed[i], label->boot_before[i], 512U);
        memcpy(label->boot_proposed[i] + 71U, length ? proposed.name : "NO NAME    ", 11U);
    }
    if (tx->growth && tx->growth->append) {
        result = fat32_replace_prepare_fsinfo(sb, fs, tx);
        if (result != VFS_OK) goto cancel_preflight;
        if (tx->fsinfo_count) {
            label->fsinfo = kmalloc(sizeof(*label->fsinfo));
            if (!label->fsinfo) { result = VFS_ERR_NO_MEM; goto cancel_preflight; }
            for (u32 i = 0; i < tx->fsinfo_count; i++) {
                if (!fat32_mounted_read(sb, tx->fsinfo_sectors[i], 1U, label->fsinfo->before[i])) {
                    result = VFS_ERR_IO; goto cancel_preflight;
                }
                memcpy(label->fsinfo->proposed[i], label->fsinfo->before[i], 512U);
                fat32_store_le32(label->fsinfo->proposed[i] + 488U, 0xffffffffU);
                fat32_store_le32(label->fsinfo->proposed[i] + 492U, 0xffffffffU);
            }
        }
    }
    if (!tx->data_buffer) {
        tx->data_buffer = kmalloc(fs->cluster_size_bytes);
        if (!tx->data_buffer) { result = VFS_ERR_NO_MEM; goto cancel_preflight; }
    }
    fs->replace_transaction = tx; fs->version_confident = false; fs->namespace_hidden = true;
    result = fat32_create_stage_directory(sb, fs, tx, &slot.cluster);
    if (result == VFS_ERR_UNCERTAIN || result == VFS_ERR_BAD_FORMAT) goto uncertain;
    if (result != VFS_OK) goto cancel;
    label->phase = FAT32_LABEL_DIRECTORY;
    if (directory_update) {
        label->directory_attempted = true;
        tx->phase = FAT32_REPLACE_PUBLISHING;
        if (!fat32_mounted_write(sb, tx->directory_sector, 1U, tx->proposed_sector)) goto uncertain;
        label->directory_flush_attempted = true;
        if (!block_flush_for_id(dev, expected_id)) goto uncertain;
        if (tx->growth) {
            tx->growth->entry_written = true;
            if (tx->growth->has_marker) {
                tx->growth->marker_attempted = true;
                if (!fat32_mounted_write(sb, tx->growth->marker_sector, 1U,
                    tx->growth->marker_proposed) || !block_flush_for_id(dev, expected_id)) goto uncertain;
            }
        }
        label->directory_flushed = true;
    }
    if (tx->fsinfo_count && fat32_replace_fsinfo(sb, tx) != VFS_OK) goto uncertain;
    label->phase = FAT32_LABEL_BOOT;
    for (u32 i = 0; i < label->boot_count; i++) {
        label->boot_attempted |= 1U << i;
        if (!fat32_mounted_write(sb, label->boot_sector[i], 1U, label->boot_proposed[i])) goto uncertain;
        label->boot_written |= 1U << i;
    }
    label->completion_flush_attempted = true;
    if (!block_flush_for_id(dev, expected_id)) goto uncertain;
    label->completion_flushed = true; label->phase = FAT32_LABEL_VERIFYING;
    if (fat32_label_verify(sb, label) != VFS_OK) goto uncertain;
    label->phase = FAT32_LABEL_VERIFIED; tx->phase = FAT32_REPLACE_HANDOFF_PUBLISHED;
    result = VFS_OK;
    goto unlock;
cancel:
    if (fat32_create_cancel_directory(sb, fs, tx) != VFS_OK ||
        (tx->growth && tx->growth->candidate_recorded &&
         (fat32_replace_fsinfo(sb, tx) != VFS_OK || !block_flush_for_id(dev, expected_id)))) goto uncertain;
    label->cancellation_flushed = true;
cancel_preflight:
    if (!block_device_id_is_live(expected_id)) result = VFS_ERR_DEVICE_GONE;
    label->phase = FAT32_LABEL_CANCELLED; tx->phase = FAT32_REPLACE_HANDOFF_CANCELLED;
unlock:
    tx->handoff_pending = true;
    (void)mutex_unlock(&sb->operation_lock);
finish:
    if (!tx->handoff_pending) {
        label->phase = FAT32_LABEL_CANCELLED; tx->phase = FAT32_REPLACE_HANDOFF_CANCELLED;
        tx->handoff_pending = true;
    }
    kfree(tx->data_buffer); tx->data_buffer = NULL;
    if (vfs_recovery_pin_finish_handoff(pin) != VFS_OK) {
        (void)vfs_recovery_pin_mark_unresolved(pin); result = VFS_ERR_UNCERTAIN;
    }
    vfs_recovery_private_release(sb);
    return result;
uncertain:
    kfree(tx->data_buffer); tx->data_buffer = NULL;
    (void)vfs_recovery_pin_mark_unresolved(pin);
    (void)mutex_unlock(&sb->operation_lock);
    vfs_recovery_private_release(sb);
    return VFS_ERR_UNCERTAIN;
}

int fat32_set_label_for_id(block_device_t *dev, u64 expected_id, const char *label)
{
    if (!dev || !fat32_label_text_valid(label, NULL)) return VFS_ERR_INVALID_PARAM;
    if (!storage_mutation_begin()) return VFS_ERR_BUSY;
    int result = fat32_set_label_guarded(dev, expected_id, label);
    storage_mutation_end();
    return result;
}

int fat32_set_label(block_device_t *dev, const char *label)
{
    return fat32_set_label_for_id(dev, dev ? dev->id : 0U, label);
}

bool fat32_read_label(block_device_t *dev, char *out, usize capacity)
{
    u8 sector_buf[512];
    const fat32_bpb_t *bpb;
    fat32_fs_t fs;
    fat32_bpb_t loaded_bpb;
    usize length = sizeof(((fat32_bpb_t *)0)->volume_label);

    if (!out || capacity < 2U) return false;
    out[0] = '\0';
    if (!dev || !fat32_label_load(dev, &fs, &loaded_bpb) ||
        !block_read(dev, 0, 1, sector_buf))
        return false;
    if (fat32_root_label(dev, &fs, out, capacity)) return true;
    bpb = (const fat32_bpb_t *)sector_buf;
    while (length && bpb->volume_label[length - 1U] == ' ') length--;
    if (!length) return false;
    /* FAT formatters conventionally store "NO NAME" when the volume has no
     * label.  Treat that sentinel as unlabeled so automount policy falls back
     * to the generation-safe disk/partition display name. */
    if (length == 7U && !memcmp(bpb->volume_label, "NO NAME", 7U))
        return false;
    if (length >= capacity) length = capacity - 1U;
    for (usize index = 0; index < length; index++) {
        unsigned char value = (unsigned char)bpb->volume_label[index];
        if (value < 0x20U || value > 0x7eU) {
            out[0] = '\0';
            return false;
        }
        out[index] = (char)value;
    }
    out[length] = '\0';
    return true;
}

static int fat32_mount(vfs_fs_type_t *fs_type, block_device_t *dev,
                        vfs_super_t **out_sb, bool read_only) {
    if (!fs_type || !dev || !out_sb) {
        return VFS_ERR_INVALID_PARAM;
    }
    u64 device_id = dev->id;

    if (!fat32_probe_for_id(dev, device_id)) {
        return VFS_ERR_BAD_FORMAT;
    }

    u8 sector_buf[512];
    if (!block_read_for_id(dev, device_id, 0, 1, sector_buf)) {
        return VFS_ERR_IO;
    }

    fat32_bpb_t *bpb = (fat32_bpb_t *)sector_buf;

    vfs_super_t *sb = (vfs_super_t *)kmalloc(sizeof(vfs_super_t));
    fat32_fs_t *fs = (fat32_fs_t *)kmalloc(sizeof(fat32_fs_t));
    vfs_node_t *root_node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    fat32_node_state_t *root_state =
        (fat32_node_state_t *)kmalloc(sizeof(*root_state));

    if (!sb || !fs || !root_node || !root_state) {
        kfree(sb);
        kfree(fs);
        kfree(root_node);
        kfree(root_state);
        return VFS_ERR_NO_MEM;
    }

    memset(sb, 0, sizeof(*sb));
    sb->read_only = read_only;
    memset(fs, 0, sizeof(*fs));
    memset(root_node, 0, sizeof(*root_node));
    memset(root_state, 0, sizeof(*root_state));
    fs->device_id = device_id;
    memcpy(&fs->bpb, bpb, sizeof(fat32_bpb_t));

    fs->bytes_per_sector = bpb->bytes_per_sector;
    fs->sectors_per_cluster = bpb->sectors_per_cluster;
    fs->cluster_size_bytes = fs->bytes_per_sector * fs->sectors_per_cluster;
    fs->reserved_sector_count = bpb->reserved_sector_count;
    fs->fat_count = bpb->fat_count;
    fs->fat_size_sectors = bpb->sectors_per_fat_32;
    fs->fs_info_sector = bpb->fs_info_sector;
    fs->backup_boot_sector = bpb->backup_boot_sector;

    fs->fat_start_lba = fs->reserved_sector_count;
    fs->data_start_lba = fs->fat_start_lba + (fs->fat_count * fs->fat_size_sectors);
    fs->root_cluster = bpb->root_cluster;

    u32 total_sectors = bpb->total_sectors_32 ? bpb->total_sectors_32 : bpb->total_sectors_16;
    u32 data_sectors = (total_sectors > fs->data_start_lba) ? (total_sectors - fs->data_start_lba) : 0;
    fs->total_clusters = (fs->sectors_per_cluster > 0) ? ((data_sectors / fs->sectors_per_cluster) + 2) : 0;
    fs->next_free_cluster = 2;
    fs->version_confident = true;
    fs->next_inode = 1;
    fs->nodes = root_state;
    root_state->node = root_node;
    root_state->first_cluster = fs->root_cluster;
    root_state->generation = 1;
    root_state->alive = true;
    root_state->version_valid = true;

    root_node->inode = 1;
    root_node->type = VFS_TYPE_DIRECTORY;
    root_node->size = 0;
    vfs_node_set_security(root_node, VFS_UID_SYSTEM,
                          FAT32_VOLUME_PERMISSIONS);
    root_node->child_mutation_policy = VFS_CHILD_MUTATION_OPEN;
    root_node->ref_count = 1;
    root_node->super = sb;
    root_node->fs_data = root_state;
    root_node->ops = &fat32_node_ops;

    sb->fs_type = fs_type;
    sb->dev = dev;
    sb->root_node = root_node;
    sb->private_data = fs;
    sb->ops = &fat32_super_ops;

    *out_sb = sb;
    return VFS_OK;
}

static vfs_fs_type_t fat32_fs_type = {
    .name = "fat32",
    .probe = fat32_probe,
    .label = fat32_read_label,
    .mount = fat32_mount,
    .next = NULL,
};

int fat32_init(void) {
    return vfs_register_fs(&fat32_fs_type);
}
