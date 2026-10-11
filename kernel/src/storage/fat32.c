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
static u32 fat32_get_next_cluster(vfs_super_t *sb, fat32_fs_t *fs, u32 current_cluster);
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

static u64 fat32_cluster_to_lba(fat32_fs_t *fs, u32 cluster) {
    return (u64)fs->data_start_lba + (u64)(cluster - 2) * fs->sectors_per_cluster;
}

static char fat32_ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static void fat32_format_83_name(const fat32_on_disk_entry_t *entry,
                                 char *dst_out) {
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
}

static bool fat32_match_text(const char *stored_name, const char *target_name) {
    const char *s1 = stored_name;
    const char *s2 = target_name;

    while (*s1 && *s2) {
        if (fat32_ascii_lower(*s1) != fat32_ascii_lower(*s2)) return false;
        s1++;
        s2++;
    }
    return (*s1 == '\0' && *s2 == '\0');
}

static bool fat32_match_name(const fat32_on_disk_entry_t *entry,
                             const char *target_name) {
    char formatted[13];
    fat32_format_83_name(entry, formatted);
    return fat32_match_text(formatted, target_name);
}

typedef struct {
    u16 units[260];
    bool present[20];
    u8 total;
    u8 checksum;
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
                              const fat32_lfn_entry_t *entry) {
    u8 sequence;
    u32 base;

    if (!state || !entry || entry->attr != 0x0FU || entry->type != 0U ||
        entry->first_cluster != 0U) return;
    sequence = entry->order & 0x1FU;
    if (sequence == 0U || sequence > 20U) {
        state->valid = false;
        return;
    }
    if (entry->order & 0x40U) {
        fat32_lfn_reset(state);
        state->total = sequence;
        state->checksum = entry->checksum;
        state->valid = true;
    }
    if (!state->valid || sequence > state->total ||
        state->present[sequence - 1U] || entry->checksum != state->checksum) {
        state->valid = false;
        return;
    }
    base = (u32)(sequence - 1U) * 13U;
    for (u32 i = 0; i < 5U; i++) state->units[base + i] = entry->name1[i];
    for (u32 i = 0; i < 6U; i++) state->units[base + 5U + i] = entry->name2[i];
    for (u32 i = 0; i < 2U; i++) state->units[base + 11U + i] = entry->name3[i];
    state->present[sequence - 1U] = true;
}

static bool fat32_lfn_encode(const fat32_lfn_state_t *state,
                             const fat32_on_disk_entry_t *short_entry,
                             char *output, usize capacity) {
    u32 unit_count;
    usize position = 0;

    if (!state || !short_entry || !output || capacity == 0U || !state->valid ||
        state->total == 0U || state->total > 20U ||
        fat32_lfn_checksum(short_entry->name) != state->checksum) return false;
    for (u32 i = 0; i < state->total; i++) {
        if (!state->present[i]) return false;
    }
    unit_count = (u32)state->total * 13U;
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
    char long_name[256];
    if (fat32_lfn_encode(state, entry, long_name, sizeof(long_name))) {
        return fat32_match_text(long_name, target_name);
    }
    return fat32_match_name(entry, target_name);
}

