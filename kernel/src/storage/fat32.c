/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <storage/fat32.h>
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
    if (!sb || !sb->private_data) return false;
    fat32_fs_t *fs = (fat32_fs_t *)sb->private_data;
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
static bool fat32_set_next_cluster(vfs_super_t *sb, fat32_fs_t *fs,
                                    u32 cluster, u32 val);

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

typedef struct {
    vfs_node_t *node;
    fat32_node_state_t *state;
} fat32_prepared_node_t;

static bool fat32_prepare_node(fat32_fs_t *fs, fat32_prepared_node_t *prepared)
{
    if (!fs || !prepared || fs->next_inode == ~(u64)0) return false;
    prepared->node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    prepared->state =
        (fat32_node_state_t *)kmalloc(sizeof(fat32_node_state_t));
    if (prepared->node && prepared->state) return true;
    kfree(prepared->node);
    kfree(prepared->state);
    prepared->node = NULL;
    prepared->state = NULL;
    return false;
}

static void fat32_discard_prepared(fat32_prepared_node_t *prepared)
{
    if (!prepared) return;
    kfree(prepared->node);
    kfree(prepared->state);
    prepared->node = NULL;
    prepared->state = NULL;
}

static int fat32_mutation_error(const fat32_fs_t *fs, int definite_error)
{
    return fs && !fs->version_confident
        ? VFS_ERR_UNCERTAIN : definite_error;
}

