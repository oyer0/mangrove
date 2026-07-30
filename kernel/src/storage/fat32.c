#include <storage/fat32.h>
#include <heap.h>
#include <string.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

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
#pragma pack(pop)

typedef struct {
    fat32_bpb_t bpb;
    u32 bytes_per_sector;
    u32 sectors_per_cluster;
    u32 cluster_size_bytes;
    u32 reserved_sector_count;
    u32 fat_count;
    u32 fat_size_sectors;
    u32 fat_start_lba;
    u32 data_start_lba;
    u32 root_cluster;
    u32 total_clusters;
    u32 next_free_cluster;
} fat32_fs_t;

static const vfs_ops_t fat32_node_ops;
static u32 fat32_get_next_cluster(vfs_super_t *sb, fat32_fs_t *fs, u32 current_cluster);

static u64 fat32_cluster_to_lba(fat32_fs_t *fs, u32 cluster) {
    return (u64)fs->data_start_lba + (u64)(cluster - 2) * fs->sectors_per_cluster;
}

static void fat32_format_83_name(const char *src_11, char *dst_out) {
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
        dst_out[pos++] = src_11[i];
    }

    if (ext_len > 0) {
        dst_out[pos++] = '.';
        for (u32 i = 0; i < ext_len; i++) {
            dst_out[pos++] = src_11[8 + i];
        }
    }

    dst_out[pos] = '\0';
}

static bool fat32_match_name(const char *src_11, const char *target_name) {
    char formatted[13];
    fat32_format_83_name(src_11, formatted);

    const char *s1 = formatted;
    const char *s2 = target_name;

    while (*s1 && *s2) {
        char c1 = *s1;
        char c2 = *s2;
        if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
        if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
        if (c1 != c2) return false;
        s1++;
        s2++;
    }

    return (*s1 == '\0' && *s2 == '\0');
}