static bool fat32_readdir(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry) {
    if (!dir || !out_entry || !dir->super || !dir->super->private_data) {
        return false;
    }

    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    u32 cluster = fat32_node_cluster(dir);
    if (cluster < 2U || cluster >= fs->total_clusters) return false;

    u32 buf_size = fs->cluster_size_bytes;
    u8 *cluster_buf = (u8 *)kmalloc(buf_size);
    if (!cluster_buf) {
        return false;
    }

    u64 lba = fat32_cluster_to_lba(fs, cluster);
    if (!fat32_mounted_read(dir->super, lba, fs->sectors_per_cluster, cluster_buf)) {
        kfree(cluster_buf);
        return false;
    }

    u32 entry_count = buf_size / sizeof(fat32_on_disk_entry_t);
    fat32_on_disk_entry_t *entries = (fat32_on_disk_entry_t *)cluster_buf;
    fat32_lfn_state_t lfn;
    u32 valid_idx = 0;
    bool found = false;
    fat32_lfn_reset(&lfn);

    for (u32 i = 0; i < entry_count; i++) {
        u8 first_byte = (u8)entries[i].name[0];

        if (first_byte == 0x00) {
            break;
        }

        if (first_byte == 0xE5) {
            fat32_lfn_reset(&lfn);
            continue;
        }

        if ((entries[i].attr & 0x0F) == 0x0F) {
            fat32_lfn_collect(&lfn, (const fat32_lfn_entry_t *)&entries[i]);
            continue;
        }
        if (entries[i].attr & 0x08) {
            fat32_lfn_reset(&lfn);
            continue;
        }

        /* FAT stores dot and dot-dot entries in every directory.  They are
         * path-navigation semantics, not user-visible directory members. */
        if (entries[i].name[0] == '.' &&
            (entries[i].name[1] == ' ' || entries[i].name[1] == '.')) {
            fat32_lfn_reset(&lfn);
            continue;
        }

        if (valid_idx == index) {
            if (!fat32_lfn_encode(&lfn, &entries[i], out_entry->name,
                                  sizeof(out_entry->name))) {
                fat32_format_83_name(&entries[i], out_entry->name);
            }
            u32 start_cluster = ((u32)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
            vfs_node_t *member = fat32_bind_node(
                dir->super, fs, cluster, i, start_cluster,
                entries[i].file_size,
                (entries[i].attr & 0x10) ? VFS_TYPE_DIRECTORY : VFS_TYPE_FILE);
            if (!member) break;
            out_entry->inode = member->inode;
            out_entry->type = (entries[i].attr & 0x10) ? VFS_TYPE_DIRECTORY : VFS_TYPE_FILE;
            found = true;
            break;
        }

        valid_idx++;
        fat32_lfn_reset(&lfn);
    }

    kfree(cluster_buf);
    return found;
}

static vfs_node_t *fat32_finddir(vfs_node_t *dir, const char *name) {
    if (!dir || !name || !dir->super || !dir->super->private_data) {
        return NULL;
    }

    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    u32 cluster = fat32_node_cluster(dir);
    if (cluster < 2U || cluster >= fs->total_clusters) return NULL;

    u32 buf_size = fs->cluster_size_bytes;
    u8 *cluster_buf = (u8 *)kmalloc(buf_size);
    if (!cluster_buf) {
        return NULL;
    }

    u64 lba = fat32_cluster_to_lba(fs, cluster);
    if (!fat32_mounted_read(dir->super, lba, fs->sectors_per_cluster, cluster_buf)) {
        kfree(cluster_buf);
        return NULL;
    }

    u32 entry_count = buf_size / sizeof(fat32_on_disk_entry_t);
    fat32_on_disk_entry_t *entries = (fat32_on_disk_entry_t *)cluster_buf;
    fat32_lfn_state_t lfn;
    vfs_node_t *node = NULL;
    fat32_lfn_reset(&lfn);

    for (u32 i = 0; i < entry_count; i++) {
        u8 first_byte = (u8)entries[i].name[0];

        if (first_byte == 0x00) {
            break;
        }

        if (first_byte == 0xE5) {
            fat32_lfn_reset(&lfn);
            continue;
        }

        if ((entries[i].attr & 0x0F) == 0x0F) {
            fat32_lfn_collect(&lfn, (const fat32_lfn_entry_t *)&entries[i]);
            continue;
        }
        if (entries[i].attr & 0x08) {
            fat32_lfn_reset(&lfn);
            continue;
        }
        if (entries[i].name[0] == '.' &&
            (entries[i].name[1] == ' ' || entries[i].name[1] == '.')) {
            fat32_lfn_reset(&lfn);
            continue;
        }

        if (fat32_match_entry(&entries[i], &lfn, name)) {
            u32 start_cluster = ((u32)entries[i].first_cluster_high << 16) |
                                entries[i].first_cluster_low;
            node = fat32_bind_node(dir->super, fs, cluster, i, start_cluster,
                                   entries[i].file_size,
                                   (entries[i].attr & 0x10)
                                       ? VFS_TYPE_DIRECTORY : VFS_TYPE_FILE);
            break;
        }
        fat32_lfn_reset(&lfn);
    }

    kfree(cluster_buf);
    return node;
}

/* Creation must distinguish a missing name from lookup-node allocation or
 * read failure. It cannot use finddir()'s nullable node as its collision
 * decision. The current FAT32 namespace implementation scans one directory
 * cluster, matching its existing supported lookup behavior. */
static int fat32_check_name_available(vfs_node_t *dir, const char *name)
{
    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    u32 cluster = fat32_node_cluster(dir);
    u8 *buffer;
    fat32_on_disk_entry_t *entries;
    fat32_lfn_state_t lfn;

    if (cluster < 2U || cluster >= fs->total_clusters)
        return VFS_ERR_UNCERTAIN;
    buffer = (u8 *)kmalloc(fs->cluster_size_bytes);
    if (!buffer) return VFS_ERR_NO_MEM;
    if (!fat32_mounted_read(dir->super, fat32_cluster_to_lba(fs, cluster),
                            fs->sectors_per_cluster, buffer)) {
        kfree(buffer);
        return VFS_ERR_IO;
    }
    entries = (fat32_on_disk_entry_t *)buffer;
    fat32_lfn_reset(&lfn);
    for (u32 i = 0; i < fs->cluster_size_bytes / sizeof(*entries); i++) {
        u8 first = (u8)entries[i].name[0];
        if (!first) break;
        if (first == 0xE5U) {
            fat32_lfn_reset(&lfn);
            continue;
        }
        if ((entries[i].attr & 0x0fU) == 0x0fU) {
            fat32_lfn_collect(&lfn, (fat32_lfn_entry_t *)&entries[i]);
            continue;
        }
        if (!(entries[i].attr & 0x08U) &&
            fat32_match_entry(&entries[i], &lfn, name)) {
            kfree(buffer);
            return VFS_ERR_INVALID_PARAM;
        }
        fat32_lfn_reset(&lfn);
    }
    kfree(buffer);
    return VFS_OK;
}

static u32 fat32_get_next_cluster(vfs_super_t *sb, fat32_fs_t *fs, u32 current_cluster) {
    if (!sb || !fs || current_cluster < 2 ||
        current_cluster >= fs->total_clusters) {
        return FAT32_CLUSTER_ERR;
    }

    u32 fat_offset = current_cluster * 4;
    u32 fat_sector = fs->fat_start_lba + (fat_offset / fs->bytes_per_sector);
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
    u32 fat_sector = fs->fat_start_lba + (fat_offset / fs->bytes_per_sector);
    u32 entry_offset = fat_offset % fs->bytes_per_sector;

    u8 *sector_buf = (u8 *)kmalloc(fs->bytes_per_sector);
    if (!sector_buf) return false;

    if (!fat32_mounted_read(sb, fat_sector, 1, sector_buf)) {
        kfree(sector_buf);
        return false;
    }

    u32 *entry_ptr = (u32 *)(sector_buf + entry_offset);
    *entry_ptr = (*entry_ptr & 0xF0000000) | (val & 0x0FFFFFFF);

    for (u32 fat_idx = 0; fat_idx < fs->fat_count; fat_idx++) {
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

static int fat32_create(vfs_node_t *dir, const char *name, vfs_node_t **out_node) {
    fat32_prepared_node_t prepared = {0};
    u32 owner_uid;
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !out_node || !dir->super || !dir->super->private_data) {
        return VFS_ERR_INVALID_PARAM;
    }

    int availability = fat32_check_name_available(dir, name);
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

    u64 lba = fat32_cluster_to_lba(fs, dir_cluster);
    if (!fat32_mounted_read(dir->super, lba, fs->sectors_per_cluster, cluster_buf)) {
        kfree(cluster_buf);
        fat32_release_unpublished(dir->super, fs, new_cluster);
        fat32_discard_prepared(&prepared);
        return fat32_mutation_error(fs, VFS_ERR_IO);
    }

    fat32_on_disk_entry_t *entries = (fat32_on_disk_entry_t *)cluster_buf;
    u32 count = cluster_size / sizeof(fat32_on_disk_entry_t);
    int target_idx = -1;

    for (u32 i = 0; i < count; i++) {
        u8 first = (u8)entries[i].name[0];
        if (first == 0x00 || first == 0xE5) {
            target_idx = i;
            break;
        }
    }

    if (target_idx == -1) {
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
        dir->super, fs, dir_cluster, (u32)target_idx, new_cluster, 0,
        VFS_TYPE_FILE, prepared.node, prepared.state);
    vfs_node_set_security(node, owner_uid, VFS_DEFAULT_FILE_PERMISSIONS);
    fat32_version_advance(fat32_state(dir));
    *out_node = node;
    return VFS_OK;
}

static int fat32_mkdir(vfs_node_t *dir, const char *name, vfs_node_t **out_node) {
    fat32_prepared_node_t prepared = {0};
    u32 owner_uid;
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !out_node || !dir->super || !dir->super->private_data) {
        return VFS_ERR_INVALID_PARAM;
    }

    int availability = fat32_check_name_available(dir, name);
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

    u64 parent_lba = fat32_cluster_to_lba(fs, parent_cluster);
    if (!fat32_mounted_read(dir->super, parent_lba, fs->sectors_per_cluster, parent_buf)) {
        kfree(parent_buf);
        fat32_release_unpublished(dir->super, fs, new_cluster);
        fat32_discard_prepared(&prepared);
        return fat32_mutation_error(fs, VFS_ERR_IO);
    }

    fat32_on_disk_entry_t *parent_entries = (fat32_on_disk_entry_t *)parent_buf;
    u32 count = cluster_size / sizeof(fat32_on_disk_entry_t);
    int target_idx = -1;

    for (u32 i = 0; i < count; i++) {
        u8 first = (u8)parent_entries[i].name[0];
        if (first == 0x00 || first == 0xE5) {
            target_idx = i;
            break;
        }
    }

    if (target_idx == -1) {
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
        dir->super, fs, parent_cluster, (u32)target_idx, new_cluster, 0,
        VFS_TYPE_DIRECTORY, prepared.node, prepared.state);
    vfs_node_set_security(node, owner_uid, VFS_DEFAULT_DIRECTORY_PERMISSIONS);
    fat32_version_advance(fat32_state(dir));
    *out_node = node;
    return VFS_OK;
}

static int fat32_unlink(vfs_node_t *dir, const char *name) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !dir->super || !dir->super->private_data) {
        return VFS_ERR_INVALID_PARAM;
    }

    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    u32 parent_cluster = fat32_node_cluster(dir);
    if (parent_cluster == FAT32_CLUSTER_ERR) return VFS_ERR_UNCERTAIN;

    u32 cluster_size = fs->cluster_size_bytes;
    u8 *parent_buf = (u8 *)kmalloc(cluster_size);
    if (!parent_buf) {
        return VFS_ERR_NO_MEM;
    }

    u64 parent_lba = fat32_cluster_to_lba(fs, parent_cluster);
    if (!fat32_mounted_read(dir->super, parent_lba, fs->sectors_per_cluster, parent_buf)) {
        kfree(parent_buf);
        return VFS_ERR_IO;
    }

    fat32_on_disk_entry_t *entries = (fat32_on_disk_entry_t *)parent_buf;
    fat32_lfn_state_t lfn;
    u32 count = cluster_size / sizeof(fat32_on_disk_entry_t);
    int target_idx = -1;
    u32 target_cluster = 0;
    fat32_lfn_reset(&lfn);

    for (u32 i = 0; i < count; i++) {
        u8 first = (u8)entries[i].name[0];
        if (first == 0x00) break;
        if (first == 0xE5) {
            fat32_lfn_reset(&lfn);
            continue;
        }
        if ((entries[i].attr & 0x0F) == 0x0F) {
            fat32_lfn_collect(&lfn, (const fat32_lfn_entry_t *)&entries[i]);
            continue;
        }
        if (entries[i].attr & 0x08) {
            fat32_lfn_reset(&lfn);
            continue;
        }

        if (fat32_match_entry(&entries[i], &lfn, name)) {
            if (entries[i].attr & 0x10) {
                kfree(parent_buf);
                return VFS_ERR_INVALID_PARAM;
            }
            target_idx = i;
            target_cluster = ((u32)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
            break;
        }
        fat32_lfn_reset(&lfn);
    }

    if (target_idx == -1) {
        kfree(parent_buf);
        return VFS_ERR_NOT_FOUND;
    }

    fat32_node_state_t *state = fat32_find_state(fs, parent_cluster,
                                                 (u32)target_idx);
    entries[target_idx].name[0] = (char)0xE5;
    if (!fat32_mounted_write(dir->super, parent_lba, fs->sectors_per_cluster, parent_buf)) {
        if (state) state->version_valid = false;
        kfree(parent_buf);
        return VFS_ERR_UNCERTAIN;
    }
    kfree(parent_buf);

    if (state) state->alive = false;
    fat32_version_advance(fat32_state(dir));
    return target_cluster < 2U || fat32_free_chain(dir->super, target_cluster)
        ? VFS_OK : VFS_ERR_UNCERTAIN;
}