static void fat32_release_unpublished(vfs_super_t *sb, fat32_fs_t *fs,
                                       u32 cluster)
{
    /* A failed directory/FAT write may have published this allocation.
     * Keep it allocated until offline repair when media outcome is unknown. */
    if (fs && fs->version_confident)
        (void)fat32_set_next_cluster(sb, fs, cluster, 0U);
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
    if (query->mode == FAT32_QUERY_INDEX) {
        if (!fat32_entry_readable(entry)) return 0;
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

static bool fat32_readdir(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry)
{
    if (!dir || !out_entry || !dir->super || !dir->super->private_data)
        return false;
    if (((fat32_fs_t *)dir->super->private_data)->namespace_hidden)
        return false;
    fat32_query_t query = {.mode = FAT32_QUERY_INDEX, .wanted_index = index};
    if (fat32_dir_query(dir, &query) != VFS_OK || !query.found) return false;
    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    fat32_walk_entry_t *item = &query.match;
    if (!fat32_lfn_encode(&item->lfn, &item->disk, out_entry->name,
                           sizeof(out_entry->name)) &&
        !fat32_format_83_name(&item->disk, out_entry->name)) return false;
    u32 first = ((u32)item->disk.first_cluster_high << 16U) |
                item->disk.first_cluster_low;
    vfs_node_t *member = fat32_bind_node(
        dir->super, fs, item->cluster, item->slot, first,
        item->disk.file_size,
        (item->disk.attr & 0x10U) ? VFS_TYPE_DIRECTORY : VFS_TYPE_FILE);
    if (!member) return false;
    out_entry->inode = member->inode;
    out_entry->type = member->type;
    return true;
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

/* A complete chain scan distinguishes a missing name from read/allocation
 * failure and locates a physical slot without assuming it is in cluster 0. */
static int fat32_check_name_available(vfs_node_t *dir, const char *name,
                                        fat32_walk_entry_t *out_free)
{
    fat32_query_t query = {.mode = FAT32_QUERY_CREATE, .name = name};
    int result = fat32_dir_query(dir, &query);
    if (result != VFS_OK) return result;
    if (query.found) return VFS_ERR_INVALID_PARAM;
    if (!query.free_found) return VFS_ERR_NO_SPACE;
    if (out_free) *out_free = query.free_slot;
    return VFS_OK;
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

static bool fat32_set_next_cluster(vfs_super_t *sb, fat32_fs_t *fs, u32 cluster, u32 val) {
    if (!sb || !fs || cluster < 2) return false;

    u32 fat_offset = cluster * 4;
    u32 active = (fs->bpb.ext_flags & 0x80U)
        ? fs->bpb.ext_flags & 0x0fU : 0U;
    if (active >= fs->fat_count) return false;
    u32 fat_sector = fs->fat_start_lba +
        active * fs->fat_size_sectors +
        (fat_offset / fs->bytes_per_sector);
    u32 entry_offset = fat_offset % fs->bytes_per_sector;

    u8 *sector_buf = (u8 *)kmalloc(fs->bytes_per_sector);
    if (!sector_buf) return false;

    if (!fat32_mounted_read(sb, fat_sector, 1, sector_buf)) {
        kfree(sector_buf);
        return false;
    }

    u32 *entry_ptr = (u32 *)(sector_buf + entry_offset);
    *entry_ptr = (*entry_ptr & 0xF0000000) | (val & 0x0FFFFFFF);

    u32 copies = (fs->bpb.ext_flags & 0x80U) ? 1U : fs->fat_count;
    for (u32 fat_idx = 0; fat_idx < copies; fat_idx++) {
        u32 target_sector = fat_sector + (fat_idx * fs->fat_size_sectors);
        if (!fat32_mounted_write(sb, target_sector, 1, sector_buf)) {
            kfree(sector_buf);
            return false;
        }
    }

    kfree(sector_buf);
    return true;
}

static bool fat32_zero_cluster(vfs_super_t *sb, fat32_fs_t *fs, u32 cluster) {
    if (!sb || !fs || cluster < 2) return false;

    u32 cluster_size = fs->cluster_size_bytes;
    u8 *zero_buf = (u8 *)kmalloc(cluster_size);
    if (!zero_buf) return false;

    memset(zero_buf, 0, cluster_size);
    u64 lba = fat32_cluster_to_lba(fs, cluster);

    bool ok = fat32_mounted_write(sb, lba, fs->sectors_per_cluster, zero_buf);
    kfree(zero_buf);
    return ok;
}

/* FSInfo free-count/next-free values are advisory.  Keeping an exact count
 * would require a full FAT scan after imported-media changes, so invalidate
 * both hints after any allocation mutation.  0xffffffff is the FAT32-defined
 * "unknown" value and avoids publishing stale values to host fsck tools. */
static void fat32_invalidate_fsinfo(vfs_super_t *sb, fat32_fs_t *fs) {
    u8 sector[512];
    u64 copies[2];
    u32 count = 0;

    if (!sb || !fs || fs->fs_info_sector == 0U ||
        fs->fs_info_sector >= fs->reserved_sector_count) return;
    copies[count++] = fs->fs_info_sector;
    if (fs->backup_boot_sector &&
        fs->backup_boot_sector <= 0xffffffffU - fs->fs_info_sector &&
        fs->backup_boot_sector + fs->fs_info_sector < fs->reserved_sector_count)
        copies[count++] = fs->backup_boot_sector + fs->fs_info_sector;
    for (u32 index = 0; index < count; index++) {
        if (!fat32_mounted_read(sb, copies[index], 1U, sector)) continue;
        fat32_store_le32(sector + 488U, 0xffffffffU);
        fat32_store_le32(sector + 492U, 0xffffffffU);
        (void)fat32_mounted_write(sb, copies[index], 1U, sector);
    }
}

u32 fat32_alloc_cluster(vfs_super_t *sb) {
    if (!sb || !sb->private_data) return FAT32_CLUSTER_ERR;
    fat32_fs_t *fs = (fat32_fs_t *)sb->private_data;
    if (!fs->version_confident) return FAT32_CLUSTER_ERR;

    u32 start = fs->next_free_cluster;
    if (start < 2 || start >= fs->total_clusters) {
        start = 2;
    }

    u32 c = start;
    do {
        u32 raw_val = fat32_get_next_cluster(sb, fs, c);
        if (raw_val == FAT32_CLUSTER_ERR) return FAT32_CLUSTER_ERR;
        if (raw_val == 0x00000000) {
            if (fat32_set_next_cluster(sb, fs, c, 0x0FFFFFFF)) {
                if (!fat32_zero_cluster(sb, fs, c)) {
                    fs->version_confident = false;
                    return FAT32_CLUSTER_ERR;
                }
                fs->next_free_cluster = c + 1;
                if (fs->next_free_cluster >= fs->total_clusters) {
                    fs->next_free_cluster = 2;
                }
                fat32_invalidate_fsinfo(sb, fs);
                return fs->version_confident ? c : FAT32_CLUSTER_ERR;
            }
            if (!fs->version_confident) return FAT32_CLUSTER_ERR;
        }
        c++;
        if (c >= fs->total_clusters) {
            c = 2;
        }
    } while (c != start);

    return FAT32_CLUSTER_ERR;
}

u32 fat32_extend_chain(vfs_super_t *sb, u32 last_cluster) {
    if (!sb || !sb->private_data) return FAT32_CLUSTER_ERR;
    fat32_fs_t *fs = (fat32_fs_t *)sb->private_data;

    u32 new_cluster = fat32_alloc_cluster(sb);
    if (new_cluster == FAT32_CLUSTER_ERR) {
        return FAT32_CLUSTER_ERR;
    }

    if (last_cluster >= 2) {
        if (!fat32_set_next_cluster(sb, fs, last_cluster, new_cluster)) {
            fat32_release_unpublished(sb, fs, new_cluster);
            return FAT32_CLUSTER_ERR;
        }
    }

    return new_cluster;
}

bool fat32_free_chain(vfs_super_t *sb, u32 start_cluster) {
    if (!sb || !sb->private_data || start_cluster < 2) return false;
    fat32_fs_t *fs = (fat32_fs_t *)sb->private_data;
    if (!fs->version_confident || fs->replace_transaction) return false;

    u32 curr = start_cluster;
    u32 remaining = fs->total_clusters;
    while (curr >= 2 && curr < FAT32_EOC_MIN && remaining--) {
        if (curr >= fs->total_clusters) {
            fs->version_confident = false;
            return false;
        }
        u32 next = fat32_get_next_cluster(sb, fs, curr);
        if (next == FAT32_CLUSTER_ERR ||
            !fat32_set_next_cluster(sb, fs, curr, 0x00000000)) {
            fs->version_confident = false;
            return false;
        }
        curr = next;
    }
    if (curr < FAT32_EOC_MIN) {
        fs->version_confident = false;
        return false;
    }

    fat32_invalidate_fsinfo(sb, fs);

    return fs->version_confident;
}

u32 fat32_get_cluster_link(vfs_super_t *sb, u32 cluster) {
    if (!sb || !sb->private_data) return FAT32_CLUSTER_ERR;
    fat32_fs_t *fs = (fat32_fs_t *)sb->private_data;
    return fat32_get_next_cluster(sb, fs, cluster);
}

static u64 fat32_read(vfs_node_t *node, u64 offset, u64 size, void *buffer) {
    if (!node || !buffer || node->type != VFS_TYPE_FILE || !node->super || !node->super->private_data) {
        return 0;
    }
    if (fat32_node_cluster(node) == FAT32_CLUSTER_ERR) return 0;

    if (offset >= node->size) {
        return 0;
    }

    u64 avail = node->size - offset;
    if (size > avail) {
        size = avail;
    }

    fat32_fs_t *fs = (fat32_fs_t *)node->super->private_data;
    u32 cluster_size = fs->cluster_size_bytes;

    u32 curr_cluster = fat32_node_cluster(node);
    u64 skip_clusters = offset / cluster_size;
    u64 offset_in_cluster = offset % cluster_size;

    /* Advance to starting cluster for requested offset */
    for (u64 i = 0; i < skip_clusters; i++) {
        curr_cluster = fat32_get_next_cluster(node->super, fs, curr_cluster);
        if (curr_cluster < 2U || curr_cluster >= fs->total_clusters ||
            curr_cluster >= FAT32_EOC_MIN) {
            return 0;
        }
    }

    u8 *cluster_buf = (u8 *)kmalloc(cluster_size);
    if (!cluster_buf) {
        return 0;
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
    return bytes_read;
}

/* The containing short-entry slot is the mount-local location key. Updating
 * by first cluster can select a different empty file (cluster zero), or a
 * different file on a corrupt volume with overlapping chains. */
static bool fat32_update_dirent_size(vfs_node_t *node, u32 size)
{
    fat32_fs_t *fs;
    fat32_node_state_t *state = fat32_state(node);
    fat32_on_disk_entry_t *entry;
    u8 *cluster_buf;
    bool result;

    if (!node || !node->super || !node->super->private_data || !state ||
        !state->alive || node->type != VFS_TYPE_FILE) return false;
    fs = (fat32_fs_t *)node->super->private_data;
    if (state->parent_cluster < 2U ||
        state->parent_cluster >= fs->total_clusters ||
        state->entry_index >= fs->cluster_size_bytes / sizeof(*entry))
        return false;
    cluster_buf = (u8 *)kmalloc(fs->cluster_size_bytes);
    if (!cluster_buf) return false;
    if (!fat32_mounted_read(node->super,
                            fat32_cluster_to_lba(fs, state->parent_cluster),
                            fs->sectors_per_cluster, cluster_buf)) {
        kfree(cluster_buf);
        return false;
    }
    entry = &((fat32_on_disk_entry_t *)cluster_buf)[state->entry_index];
    if ((u8)entry->name[0] == 0U || (u8)entry->name[0] == 0xE5U ||
        (entry->attr & 0x18U) != 0U ||
        (((u32)entry->first_cluster_high << 16U) |
         entry->first_cluster_low) != state->first_cluster) {
        kfree(cluster_buf);
        state->version_valid = false;
        return false;
    }
    entry->file_size = size;
    result = fat32_mounted_write(node->super,
                                  fat32_cluster_to_lba(fs,
                                                       state->parent_cluster),
                                  fs->sectors_per_cluster, cluster_buf);
    kfree(cluster_buf);
    return result;
}

static u64 fat32_write(vfs_node_t *node, u64 offset, u64 size, const void *buffer) {
    fat32_node_state_t *state = fat32_state(node);
    if (!node || !buffer || node->type != VFS_TYPE_FILE || !node->super || !node->super->private_data) {
        return 0;
    }
    if (!state || !state->alive || !state->version_valid ||
        state->generation == ~(u64)0) return 0;

    fat32_fs_t *fs = (fat32_fs_t *)node->super->private_data;
    u32 cluster_size = fs->cluster_size_bytes;

    /* Count allocated clusters in existing cluster chain */
    u32 chain_clusters = 0;
    u32 c = fat32_node_cluster(node);
    while (c >= 2U && c < FAT32_EOC_MIN &&
           c < fs->total_clusters && chain_clusters < fs->total_clusters) {
        chain_clusters++;
        c = fat32_get_next_cluster(node->super, fs, c);
    }
    if (c != FAT32_EOC_MIN || !chain_clusters) return 0;

    u64 max_capacity = (u64)chain_clusters * cluster_size;
    if (offset >= max_capacity) {
        return 0; // Exceeds allocated cluster chain capacity
    }

    u64 avail_capacity = max_capacity - offset;
    if (size > avail_capacity) {
        size = avail_capacity;
    }
    if (offset > 0xffffffffULL || size > 0xffffffffULL - offset)
        return 0;

    if (size == 0) {
        return 0;
    }

    u32 curr_cluster = fat32_node_cluster(node);
    u64 skip_clusters = offset / cluster_size;
    u64 offset_in_cluster = offset % cluster_size;

    /* Advance to starting cluster for requested offset */
    for (u64 i = 0; i < skip_clusters; i++) {
        curr_cluster = fat32_get_next_cluster(node->super, fs, curr_cluster);
        if (curr_cluster < 2U || curr_cluster >= fs->total_clusters ||
            curr_cluster >= FAT32_EOC_MIN) {
            return 0;
        }
    }

    u8 *cluster_buf = (u8 *)kmalloc(cluster_size);
    if (!cluster_buf) {
        return 0;
    }

    u64 bytes_written = 0;
    u64 bytes_remaining = size;
    const u8 *in_ptr = (const u8 *)buffer;
    bool failed = false;

    while (bytes_remaining > 0 && curr_cluster >= 2U &&
           curr_cluster < fs->total_clusters &&
           curr_cluster < FAT32_EOC_MIN) {
        u64 lba = fat32_cluster_to_lba(fs, curr_cluster);

        /* Read existing cluster for partial block updates */
        if (!fat32_mounted_read(node->super, lba, fs->sectors_per_cluster, cluster_buf)) {
            failed = true;
            break;
        }

        u64 chunk_avail = cluster_size - offset_in_cluster;
        u64 chunk_size = (bytes_remaining < chunk_avail) ? bytes_remaining : chunk_avail;

        memcpy(cluster_buf + offset_in_cluster, in_ptr, chunk_size);

        if (!fat32_mounted_write(node->super, lba, fs->sectors_per_cluster, cluster_buf)) {
            failed = true;
            break;
        }

        bytes_written += chunk_size;
        bytes_remaining -= chunk_size;
        in_ptr += chunk_size;

        offset_in_cluster = 0;

        if (bytes_remaining > 0) {
            curr_cluster = fat32_get_next_cluster(node->super, fs, curr_cluster);
        }
    }

    kfree(cluster_buf);

    /* Expand in-memory node size and update on-disk directory entry if file grew */
    if (offset + bytes_written > node->size) {
        u32 new_size = (u32)(offset + bytes_written);
        if (fat32_update_dirent_size(node, new_size))
            vfs_nodes_update_size(node->super, node->inode, new_size);
        else failed = true;
    }
    if (failed) state->version_valid = false;
    else if (bytes_written) fat32_version_advance(state);

    return bytes_written;
}

/* A truncate keeps the file's currently allocated chain.  This is safe for
 * the existing single-chain FAT32 writer: file size gates all later reads,
 * and a subsequent overwrite reuses the same allocation instead of exposing
 * old content or allocating unboundedly. */
static int fat32_truncate(vfs_node_t *node) {
    fat32_node_state_t *state = fat32_state(node);
    if (!node || node->type != VFS_TYPE_FILE || !node->super ||
        !node->super->private_data) return VFS_ERR_INVALID_PARAM;
    fat32_fs_t *fs = (fat32_fs_t *)node->super->private_data;
    if (!fs->version_confident || fs->replace_transaction)
        return VFS_ERR_UNCERTAIN;
    if (!state || !state->alive || !state->version_valid)
        return VFS_ERR_UNCERTAIN;
    if (node->size == 0U) return VFS_OK;
    if (state->generation == ~(u64)0) return VFS_ERR_NO_SPACE;
    if (!fat32_update_dirent_size(node, 0U)) {
        state->version_valid = false;
        return VFS_ERR_UNCERTAIN;
    }
    vfs_nodes_update_size(node->super, node->inode, 0U);
    fat32_version_advance(state);
    return VFS_OK;
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
    FAT32_REPLACE_CANCELLING,
    FAT32_REPLACE_DIR_ALLOCATING,
    FAT32_REPLACE_DIR_INITIALIZING,
    FAT32_REPLACE_DIR_LINKING,
    FAT32_REPLACE_DIR_FLUSHING,
    FAT32_REPLACE_PUBLISHING,
    FAT32_REPLACE_DIR_RELOCATING,
    FAT32_REPLACE_UNLINK_PUBLISHING,
    FAT32_REPLACE_UNLINK_CLEANUP,
    FAT32_REPLACE_RECLAIMING,
    FAT32_REPLACE_FSINFO,
    FAT32_REPLACE_HANDOFF_PUBLISHED,
    FAT32_REPLACE_HANDOFF_CANCELLED,
} fat32_replace_phase_t;

typedef struct {
    u64 sector[2][4];
    u8 before[2][4][512], proposed[2][4][512];
} fat32_growth_second_images_t;

/* Only safe create uses this inventory. The second pair of FAT images is
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

/* All inventory and both publication images survive an uncertain outcome.
 * The pin retains the superblock and this mount-owned object after removal. */
typedef struct fat32_replace_transaction {
    u32 *old_clusters;
    u32 *fresh_clusters;
    u8 *data_buffer;
    fat32_directory_growth_t *growth;
    fat32_lfn_retirement_t *retirement;
    fat32_lfn_creation_t *lfn_creation;
    u32 old_count, fresh_capacity, fresh_count;
    u32 cancellation_cursor, reclamation_cursor, fsinfo_cursor;
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
    fs->replace_transaction = NULL;
    fs->version_confident = true;
    fs->namespace_hidden = false;
    fat32_replace_free(tx);
    return true;
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
    fat32_on_disk_entry_t *new_entry =
        &((fat32_on_disk_entry_t *)tx->proposed_sector)[entry_in_sector];
    new_entry->first_cluster_high = (u16)(tx->new_first >> 16U);
    new_entry->first_cluster_low = (u16)tx->new_first;
    new_entry->file_size = (u32)length;
    tx->phase = FAT32_REPLACE_PUBLISHING;
    if (!fat32_mounted_write(sb, tx->directory_sector, 1U,
                             tx->proposed_sector)) {
        result = VFS_ERR_UNCERTAIN;
        goto uncertain;
    }
    state->first_cluster = tx->new_first;
    fat32_version_advance(state);
    vfs_nodes_update_size(sb, record_id, length);
    result = fat32_replace_reclaim(sb, fs, tx);
    if (result != VFS_OK) goto uncertain;
    result = fat32_replace_fsinfo(sb, tx);
    if (result != VFS_OK) goto uncertain;
    kfree(tx->data_buffer);
    tx->data_buffer = NULL;
    tx->phase = FAT32_REPLACE_HANDOFF_PUBLISHED;
    tx->handoff_pending = true;
    *out_pin = tx->pin_id;
    return VFS_OK;

cancel:
    if (fat32_replace_cancel(sb, fs, tx) != VFS_OK) goto uncertain;
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

/* The legacy create/mkdir writer must not truncate a requested name into an
 * unchecked alias. Preserve portable 8.3 punctuation as well as letters. */
static bool fat32_legacy_name_exact(const char *name)
{
    fat32_on_disk_entry_t entry = {0};
    if (!fat32_name_valid(name)) return false;
    fat32_create_83_name(name, entry.name);
    return fat32_match_name(&entry, name);
}

/* FAT32 creation currently produces conventional short entries only.  Keep
 * rename equally explicit: it is safe for one directory and one 8.3 entry,
 * but must not silently leave imported LFN records pointing at stale names. */
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
static int fat32_create_lfn_alias(vfs_node_t *dir, const char *name,
                                  char out_short[11])
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
        fat32_query_t query = {.mode = FAT32_QUERY_CREATE, .name = spelling};
        int result = fat32_dir_query_view(dir, &query, &view);
        if (result != VFS_OK) return result;
        if (query.found) continue;
        memcpy(out_short, candidate.name, sizeof(candidate.name));
        return VFS_OK;
    }
    return VFS_ERR_NO_SPACE;
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

/* The caller owns operation_lock and a private, preallocated VFS node. No
 * path lookup or VFS allocation follows the visibility write. */
static int fat32_create_contents_locked(vfs_node_t *dir,
                                        vfs_node_t *prepared_node,
                                        const char *name, u64 length,
                                        vfs_replace_source_t source,
                                        void *context,
                                        vfs_file_facts_t *out_facts,
                                        u64 *out_pin)
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
        (length && !source)) return VFS_ERR_INVALID_PARAM;
    *out_pin = 0;
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

    new_count = (u32)(length / fs->cluster_size_bytes +
                      (length % fs->cluster_size_bytes != 0U));
    tx = (fat32_replace_transaction_t *)kmalloc(sizeof(*tx));
    if (!tx) return VFS_ERR_NO_MEM;
    memset(tx, 0, sizeof(*tx));
    tx->new_size = (u32)length;
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
    if (long_name && lfn_count + 1U <= 16U) {
        result = fat32_create_lfn_slot(dir, lfn_count + 1U, tx,
                                        &slot.cluster, &slot.slot);
        if (result != VFS_OK && result != VFS_ERR_NO_SPACE) {
            if (result == VFS_ERR_BAD_FORMAT) fs->version_confident = false;
            goto before_pin;
        }
    } else if (!long_name && has_free_slot) {
        if (slot.cluster < 2U || slot.cluster >= fs->total_clusters ||
            slot.slot >= fs->cluster_size_bytes / sizeof(slot.disk)) {
            result = VFS_ERR_BAD_FORMAT;
            goto before_pin;
        }
        tx->directory_sector = fat32_cluster_to_lba(fs, slot.cluster) +
            slot.slot / (512U / sizeof(slot.disk));
        if (!fat32_mounted_read(dir->super, tx->directory_sector, 1U,
                                tx->old_sector)) {
            result = VFS_ERR_IO;
            goto before_pin;
        }
        fat32_on_disk_entry_t *old_entry =
            &((fat32_on_disk_entry_t *)tx->old_sector)[slot.slot % 16U];
        if (memcmp(old_entry, &slot.disk, sizeof(*old_entry)) != 0 ||
            ((u8)old_entry->name[0] != 0U &&
             (u8)old_entry->name[0] != 0xe5U)) {
            fs->version_confident = false;
            result = VFS_ERR_BAD_FORMAT;
            goto before_pin;
        }
        memcpy(tx->proposed_sector, tx->old_sector, 512U);
    }
    if (long_name && (lfn_count + 1U > 16U || result == VFS_ERR_NO_SPACE)) {
        tx->growth = (fat32_directory_growth_t *)kmalloc(sizeof(*tx->growth));
        tx->lfn_creation = (fat32_lfn_creation_t *)kmalloc(sizeof(*tx->lfn_creation));
        if (!tx->growth || !tx->lfn_creation) {
            result = VFS_ERR_NO_MEM;
            goto before_pin;
        }
        memset(tx->growth, 0, sizeof(*tx->growth));
        memset(tx->lfn_creation, 0, sizeof(*tx->lfn_creation));
        tx->growth->fat_step = 4U;
        result = fat32_create_cross_sector_plan(dir, tx, lfn_units,
            lfn_units_count, lfn_count, short_name, &slot.cluster, &slot.slot);
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
                    goto before_pin;
                }
                memset(tx->growth->second_images, 0,
                       sizeof(*tx->growth->second_images));
            }
            if (tx->growth && tx->growth->append && !tx->data_buffer) {
                tx->data_buffer = (u8 *)kmalloc(fs->cluster_size_bytes);
                if (!tx->data_buffer) { result = VFS_ERR_NO_MEM; goto before_pin; }
            }
        }
        else if (result == VFS_ERR_NO_SPACE) {
            kfree(tx->lfn_creation);
            tx->lfn_creation = NULL;
            memset(tx->growth, 0, sizeof(*tx->growth));
            tx->growth->fat_step = 4U;
        } else {
            if (result == VFS_ERR_BAD_FORMAT) fs->version_confident = false;
            goto before_pin;
        }
    }
    if (!cross_sector && ((long_name && result == VFS_ERR_NO_SPACE) ||
        (!long_name && !has_free_slot))) {
        if (!tx->growth) {
            tx->growth = (fat32_directory_growth_t *)kmalloc(sizeof(*tx->growth));
            if (!tx->growth) { result = VFS_ERR_NO_MEM; goto before_pin; }
            memset(tx->growth, 0, sizeof(*tx->growth));
            tx->growth->fat_step = 4U;
        }
        result = fat32_create_prepare_growth(dir, lfn_count + 1U, tx,
                                              &slot.cluster, &slot.slot);
        if (result != VFS_OK) {
            if (result == VFS_ERR_BAD_FORMAT) fs->version_confident = false;
            goto before_pin;
        }
        if (tx->growth->append && !tx->data_buffer) {
            tx->data_buffer = (u8 *)kmalloc(fs->cluster_size_bytes);
            if (!tx->data_buffer) { result = VFS_ERR_NO_MEM; goto before_pin; }
        }
    }
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
    entry->attr = 0x20U;
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
    result = fat32_stage_fresh(dir->super, fs, tx, length, source, context);
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
        } else if (tx->fresh_count &&
                   !block_flush_for_id(dir->super->dev, fs->device_id)) {
            goto uncertain;
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
    entry->file_size = (u32)length;
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
    if ((tx->growth || cross_sector) &&
        !block_flush_for_id(dir->super->dev, fs->device_id)) goto uncertain;

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
        .size = length,
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
    if (tx->growth &&
        (tx->fresh_count || tx->growth->candidate_recorded) &&
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

static int fat32_create(vfs_node_t *dir, const char *name, vfs_node_t **out_node) {
    fat32_prepared_node_t prepared = {0};
    fat32_walk_entry_t free_slot;
    u32 owner_uid;
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !out_node || !dir->super || !dir->super->private_data) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (!fat32_legacy_name_exact(name)) return VFS_ERR_INVALID_PARAM;

    int availability = fat32_check_name_available(dir, name, &free_slot);
    if (availability != VFS_OK) return availability;

    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    u32 dir_cluster = fat32_node_cluster(dir);
    if (dir_cluster == FAT32_CLUSTER_ERR) return VFS_ERR_UNCERTAIN;
    if (!vfs_current_uid(&owner_uid)) return VFS_ERR_ACCESS_DENIED;
    if (!fat32_prepare_node(fs, &prepared)) return VFS_ERR_NO_MEM;

    u32 new_cluster = fat32_alloc_cluster(dir->super);
    if (new_cluster == FAT32_CLUSTER_ERR) {
        fat32_discard_prepared(&prepared);
        return fat32_mutation_error(fs, VFS_ERR_NO_MEM);
    }

    u32 cluster_size = fs->cluster_size_bytes;
    u8 *cluster_buf = (u8 *)kmalloc(cluster_size);
    if (!cluster_buf) {
        fat32_release_unpublished(dir->super, fs, new_cluster);
        fat32_discard_prepared(&prepared);
        return fat32_mutation_error(fs, VFS_ERR_NO_MEM);
    }

    u64 lba = fat32_cluster_to_lba(fs, free_slot.cluster);
    if (!fat32_mounted_read(dir->super, lba, fs->sectors_per_cluster, cluster_buf)) {
        kfree(cluster_buf);
        fat32_release_unpublished(dir->super, fs, new_cluster);
        fat32_discard_prepared(&prepared);
        return fat32_mutation_error(fs, VFS_ERR_IO);
    }

    fat32_on_disk_entry_t *entries = (fat32_on_disk_entry_t *)cluster_buf;
    u32 count = cluster_size / sizeof(fat32_on_disk_entry_t);
    u32 target_idx = free_slot.slot;
    if (target_idx >= count ||
        ((u8)entries[target_idx].name[0] != 0x00U &&
         (u8)entries[target_idx].name[0] != 0xe5U)) {
        kfree(cluster_buf);
        fat32_release_unpublished(dir->super, fs, new_cluster);
        fat32_discard_prepared(&prepared);
        return fat32_mutation_error(fs, VFS_ERR_NO_MEM);
    }

    memset(&entries[target_idx], 0, sizeof(fat32_on_disk_entry_t));
    fat32_create_83_name(name, entries[target_idx].name);
    entries[target_idx].attr = 0x20;
    entries[target_idx].first_cluster_high = (u16)(new_cluster >> 16);
    entries[target_idx].first_cluster_low  = (u16)(new_cluster & 0xFFFF);
    entries[target_idx].file_size = 0;

    if (!fat32_mounted_write(dir->super, lba, fs->sectors_per_cluster, cluster_buf)) {
        kfree(cluster_buf);
        fat32_release_unpublished(dir->super, fs, new_cluster);
        fat32_discard_prepared(&prepared);
        return fat32_mutation_error(fs, VFS_ERR_IO);
    }

    kfree(cluster_buf);

    vfs_node_t *node = fat32_attach_node(
        dir->super, fs, free_slot.cluster, target_idx, new_cluster, 0,
        VFS_TYPE_FILE, prepared.node, prepared.state);
    vfs_node_set_security(node, owner_uid, VFS_DEFAULT_FILE_PERMISSIONS);
    fat32_version_advance(fat32_state(dir));
    *out_node = node;
    return VFS_OK;
}

static int fat32_mkdir(vfs_node_t *dir, const char *name, vfs_node_t **out_node) {
    fat32_prepared_node_t prepared = {0};
    fat32_walk_entry_t free_slot;
    u32 owner_uid;
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !out_node || !dir->super || !dir->super->private_data) {
        return VFS_ERR_INVALID_PARAM;
    }
    if (!fat32_legacy_name_exact(name)) return VFS_ERR_INVALID_PARAM;

    int availability = fat32_check_name_available(dir, name, &free_slot);
    if (availability != VFS_OK) return availability;

    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    u32 parent_cluster = fat32_node_cluster(dir);
    if (parent_cluster == FAT32_CLUSTER_ERR) return VFS_ERR_UNCERTAIN;
    if (!vfs_current_uid(&owner_uid)) return VFS_ERR_ACCESS_DENIED;
    if (!fat32_prepare_node(fs, &prepared)) return VFS_ERR_NO_MEM;

    u32 new_cluster = fat32_alloc_cluster(dir->super);
    if (new_cluster == FAT32_CLUSTER_ERR) {
        fat32_discard_prepared(&prepared);
        return fat32_mutation_error(fs, VFS_ERR_NO_MEM);
    }

    u32 cluster_size = fs->cluster_size_bytes;
    u8 *new_dir_buf = (u8 *)kmalloc(cluster_size);
    if (!new_dir_buf) {
        fat32_release_unpublished(dir->super, fs, new_cluster);
        fat32_discard_prepared(&prepared);
        return fat32_mutation_error(fs, VFS_ERR_NO_MEM);
    }

    memset(new_dir_buf, 0, cluster_size);
    fat32_on_disk_entry_t *dot_entries = (fat32_on_disk_entry_t *)new_dir_buf;

    /* Entry 0: "." */
    memset(dot_entries[0].name, ' ', 11);
    dot_entries[0].name[0] = '.';
    dot_entries[0].attr = 0x10;
    dot_entries[0].first_cluster_high = (u16)(new_cluster >> 16);
    dot_entries[0].first_cluster_low  = (u16)(new_cluster & 0xFFFF);

    /* Entry 1: ".." */
    memset(dot_entries[1].name, ' ', 11);
    dot_entries[1].name[0] = '.';
    dot_entries[1].name[1] = '.';
    dot_entries[1].attr = 0x10;
    u32 parent_link_cluster = (parent_cluster == fs->root_cluster) ? 0 : parent_cluster;
    dot_entries[1].first_cluster_high = (u16)(parent_link_cluster >> 16);
    dot_entries[1].first_cluster_low  = (u16)(parent_link_cluster & 0xFFFF);

    u64 new_dir_lba = fat32_cluster_to_lba(fs, new_cluster);
    if (!fat32_mounted_write(dir->super, new_dir_lba, fs->sectors_per_cluster, new_dir_buf)) {
        kfree(new_dir_buf);
        fat32_release_unpublished(dir->super, fs, new_cluster);
        fat32_discard_prepared(&prepared);
        return fat32_mutation_error(fs, VFS_ERR_IO);
    }
    kfree(new_dir_buf);

    /* Insert entry in parent directory */
    u8 *parent_buf = (u8 *)kmalloc(cluster_size);
    if (!parent_buf) {
        fat32_release_unpublished(dir->super, fs, new_cluster);
        fat32_discard_prepared(&prepared);
        return fat32_mutation_error(fs, VFS_ERR_NO_MEM);
    }

    u64 parent_lba = fat32_cluster_to_lba(fs, free_slot.cluster);
    if (!fat32_mounted_read(dir->super, parent_lba, fs->sectors_per_cluster, parent_buf)) {
        kfree(parent_buf);
        fat32_release_unpublished(dir->super, fs, new_cluster);
        fat32_discard_prepared(&prepared);
        return fat32_mutation_error(fs, VFS_ERR_IO);
    }

    fat32_on_disk_entry_t *parent_entries = (fat32_on_disk_entry_t *)parent_buf;
    u32 count = cluster_size / sizeof(fat32_on_disk_entry_t);
    u32 target_idx = free_slot.slot;
    if (target_idx >= count ||
        ((u8)parent_entries[target_idx].name[0] != 0x00U &&
         (u8)parent_entries[target_idx].name[0] != 0xe5U)) {
        kfree(parent_buf);
        fat32_release_unpublished(dir->super, fs, new_cluster);
        fat32_discard_prepared(&prepared);
        return fat32_mutation_error(fs, VFS_ERR_NO_MEM);
    }

    memset(&parent_entries[target_idx], 0, sizeof(fat32_on_disk_entry_t));
    fat32_create_83_name(name, parent_entries[target_idx].name);
    parent_entries[target_idx].attr = 0x10;
    parent_entries[target_idx].first_cluster_high = (u16)(new_cluster >> 16);
    parent_entries[target_idx].first_cluster_low  = (u16)(new_cluster & 0xFFFF);
    parent_entries[target_idx].file_size = 0;

    if (!fat32_mounted_write(dir->super, parent_lba, fs->sectors_per_cluster, parent_buf)) {
        kfree(parent_buf);
        fat32_release_unpublished(dir->super, fs, new_cluster);
        fat32_discard_prepared(&prepared);
        return fat32_mutation_error(fs, VFS_ERR_IO);
    }

    kfree(parent_buf);

    vfs_node_t *node = fat32_attach_node(
        dir->super, fs, free_slot.cluster, target_idx, new_cluster, 0,
        VFS_TYPE_DIRECTORY, prepared.node, prepared.state);
    vfs_node_set_security(node, owner_uid, VFS_DEFAULT_DIRECTORY_PERMISSIONS);
    fat32_version_advance(fat32_state(dir));
    *out_node = node;
    return VFS_OK;
}