static bool fat32_readdir(vfs_node_t *dir, u32 index, vfs_dirent_t *out_entry) {
    if (!dir || !out_entry || !dir->super || !dir->super->private_data) {
        return false;
    }

    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    u32 cluster = (u32)(uintptr_t)dir->fs_data;

    u32 buf_size = fs->cluster_size_bytes;
    u8 *cluster_buf = (u8 *)kmalloc(buf_size);
    if (!cluster_buf) {
        return false;
    }

    u64 lba = fat32_cluster_to_lba(fs, cluster);
    if (!block_read(dir->super->dev, lba, fs->sectors_per_cluster, cluster_buf)) {
        kfree(cluster_buf);
        return false;
    }

    u32 entry_count = buf_size / sizeof(fat32_on_disk_entry_t);
    fat32_on_disk_entry_t *entries = (fat32_on_disk_entry_t *)cluster_buf;
    u32 valid_idx = 0;
    bool found = false;

    for (u32 i = 0; i < entry_count; i++) {
        u8 first_byte = (u8)entries[i].name[0];

        if (first_byte == 0x00) {
            break;
        }

        if (first_byte == 0xE5) {
            continue;
        }

        /* Skip LFN (0x0F) and Volume Label (0x08) */
        if ((entries[i].attr & 0x0F) == 0x0F || (entries[i].attr & 0x08)) {
            continue;
        }

        if (valid_idx == index) {
            fat32_format_83_name(entries[i].name, out_entry->name);
            u32 start_cluster = ((u32)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
            out_entry->inode = start_cluster;
            out_entry->type = (entries[i].attr & 0x10) ? VFS_TYPE_DIRECTORY : VFS_TYPE_FILE;
            found = true;
            break;
        }

        valid_idx++;
    }

    kfree(cluster_buf);
    return found;
}

static vfs_node_t *fat32_finddir(vfs_node_t *dir, const char *name) {
    if (!dir || !name || !dir->super || !dir->super->private_data) {
        return NULL;
    }

    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    u32 cluster = (u32)(uintptr_t)dir->fs_data;

    u32 buf_size = fs->cluster_size_bytes;
    u8 *cluster_buf = (u8 *)kmalloc(buf_size);
    if (!cluster_buf) {
        return NULL;
    }

    u64 lba = fat32_cluster_to_lba(fs, cluster);
    if (!block_read(dir->super->dev, lba, fs->sectors_per_cluster, cluster_buf)) {
        kfree(cluster_buf);
        return NULL;
    }

    u32 entry_count = buf_size / sizeof(fat32_on_disk_entry_t);
    fat32_on_disk_entry_t *entries = (fat32_on_disk_entry_t *)cluster_buf;
    vfs_node_t *node = NULL;

    for (u32 i = 0; i < entry_count; i++) {
        u8 first_byte = (u8)entries[i].name[0];

        if (first_byte == 0x00) {
            break;
        }

        if (first_byte == 0xE5) {
            continue;
        }

        /* Skip LFN (0x0F) and Volume Label (0x08) */
        if ((entries[i].attr & 0x0F) == 0x0F || (entries[i].attr & 0x08)) {
            continue;
        }

        if (fat32_match_name(entries[i].name, name)) {
            node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
            if (node) {
                u32 start_cluster = ((u32)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
                node->inode = start_cluster;
                node->type = (entries[i].attr & 0x10) ? VFS_TYPE_DIRECTORY : VFS_TYPE_FILE;
                node->size = entries[i].file_size;
                node->ref_count = 1;
                node->super = dir->super;
                node->fs_data = (void *)(uintptr_t)start_cluster;
                node->ops = dir->ops;
            }
            break;
        }
    }

    kfree(cluster_buf);
    return node;
}

static u32 fat32_get_next_cluster(vfs_super_t *sb, fat32_fs_t *fs, u32 current_cluster) {
    if (!sb || !fs || current_cluster < 2) {
        return FAT32_CLUSTER_ERR;
    }

    u32 fat_offset = current_cluster * 4;
    u32 fat_sector = fs->fat_start_lba + (fat_offset / fs->bytes_per_sector);
    u32 entry_offset = fat_offset % fs->bytes_per_sector;

    u8 *sector_buf = (u8 *)kmalloc(fs->bytes_per_sector);
    if (!sector_buf) {
        return FAT32_CLUSTER_ERR;
    }

    if (!block_read(sb->dev, fat_sector, 1, sector_buf)) {
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

static bool fat32_set_next_cluster(vfs_super_t *sb, fat32_fs_t *fs, u32 cluster, u32 val) {
    if (!sb || !fs || cluster < 2) return false;

    u32 fat_offset = cluster * 4;
    u32 fat_sector = fs->fat_start_lba + (fat_offset / fs->bytes_per_sector);
    u32 entry_offset = fat_offset % fs->bytes_per_sector;

    u8 *sector_buf = (u8 *)kmalloc(fs->bytes_per_sector);
    if (!sector_buf) return false;

    if (!block_read(sb->dev, fat_sector, 1, sector_buf)) {
        kfree(sector_buf);
        return false;
    }

    u32 *entry_ptr = (u32 *)(sector_buf + entry_offset);
    *entry_ptr = (*entry_ptr & 0xF0000000) | (val & 0x0FFFFFFF);

    for (u32 fat_idx = 0; fat_idx < fs->fat_count; fat_idx++) {
        u32 target_sector = fat_sector + (fat_idx * fs->fat_size_sectors);
        if (!block_write(sb->dev, target_sector, 1, sector_buf)) {
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

    bool ok = block_write(sb->dev, lba, fs->sectors_per_cluster, zero_buf);
    kfree(zero_buf);
    return ok;
}

u32 fat32_alloc_cluster(vfs_super_t *sb) {
    if (!sb || !sb->private_data) return FAT32_CLUSTER_ERR;
    fat32_fs_t *fs = (fat32_fs_t *)sb->private_data;

    u32 start = fs->next_free_cluster;
    if (start < 2 || start >= fs->total_clusters) {
        start = 2;
    }

    u32 c = start;
    do {
        u32 raw_val = fat32_get_next_cluster(sb, fs, c);
        if (raw_val == 0x00000000) {
            if (fat32_set_next_cluster(sb, fs, c, 0x0FFFFFFF)) {
                fat32_zero_cluster(sb, fs, c);
                fs->next_free_cluster = c + 1;
                if (fs->next_free_cluster >= fs->total_clusters) {
                    fs->next_free_cluster = 2;
                }
                return c;
            }
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
            fat32_set_next_cluster(sb, fs, new_cluster, 0x00000000);
            return FAT32_CLUSTER_ERR;
        }
    }

    return new_cluster;
}

bool fat32_free_chain(vfs_super_t *sb, u32 start_cluster) {
    if (!sb || !sb->private_data || start_cluster < 2) return false;
    fat32_fs_t *fs = (fat32_fs_t *)sb->private_data;

    u32 curr = start_cluster;
    while (curr >= 2 && curr < FAT32_EOC_MIN && curr != FAT32_CLUSTER_ERR) {
        u32 next = fat32_get_next_cluster(sb, fs, curr);
        fat32_set_next_cluster(sb, fs, curr, 0x00000000);
        curr = next;
    }

    return true;
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

    if (offset >= node->size) {
        return 0;
    }

    u64 avail = node->size - offset;
    if (size > avail) {
        size = avail;
    }

    fat32_fs_t *fs = (fat32_fs_t *)node->super->private_data;
    u32 cluster_size = fs->cluster_size_bytes;

    u32 curr_cluster = (u32)(uintptr_t)node->fs_data;
    u64 skip_clusters = offset / cluster_size;
    u64 offset_in_cluster = offset % cluster_size;

    /* Advance to starting cluster for requested offset */
    for (u64 i = 0; i < skip_clusters; i++) {
        curr_cluster = fat32_get_next_cluster(node->super, fs, curr_cluster);
        if (curr_cluster >= FAT32_EOC_MIN || curr_cluster == FAT32_CLUSTER_ERR) {
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

    while (bytes_remaining > 0 && curr_cluster < FAT32_EOC_MIN && curr_cluster != FAT32_CLUSTER_ERR) {
        u64 lba = fat32_cluster_to_lba(fs, curr_cluster);
        if (!block_read(node->super->dev, lba, fs->sectors_per_cluster, cluster_buf)) {
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

static void fat32_update_dirent_size(vfs_node_t *node) {
    if (!node || !node->super || !node->super->private_data) return;
    fat32_fs_t *fs = (fat32_fs_t *)node->super->private_data;

    u32 target_cluster = (u32)(uintptr_t)node->fs_data;
    u32 root_cluster = fs->root_cluster;

    u32 buf_size = fs->cluster_size_bytes;
    u8 *cluster_buf = (u8 *)kmalloc(buf_size);
    if (!cluster_buf) return;

    u64 lba = fat32_cluster_to_lba(fs, root_cluster);
    if (block_read(node->super->dev, lba, fs->sectors_per_cluster, cluster_buf)) {
        fat32_on_disk_entry_t *entries = (fat32_on_disk_entry_t *)cluster_buf;
        u32 count = buf_size / sizeof(fat32_on_disk_entry_t);
        bool updated = false;

        for (u32 i = 0; i < count; i++) {
            if ((u8)entries[i].name[0] == 0x00) break;
            if ((u8)entries[i].name[0] == 0xE5) continue;
            if ((entries[i].attr & 0x0F) == 0x0F || (entries[i].attr & 0x08)) continue;

            u32 entry_cluster = ((u32)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
            if (entry_cluster == target_cluster) {
                entries[i].file_size = (u32)node->size;
                updated = true;
                break;
            }
        }

        if (updated) {
            block_write(node->super->dev, lba, fs->sectors_per_cluster, cluster_buf);
        }
    }

    kfree(cluster_buf);
}

static u64 fat32_write(vfs_node_t *node, u64 offset, u64 size, const void *buffer) {
    if (!node || !buffer || node->type != VFS_TYPE_FILE || !node->super || !node->super->private_data) {
        return 0;
    }

    fat32_fs_t *fs = (fat32_fs_t *)node->super->private_data;
    u32 cluster_size = fs->cluster_size_bytes;

    /* Count allocated clusters in existing cluster chain */
    u32 chain_clusters = 0;
    u32 c = (u32)(uintptr_t)node->fs_data;
    while (c < FAT32_EOC_MIN && c != FAT32_CLUSTER_ERR) {
        chain_clusters++;
        c = fat32_get_next_cluster(node->super, fs, c);
    }

    u64 max_capacity = (u64)chain_clusters * cluster_size;
    if (offset >= max_capacity) {
        return 0; // Exceeds allocated cluster chain capacity
    }

    u64 avail_capacity = max_capacity - offset;
    if (size > avail_capacity) {
        size = avail_capacity;
    }

    if (size == 0) {
        return 0;
    }

    u32 curr_cluster = (u32)(uintptr_t)node->fs_data;
    u64 skip_clusters = offset / cluster_size;
    u64 offset_in_cluster = offset % cluster_size;

    /* Advance to starting cluster for requested offset */
    for (u64 i = 0; i < skip_clusters; i++) {
        curr_cluster = fat32_get_next_cluster(node->super, fs, curr_cluster);
        if (curr_cluster >= FAT32_EOC_MIN || curr_cluster == FAT32_CLUSTER_ERR) {
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

    while (bytes_remaining > 0 && curr_cluster < FAT32_EOC_MIN && curr_cluster != FAT32_CLUSTER_ERR) {
        u64 lba = fat32_cluster_to_lba(fs, curr_cluster);

        /* Read existing cluster for partial block updates */
        if (!block_read(node->super->dev, lba, fs->sectors_per_cluster, cluster_buf)) {
            break;
        }

        u64 chunk_avail = cluster_size - offset_in_cluster;
        u64 chunk_size = (bytes_remaining < chunk_avail) ? bytes_remaining : chunk_avail;

        memcpy(cluster_buf + offset_in_cluster, in_ptr, chunk_size);

        if (!block_write(node->super->dev, lba, fs->sectors_per_cluster, cluster_buf)) {
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
        node->size = offset + bytes_written;
        fat32_update_dirent_size(node);
    }

    return bytes_written;
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

static int fat32_create(vfs_node_t *dir, const char *name, vfs_node_t **out_node) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !out_node || !dir->super || !dir->super->private_data) {
        return VFS_ERR_INVALID_PARAM;
    }

    vfs_node_t *existing = fat32_finddir(dir, name);
    if (existing) {
        kfree(existing);
        return VFS_ERR_INVALID_PARAM;
    }

    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    u32 dir_cluster = (u32)(uintptr_t)dir->fs_data;

    u32 new_cluster = fat32_alloc_cluster(dir->super);
    if (new_cluster == FAT32_CLUSTER_ERR) {
        return VFS_ERR_NO_MEM;
    }

    u32 cluster_size = fs->cluster_size_bytes;
    u8 *cluster_buf = (u8 *)kmalloc(cluster_size);
    if (!cluster_buf) {
        fat32_set_next_cluster(dir->super, fs, new_cluster, 0x00000000);
        return VFS_ERR_NO_MEM;
    }

    u64 lba = fat32_cluster_to_lba(fs, dir_cluster);
    if (!block_read(dir->super->dev, lba, fs->sectors_per_cluster, cluster_buf)) {
        kfree(cluster_buf);
        fat32_set_next_cluster(dir->super, fs, new_cluster, 0x00000000);
        return VFS_ERR_IO;
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
        fat32_set_next_cluster(dir->super, fs, new_cluster, 0x00000000);
        return VFS_ERR_NO_MEM;
    }

    memset(&entries[target_idx], 0, sizeof(fat32_on_disk_entry_t));
    fat32_create_83_name(name, entries[target_idx].name);
    entries[target_idx].attr = 0x20;
    entries[target_idx].first_cluster_high = (u16)(new_cluster >> 16);
    entries[target_idx].first_cluster_low  = (u16)(new_cluster & 0xFFFF);
    entries[target_idx].file_size = 0;

    if (!block_write(dir->super->dev, lba, fs->sectors_per_cluster, cluster_buf)) {
        kfree(cluster_buf);
        fat32_set_next_cluster(dir->super, fs, new_cluster, 0x00000000);
        return VFS_ERR_IO;
    }

    kfree(cluster_buf);

    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) {
        return VFS_ERR_NO_MEM;
    }

    node->inode = new_cluster;
    node->type = VFS_TYPE_FILE;
    node->size = 0;
    node->ref_count = 1;
    node->super = dir->super;
    node->fs_data = (void *)(uintptr_t)new_cluster;
    node->ops = dir->ops;

    *out_node = node;
    return VFS_OK;
}

static int fat32_mkdir(vfs_node_t *dir, const char *name, vfs_node_t **out_node) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !out_node || !dir->super || !dir->super->private_data) {
        return VFS_ERR_INVALID_PARAM;
    }

    vfs_node_t *existing = fat32_finddir(dir, name);
    if (existing) {
        kfree(existing);
        return VFS_ERR_INVALID_PARAM;
    }

    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    u32 parent_cluster = (u32)(uintptr_t)dir->fs_data;

    u32 new_cluster = fat32_alloc_cluster(dir->super);
    if (new_cluster == FAT32_CLUSTER_ERR) {
        return VFS_ERR_NO_MEM;
    }

    u32 cluster_size = fs->cluster_size_bytes;
    u8 *new_dir_buf = (u8 *)kmalloc(cluster_size);
    if (!new_dir_buf) {
        fat32_set_next_cluster(dir->super, fs, new_cluster, 0x00000000);
        return VFS_ERR_NO_MEM;
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
    if (!block_write(dir->super->dev, new_dir_lba, fs->sectors_per_cluster, new_dir_buf)) {
        kfree(new_dir_buf);
        fat32_set_next_cluster(dir->super, fs, new_cluster, 0x00000000);
        return VFS_ERR_IO;
    }
    kfree(new_dir_buf);

    /* Insert entry in parent directory */
    u8 *parent_buf = (u8 *)kmalloc(cluster_size);
    if (!parent_buf) {
        fat32_set_next_cluster(dir->super, fs, new_cluster, 0x00000000);
        return VFS_ERR_NO_MEM;
    }

    u64 parent_lba = fat32_cluster_to_lba(fs, parent_cluster);
    if (!block_read(dir->super->dev, parent_lba, fs->sectors_per_cluster, parent_buf)) {
        kfree(parent_buf);
        fat32_set_next_cluster(dir->super, fs, new_cluster, 0x00000000);
        return VFS_ERR_IO;
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
        fat32_set_next_cluster(dir->super, fs, new_cluster, 0x00000000);
        return VFS_ERR_NO_MEM;
    }

    memset(&parent_entries[target_idx], 0, sizeof(fat32_on_disk_entry_t));
    fat32_create_83_name(name, parent_entries[target_idx].name);
    parent_entries[target_idx].attr = 0x10;
    parent_entries[target_idx].first_cluster_high = (u16)(new_cluster >> 16);
    parent_entries[target_idx].first_cluster_low  = (u16)(new_cluster & 0xFFFF);
    parent_entries[target_idx].file_size = 0;

    if (!block_write(dir->super->dev, parent_lba, fs->sectors_per_cluster, parent_buf)) {
        kfree(parent_buf);
        fat32_set_next_cluster(dir->super, fs, new_cluster, 0x00000000);
        return VFS_ERR_IO;
    }

    kfree(parent_buf);

    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) {
        return VFS_ERR_NO_MEM;
    }

    node->inode = new_cluster;
    node->type = VFS_TYPE_DIRECTORY;
    node->size = 0;
    node->ref_count = 1;
    node->super = dir->super;
    node->fs_data = (void *)(uintptr_t)new_cluster;
    node->ops = dir->ops;

    *out_node = node;
    return VFS_OK;
}

static int fat32_unlink(vfs_node_t *dir, const char *name) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !dir->super || !dir->super->private_data) {
        return VFS_ERR_INVALID_PARAM;
    }

    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    u32 parent_cluster = (u32)(uintptr_t)dir->fs_data;

    u32 cluster_size = fs->cluster_size_bytes;
    u8 *parent_buf = (u8 *)kmalloc(cluster_size);
    if (!parent_buf) {
        return VFS_ERR_NO_MEM;
    }

    u64 parent_lba = fat32_cluster_to_lba(fs, parent_cluster);
    if (!block_read(dir->super->dev, parent_lba, fs->sectors_per_cluster, parent_buf)) {
        kfree(parent_buf);
        return VFS_ERR_IO;
    }

    fat32_on_disk_entry_t *entries = (fat32_on_disk_entry_t *)parent_buf;
    u32 count = cluster_size / sizeof(fat32_on_disk_entry_t);
    int target_idx = -1;
    u32 target_cluster = 0;

    for (u32 i = 0; i < count; i++) {
        u8 first = (u8)entries[i].name[0];
        if (first == 0x00) break;
        if (first == 0xE5) continue;
        if ((entries[i].attr & 0x0F) == 0x0F || (entries[i].attr & 0x08)) continue;

        if (fat32_match_name(entries[i].name, name)) {
            if (entries[i].attr & 0x10) {
                kfree(parent_buf);
                return VFS_ERR_INVALID_PARAM;
            }
            target_idx = i;
            target_cluster = ((u32)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
            break;
        }
    }

    if (target_idx == -1) {
        kfree(parent_buf);
        return VFS_ERR_NOT_FOUND;
    }

    entries[target_idx].name[0] = (char)0xE5;
    if (!block_write(dir->super->dev, parent_lba, fs->sectors_per_cluster, parent_buf)) {
        kfree(parent_buf);
        return VFS_ERR_IO;
    }
    kfree(parent_buf);

    fat32_free_chain(dir->super, target_cluster);
    return VFS_OK;
}

static int fat32_rmdir(vfs_node_t *dir, const char *name) {
    if (!dir || !name || dir->type != VFS_TYPE_DIRECTORY || !dir->super || !dir->super->private_data) {
        return VFS_ERR_INVALID_PARAM;
    }

    fat32_fs_t *fs = (fat32_fs_t *)dir->super->private_data;
    u32 parent_cluster = (u32)(uintptr_t)dir->fs_data;

    u32 cluster_size = fs->cluster_size_bytes;
    u8 *parent_buf = (u8 *)kmalloc(cluster_size);
    if (!parent_buf) {
        return VFS_ERR_NO_MEM;
    }

    u64 parent_lba = fat32_cluster_to_lba(fs, parent_cluster);
    if (!block_read(dir->super->dev, parent_lba, fs->sectors_per_cluster, parent_buf)) {
        kfree(parent_buf);
        return VFS_ERR_IO;
    }

    fat32_on_disk_entry_t *entries = (fat32_on_disk_entry_t *)parent_buf;
    u32 count = cluster_size / sizeof(fat32_on_disk_entry_t);
    int target_idx = -1;
    u32 target_cluster = 0;

    for (u32 i = 0; i < count; i++) {
        u8 first = (u8)entries[i].name[0];
        if (first == 0x00) break;
        if (first == 0xE5) continue;
        if ((entries[i].attr & 0x0F) == 0x0F || (entries[i].attr & 0x08)) continue;

        if (fat32_match_name(entries[i].name, name)) {
            if (!(entries[i].attr & 0x10)) {
                kfree(parent_buf);
                return VFS_ERR_INVALID_PARAM;
            }
            target_idx = i;
            target_cluster = ((u32)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
            break;
        }
    }

    if (target_idx == -1) {
        kfree(parent_buf);
        return VFS_ERR_NOT_FOUND;
    }

    u8 *dir_buf = (u8 *)kmalloc(cluster_size);
    if (!dir_buf) {
        kfree(parent_buf);
        return VFS_ERR_NO_MEM;
    }

    u64 target_lba = fat32_cluster_to_lba(fs, target_cluster);
    if (!block_read(dir->super->dev, target_lba, fs->sectors_per_cluster, dir_buf)) {
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
        return VFS_ERR_INVALID_PARAM;
    }

    entries[target_idx].name[0] = (char)0xE5;
    if (!block_write(dir->super->dev, parent_lba, fs->sectors_per_cluster, parent_buf)) {
        kfree(parent_buf);
        return VFS_ERR_IO;
    }
    kfree(parent_buf);

    fat32_free_chain(dir->super, target_cluster);
    return VFS_OK;
}

static const vfs_ops_t fat32_node_ops = {
    .read = fat32_read,
    .write = fat32_write,
    .finddir = fat32_finddir,
    .readdir = fat32_readdir,
    .create = fat32_create,
    .mkdir = fat32_mkdir,
    .unlink = fat32_unlink,
    .rmdir = fat32_rmdir,
};

static int fat32_unmount(vfs_super_t *sb) {
    if (!sb) return VFS_ERR_INVALID_PARAM;
    return VFS_OK;
}

static const vfs_super_ops_t fat32_super_ops = {
    .unmount = fat32_unmount,
    .sync = NULL,
};

static bool fat32_probe(block_device_t *dev) {
    if (!dev) return false;

    u8 sector_buf[512];
    if (!block_read(dev, 0, 1, sector_buf)) {
        return false;
    }

    if (sector_buf[510] != 0x55 || sector_buf[511] != 0xAA) {
        return false;
    }

    fat32_bpb_t *bpb = (fat32_bpb_t *)sector_buf;

    if (bpb->bytes_per_sector != 512 && bpb->bytes_per_sector != 1024 &&
        bpb->bytes_per_sector != 2048 && bpb->bytes_per_sector != 4096) {
        return false;
    }

    if (bpb->sectors_per_cluster == 0 || (bpb->sectors_per_cluster & (bpb->sectors_per_cluster - 1)) != 0) {
        return false;
    }

    if (bpb->sectors_per_fat_16 != 0 || bpb->sectors_per_fat_32 == 0) {
        return false;
    }

    if (bpb->root_cluster < 2) {
        return false;
    }

    return true;
}

static int fat32_mount(vfs_fs_type_t *fs_type, block_device_t *dev, vfs_super_t **out_sb) {
    if (!fs_type || !dev || !out_sb) {
        return VFS_ERR_INVALID_PARAM;
    }

    if (!fat32_probe(dev)) {
        return VFS_ERR_BAD_FORMAT;
    }

    u8 sector_buf[512];
    if (!block_read(dev, 0, 1, sector_buf)) {
        return VFS_ERR_IO;
    }

    fat32_bpb_t *bpb = (fat32_bpb_t *)sector_buf;

    vfs_super_t *sb = (vfs_super_t *)kmalloc(sizeof(vfs_super_t));
    fat32_fs_t *fs = (fat32_fs_t *)kmalloc(sizeof(fat32_fs_t));
    vfs_node_t *root_node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));

    if (!sb || !fs || !root_node) {
        return VFS_ERR_NO_MEM;
    }

    memcpy(&fs->bpb, bpb, sizeof(fat32_bpb_t));

    fs->bytes_per_sector = bpb->bytes_per_sector;
    fs->sectors_per_cluster = bpb->sectors_per_cluster;
    fs->cluster_size_bytes = fs->bytes_per_sector * fs->sectors_per_cluster;
    fs->reserved_sector_count = bpb->reserved_sector_count;
    fs->fat_count = bpb->fat_count;
    fs->fat_size_sectors = bpb->sectors_per_fat_32;

    fs->fat_start_lba = fs->reserved_sector_count;
    fs->data_start_lba = fs->fat_start_lba + (fs->fat_count * fs->fat_size_sectors);
    fs->root_cluster = bpb->root_cluster;

    u32 total_sectors = bpb->total_sectors_32 ? bpb->total_sectors_32 : bpb->total_sectors_16;
    u32 data_sectors = (total_sectors > fs->data_start_lba) ? (total_sectors - fs->data_start_lba) : 0;
    fs->total_clusters = (fs->sectors_per_cluster > 0) ? ((data_sectors / fs->sectors_per_cluster) + 2) : 0;
    fs->next_free_cluster = 2;

    root_node->inode = (u64)fs->root_cluster;
    root_node->type = VFS_TYPE_DIRECTORY;
    root_node->size = 0;
    root_node->ref_count = 1;
    root_node->super = sb;
    root_node->fs_data = (void *)(uintptr_t)fs->root_cluster;
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
    .mount = fat32_mount,
    .next = NULL,
};

int fat32_init(void) {
    return vfs_register_fs(&fat32_fs_type);
}