static int fat32_rmdir(vfs_node_t *dir, const char *name) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !dir->super || !dir->super->private_data) {
        return VFS_ERR_INVALID_PARAM;
    }

    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    u32 parent_cluster = fat32_node_cluster(dir);
    if (parent_cluster == FAT32_CLUSTER_ERR) return VFS_ERR_UNCERTAIN;

    u32 cluster_size = fs->cluster_size_bytes;
    u8 *parent_buf = (u8 *)kmalloc(cluster_size);
    if (!parent_buf) {
        return VFS_ERR_NO_MEM;
    }

    u64 parent_lba = fat32_cluster_to_lba(fs, parent_cluster);
    if (!fat32_mounted_read(dir->super, parent_lba, fs->sectors_per_cluster, parent_buf)) {
        kfree(parent_buf);
        return VFS_ERR_IO;
    }

    fat32_on_disk_entry_t *entries = (fat32_on_disk_entry_t *)parent_buf;
    fat32_lfn_state_t lfn;
    u32 count = cluster_size / sizeof(fat32_on_disk_entry_t);
    int target_idx = -1;
    u32 target_cluster = 0;
    fat32_lfn_reset(&lfn);

    for (u32 i = 0; i < count; i++) {
        u8 first = (u8)entries[i].name[0];
        if (first == 0x00) break;
        if (first == 0xE5) {
            fat32_lfn_reset(&lfn);
            continue;
        }
        if ((entries[i].attr & 0x0F) == 0x0F) {
            fat32_lfn_collect(&lfn, (const fat32_lfn_entry_t *)&entries[i]);
            continue;
        }
        if (entries[i].attr & 0x08) {
            fat32_lfn_reset(&lfn);
            continue;
        }

        if (fat32_match_entry(&entries[i], &lfn, name)) {
            if (!(entries[i].attr & 0x10)) {
                kfree(parent_buf);
                return VFS_ERR_INVALID_PARAM;
            }
            target_idx = i;
            target_cluster = ((u32)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
            break;
        }
        fat32_lfn_reset(&lfn);
    }

    if (target_idx == -1) {
        kfree(parent_buf);
        return VFS_ERR_NOT_FOUND;
    }
    fat32_node_state_t *state = fat32_find_state(fs, parent_cluster,
                                                 (u32)target_idx);

    u8 *dir_buf = (u8 *)kmalloc(cluster_size);
    if (!dir_buf) {
        kfree(parent_buf);
        return VFS_ERR_NO_MEM;
    }

    u64 target_lba = fat32_cluster_to_lba(fs, target_cluster);
    if (!fat32_mounted_read(dir->super, target_lba, fs->sectors_per_cluster, dir_buf)) {
        kfree(dir_buf);
        kfree(parent_buf);
        return VFS_ERR_IO;
    }

    fat32_on_disk_entry_t *dir_entries = (fat32_on_disk_entry_t *)dir_buf;
    u32 dir_count = cluster_size / sizeof(fat32_on_disk_entry_t);
    bool is_empty = true;

    for (u32 i = 0; i < dir_count; i++) {
        u8 first = (u8)dir_entries[i].name[0];
        if (first == 0x00) break;
        if (first == 0xE5) continue;

        if (dir_entries[i].name[0] == '.') {
            if (dir_entries[i].name[1] == ' ' || dir_entries[i].name[1] == '.') {
                continue;
            }
        }

        is_empty = false;
        break;
    }

    kfree(dir_buf);

    if (!is_empty) {
        kfree(parent_buf);
        return VFS_ERR_NOT_EMPTY;
    }

    entries[target_idx].name[0] = (char)0xE5;
    if (!fat32_mounted_write(dir->super, parent_lba, fs->sectors_per_cluster, parent_buf)) {
        if (state) state->version_valid = false;
        kfree(parent_buf);
        return VFS_ERR_UNCERTAIN;
    }
    kfree(parent_buf);

    if (state) state->alive = false;
    fat32_version_advance(fat32_state(dir));
    return fat32_free_chain(dir->super, target_cluster)
        ? VFS_OK : VFS_ERR_UNCERTAIN;
}