static int fat32_unlink(vfs_node_t *dir, const char *name);

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
 * Directory chains use the bounded namespace-walk limit, not the file-size
 * limit, and namespace reads stay hidden until resolved handoff. */
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
    fs = (fat32_fs_t *)dir->super->private_data;
    if (!fs->version_confident || fs->replace_transaction)
        return VFS_ERR_UNCERTAIN;
    result = fat32_dir_find(dir, name, &match);
    if (result != VFS_OK) return result;
    if (!!(match.disk.attr & 0x10U) != directory)
        return VFS_ERR_INVALID_PARAM;
    if (!directory && (!match.has_lfn || !match.lfn.total ||
        match.slot % 16U >= match.lfn.total))
        return fat32_unlink(dir, name);
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
    if (directory && state &&
        (!state->version_valid || !state->node ||
         state->node->type != VFS_TYPE_DIRECTORY ||
         state->first_cluster != first)) {
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
        fat32_node_state_t old_state = {.first_cluster = first};
        vfs_node_t old_node = {.super = dir->super, .fs_data = &old_state,
                               .size = match.disk.file_size};
        result = fat32_replace_old_inventory(&old_node, tx);
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
    if (directory) fs->namespace_hidden = true;
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

static int fat32_unlink(vfs_node_t *dir, const char *name)
{
    fat32_walk_entry_t match;
    fat32_fs_t *fs;
    u8 *buffer;
    int result;
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY ||
        !dir->super || !dir->super->private_data) return VFS_ERR_INVALID_PARAM;
    fs = (fat32_fs_t *)dir->super->private_data;
    result = fat32_dir_find(dir, name, &match);
    if (result != VFS_OK) return result;
    if (match.disk.attr & 0x10U) return VFS_ERR_INVALID_PARAM;
    /* The new creation path publishes LFNs within one sector. Retire those
     * slots with their short entry; retain the existing handling of imported
     * entry sets that span sectors. */
    bool same_sector_lfn = match.has_lfn && match.lfn.total &&
                           match.slot % 16U >= match.lfn.total;
    if (match.has_lfn && !same_sector_lfn) return VFS_ERR_UNSUPPORTED;
    result = fat32_dir_load_slot(dir->super, &match, &buffer);
    if (result != VFS_OK) return result;
    u32 first = ((u32)match.disk.first_cluster_high << 16U) |
                match.disk.first_cluster_low;
    fat32_node_state_t *state = fat32_find_state(fs, match.cluster, match.slot);
    if (same_sector_lfn) {
        u32 start = match.slot - match.lfn.total;
        for (u32 i = 0U; i < match.lfn.total; i++) {
            fat32_lfn_entry_t *lfn =
                &((fat32_lfn_entry_t *)buffer)[start + i];
            u32 sequence = match.lfn.total - i;
            if (lfn->attr != 0x0fU ||
                lfn->order != (u8)(sequence | (i == 0U ? 0x40U : 0U)) ||
                lfn->checksum != fat32_lfn_checksum(match.disk.name)) {
                kfree(buffer);
                fs->version_confident = false;
                return VFS_ERR_BAD_FORMAT;
            }
        }
        for (u32 i = 0U; i < match.lfn.total; i++)
            buffer[(start + i) * 32U] = 0xe5U;
    }
    ((fat32_on_disk_entry_t *)buffer)[match.slot].name[0] = (char)0xe5;
    bool written = fat32_mounted_write(
        dir->super, fat32_cluster_to_lba(fs, match.cluster),
        fs->sectors_per_cluster, buffer);
    kfree(buffer);
    if (!written) {
        if (state) state->version_valid = false;
        return VFS_ERR_UNCERTAIN;
    }
    if (state) state->alive = false;
    fat32_version_advance(fat32_state(dir));
    return first < 2U || fat32_free_chain(dir->super, first)
        ? VFS_OK : VFS_ERR_UNCERTAIN;
}

static int fat32_rmdir(vfs_node_t *dir, const char *name)
{
    fat32_walk_entry_t match;
    fat32_fs_t *fs;
    u8 *buffer;
    int result;
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY ||
        !dir->super || !dir->super->private_data) return VFS_ERR_INVALID_PARAM;
    fs = (fat32_fs_t *)dir->super->private_data;
    result = fat32_dir_find(dir, name, &match);
    if (result != VFS_OK) return result;
    if (!(match.disk.attr & 0x10U)) return VFS_ERR_INVALID_PARAM;
    /* VFS selects rmdir_pinned for mounted FAT32 directories. Keep direct
     * legacy calls from retiring only the short slot of an LFN set. */
    if (match.has_lfn) return VFS_ERR_UNSUPPORTED;
    u32 first = ((u32)match.disk.first_cluster_high << 16U) |
                match.disk.first_cluster_low;
    fat32_empty_check_t check = {0};
    result = fat32_dir_walk(dir->super, first, true, false,
                            fat32_empty_visit, &check);
    if (result != VFS_OK) return result;
    if (check.occupied) return VFS_ERR_NOT_EMPTY;
    result = fat32_dir_load_slot(dir->super, &match, &buffer);
    if (result != VFS_OK) return result;
    fat32_node_state_t *state = fat32_find_state(fs, match.cluster, match.slot);
    ((fat32_on_disk_entry_t *)buffer)[match.slot].name[0] = (char)0xe5;
    bool written = fat32_mounted_write(
        dir->super, fat32_cluster_to_lba(fs, match.cluster),
        fs->sectors_per_cluster, buffer);
    kfree(buffer);
    if (!written) {
        if (state) state->version_valid = false;
        return VFS_ERR_UNCERTAIN;
    }
    if (state) state->alive = false;
    fat32_version_advance(fat32_state(dir));
    return fat32_free_chain(dir->super, first)
        ? VFS_OK : VFS_ERR_UNCERTAIN;
}