static int fat32_rename(vfs_node_t *src_dir, const char *src_name,
                        vfs_node_t *dst_dir, const char *dst_name) {
    fat32_fs_t *fs;
    u8 *directory_buf;
    fat32_on_disk_entry_t *entries;
    fat32_lfn_state_t lfn;
    char short_name[11];
    u32 directory_cluster;
    u32 count;
    int source_index = -1;
    bool source_has_lfn = false;

    if (!src_dir || !dst_dir || !src_name || !dst_name ||
        src_dir->type != VFS_TYPE_DIRECTORY ||
        dst_dir->type != VFS_TYPE_DIRECTORY ||
        src_dir->super != dst_dir->super || !src_dir->super ||
        !src_dir->super->private_data ||
        src_dir->fs_data != dst_dir->fs_data) {
        return VFS_ERR_UNSUPPORTED;
    }
    if (!fat32_make_83_name(dst_name, short_name)) return VFS_ERR_UNSUPPORTED;

    fs = (fat32_fs_t *)src_dir->super->private_data;
    directory_cluster = fat32_node_cluster(src_dir);
    if (directory_cluster < 2U || directory_cluster >= fs->total_clusters)
        return VFS_ERR_IO;
    directory_buf = (u8 *)kmalloc(fs->cluster_size_bytes);
    if (!directory_buf) return VFS_ERR_NO_MEM;
    if (!fat32_mounted_read(src_dir->super,
                    fat32_cluster_to_lba(fs, directory_cluster),
                    fs->sectors_per_cluster, directory_buf)) {
        kfree(directory_buf);
        return VFS_ERR_IO;
    }

    entries = (fat32_on_disk_entry_t *)directory_buf;
    count = fs->cluster_size_bytes / sizeof(*entries);
    fat32_lfn_reset(&lfn);
    for (u32 index = 0; index < count; index++) {
        u8 first = (u8)entries[index].name[0];
        char decoded_lfn[256];

        if (first == 0x00U) break;
        if (first == 0xE5U) {
            fat32_lfn_reset(&lfn);
            continue;
        }
        if ((entries[index].attr & 0x0FU) == 0x0FU) {
            fat32_lfn_collect(&lfn,
                               (const fat32_lfn_entry_t *)&entries[index]);
            continue;
        }
        if ((entries[index].attr & 0x08U) != 0U) {
            fat32_lfn_reset(&lfn);
            continue;
        }
        if (fat32_match_entry(&entries[index], &lfn, dst_name)) {
            if (!fat32_match_entry(&entries[index], &lfn, src_name)) {
                kfree(directory_buf);
                return VFS_ERR_INVALID_PARAM;
            }
        }
        if (fat32_match_entry(&entries[index], &lfn, src_name)) {
            source_index = (int)index;
            source_has_lfn = fat32_lfn_encode(&lfn, &entries[index],
                                               decoded_lfn,
                                               sizeof(decoded_lfn));
        }
        fat32_lfn_reset(&lfn);
    }
    if (source_index < 0) {
        kfree(directory_buf);
        return VFS_ERR_NOT_FOUND;
    }
    if (source_has_lfn) {
        kfree(directory_buf);
        return VFS_ERR_UNSUPPORTED;
    }

    memcpy(entries[source_index].name, short_name,
           sizeof(entries[source_index].name));
    entries[source_index].nt_reserved &= (u8)~0x18U;
    if (!fat32_mounted_write(src_dir->super,
                     fat32_cluster_to_lba(fs, directory_cluster),
                     fs->sectors_per_cluster, directory_buf)) {
        fat32_node_state_t *state = fat32_find_state(
            fs, directory_cluster, (u32)source_index);
        if (state) state->version_valid = false;
        kfree(directory_buf);
        return VFS_ERR_UNCERTAIN;
    }
    kfree(directory_buf);
    fat32_version_advance(fat32_find_state(
        fs, directory_cluster, (u32)source_index));
    fat32_version_advance(fat32_state(src_dir));
    return VFS_OK;
}

static const vfs_ops_t fat32_node_ops = {
    .read = fat32_read,
    .write = fat32_write,
    .file_facts = fat32_file_facts,
    .snapshot_stream = fat32_snapshot_stream,
    .finddir = fat32_finddir,
    .readdir = fat32_readdir,
    .readdir_open = NULL,
    .readdir_next = NULL,
    .readdir_close = NULL,
    .create = fat32_create,
    .mkdir = fat32_mkdir,
    .unlink = fat32_unlink,
    .rmdir = fat32_rmdir,
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