static int fat32_rename(vfs_node_t *src_dir, const char *src_name,
                         vfs_node_t *dst_dir, const char *dst_name)
{
    fat32_walk_entry_t source, destination;
    fat32_fs_t *fs;
    char short_name[11];
    u8 *buffer;
    int result;
    if (!src_dir || !dst_dir || !src_name || !dst_name ||
        src_dir->type != VFS_TYPE_DIRECTORY ||
        dst_dir->type != VFS_TYPE_DIRECTORY ||
        src_dir->super != dst_dir->super || !src_dir->super ||
        !src_dir->super->private_data ||
        src_dir->fs_data != dst_dir->fs_data)
        return VFS_ERR_UNSUPPORTED;
    if (!fat32_make_83_name(dst_name, short_name))
        return VFS_ERR_UNSUPPORTED;
    fs = (fat32_fs_t *)src_dir->super->private_data;
    result = fat32_dir_find(src_dir, src_name, &source);
    if (result != VFS_OK) return result;
    if (source.has_lfn) return VFS_ERR_UNSUPPORTED;
    result = fat32_dir_find(src_dir, dst_name, &destination);
    if (result != VFS_OK && result != VFS_ERR_NOT_FOUND) return result;
    if (result == VFS_OK &&
        (source.cluster != destination.cluster ||
         source.slot != destination.slot)) return VFS_ERR_INVALID_PARAM;
    result = fat32_dir_load_slot(src_dir->super, &source, &buffer);
    if (result != VFS_OK) return result;
    fat32_on_disk_entry_t *entry = &((fat32_on_disk_entry_t *)buffer)[source.slot];
    memcpy(entry->name, short_name, sizeof(entry->name));
    entry->nt_reserved &= (u8)~0x18U;
    bool written = fat32_mounted_write(
        src_dir->super, fat32_cluster_to_lba(fs, source.cluster),
        fs->sectors_per_cluster, buffer);
    kfree(buffer);
    fat32_node_state_t *state = fat32_find_state(fs, source.cluster,
                                                 source.slot);
    if (!written) {
        if (state) state->version_valid = false;
        return VFS_ERR_UNCERTAIN;
    }
    fat32_version_advance(state);
    fat32_version_advance(fat32_state(src_dir));
    return VFS_OK;
}

static const vfs_ops_t fat32_node_ops = {
    .read = fat32_read,
    .write = fat32_write,
    .file_facts = fat32_file_facts,
    .replace = fat32_replace_locked,
    .replacement_max_bytes = FAT32_REPLACE_MAX_BYTES,
    .create_contents = fat32_create_contents_locked,
    .create_contents_max_bytes = FAT32_REPLACE_MAX_BYTES,
    .discard_created_node = fat32_discard_created_node,
    .snapshot_stream = fat32_snapshot_stream,
    .finddir = fat32_finddir,
    .readdir = fat32_readdir,
    .readdir_open = NULL,
    .readdir_next = NULL,
    .readdir_close = NULL,
    .create = fat32_create,
    .mkdir = fat32_mkdir,
    .unlink = fat32_unlink,
    .unlink_pinned = fat32_unlink_pinned,
    .rmdir = fat32_rmdir,
    .rmdir_pinned = fat32_rmdir_pinned,
    .rename = fat32_rename,
    .truncate = fat32_truncate,
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

static const vfs_super_ops_t fat32_super_ops = {
    .unmount = fat32_unmount,
    .sync = NULL,
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

bool fat32_set_label(block_device_t *dev, const char *label)
{
    fat32_fs_t fs;
    fat32_bpb_t bpb;
    u8 primary[512];
    u8 backup[512];
    u8 *cluster_data;
    usize label_length;
    u32 cluster;
    bool directory_updated = false;

    if (!fat32_label_text_valid(label, &label_length) ||
        !fat32_label_load(dev, &fs, &bpb) ||
        bpb.backup_boot_sector == 0U ||
        bpb.backup_boot_sector >= dev->sector_count ||
        !block_read(dev, 0U, 1U, primary) ||
        !block_read(dev, bpb.backup_boot_sector, 1U, backup)) return false;
    cluster_data = (u8 *)kmalloc(fs.cluster_size_bytes);
    if (!cluster_data) return false;
    cluster = fs.root_cluster;
    for (u32 step = 0; step < fs.total_clusters - 2U && !directory_updated;
         step++) {
        u64 lba;
        fat32_on_disk_entry_t *entries;
        u32 count;
        u32 candidate = ~(u32)0;
        u32 next;

        if (!fat32_label_cluster_lba(&fs, cluster, &lba) ||
            lba > dev->sector_count ||
            fs.sectors_per_cluster > dev->sector_count - lba ||
            !block_read(dev, lba, fs.sectors_per_cluster, cluster_data))
            break;
        entries = (fat32_on_disk_entry_t *)cluster_data;
        count = fs.cluster_size_bytes / sizeof(*entries);
        for (u32 index = 0; index < count; index++) {
            u8 first = (u8)entries[index].name[0];
            if (entries[index].attr == 0x08U && first != 0xe5U &&
                first != 0x00U) {
                candidate = index;
                break;
            }
            if (candidate == ~(u32)0 && (first == 0xe5U || first == 0x00U))
                candidate = index;
            if (first == 0x00U) break;
        }
        if (candidate != ~(u32)0) {
            fat32_on_disk_entry_t *entry = &entries[candidate];
            if (label_length) {
                memset(entry, 0, sizeof(*entry));
                memset(entry->name, ' ', sizeof(entry->name));
                memcpy(entry->name, label, label_length);
                entry->attr = 0x08U;
            } else if (entry->attr == 0x08U &&
                       (u8)entry->name[0] != 0xe5U &&
                       entry->name[0] != 0x00) {
                memset(entry, 0, sizeof(*entry));
                entry->name[0] = (char)0xe5U;
            }
            if (!block_write(dev, lba, fs.sectors_per_cluster, cluster_data))
                break;
            directory_updated = true;
            break;
        }
        if (!fat32_label_next_cluster(dev, &fs, cluster, &next) ||
            next >= FAT32_EOC_MIN) break;
        cluster = next;
    }
    kfree(cluster_data);
    if (!directory_updated && label_length) return false;
    memcpy(primary + 71U, "NO NAME    ", 11U);
    memcpy(backup + 71U, "NO NAME    ", 11U);
    if (label_length) {
        memcpy(primary + 71U, label, label_length);
        memcpy(backup + 71U, label, label_length);
    }
    return block_write(dev, 0U, 1U, primary) &&
           block_write(dev, bpb.backup_boot_sector, 1U, backup);
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
    (void)read_only;
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
