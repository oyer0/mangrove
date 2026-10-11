/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production FAT32 functions with a controlled mounted block device. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../kernel/src/storage/fat32.c"

static u8 media[128][512];
static block_device_t device;
static bool online;
static bool fail_write;
static unsigned writes;
static unsigned fat_reads, directory_reads;
static unsigned allocations, fail_allocation;
static usize live_requested, peak_requested;
static u64 fail_read_lba;
static FILE *image_file;
static u64 image_sectors;
static unsigned image_reads;
static unsigned remove_after_reads;
static unsigned replace_after_reads;
static fat32_fs_t fs;
static vfs_super_t super;
static vfs_node_t root;
static fat32_node_state_t root_state;

void *kmalloc(usize size)
{
    if (++allocations == fail_allocation) return NULL;
    usize *base = (usize *)malloc(size + sizeof(usize));
    if (!base) return NULL;
    *base = size;
    live_requested += size;
    if (live_requested > peak_requested) peak_requested = live_requested;
    return base + 1;
}
void kfree(void *ptr)
{
    if (!ptr) return;
    usize *base = (usize *)ptr - 1;
    live_requested -= *base;
    free(base);
}
int vfs_register_fs(vfs_fs_type_t *type) { (void)type; return VFS_OK; }
bool vfs_current_uid(u32 *out) { *out = 7; return true; }
void vfs_node_set_security(vfs_node_t *node, u32 owner, u32 permissions)
{ node->owner_uid = owner; node->permissions = permissions; }
void vfs_node_register(vfs_node_t *node)
{ assert(node->super && node->super->private_data); }
void vfs_nodes_update_size(vfs_super_t *sb, u64 inode, u64 size)
{
    assert(sb == &super);
    for (fat32_node_state_t *state = fs.nodes; state; state = state->next)
        if (state->node->inode == inode) state->node->size = size;
}
bool block_read_for_id(block_device_t *dev, u64 id, u64 lba, u32 count,
                       void *buffer)
{
    if (image_file) {
        if (!online || dev != &device || id != device.id ||
            count != 1U || lba >= image_sectors ||
            fseeko(image_file, (off_t)(lba * 512U), SEEK_SET) != 0)
            return false;
        if (fread(buffer, 1U, 512U, image_file) != 512U) return false;
        image_reads++;
        return true;
    }
    if (!online || dev != &device || id != device.id ||
        lba >= 128 || count != 1 || lba == fail_read_lba) return false;
    memcpy(buffer, media[lba], 512);
    if (lba == 1U) fat_reads++;
    else directory_reads++;
    if (remove_after_reads && --remove_after_reads == 0U) online = false;
    if (replace_after_reads && --replace_after_reads == 0U) device.id++;
    return true;
}
bool block_write_for_id(block_device_t *dev, u64 id, u64 lba, u32 count,
                        const void *buffer)
{
    if (image_file) return false; /* disposable image is opened read-only */
    if (!online || dev != &device || id != device.id ||
        lba >= 128 || count != 1) return false;
    writes++;
    if (fail_write) {
        memcpy(media[lba], buffer, 128); /* ambiguous sector write */
        return false;
    }
    memcpy(media[lba], buffer, 512);
    return true;
}
bool block_read(block_device_t *dev, u64 lba, u32 count, void *buffer)
{ return block_read_for_id(dev, dev->id, lba, count, buffer); }

static void entry(u32 index, const char *short_name, u32 cluster, u32 size)
{
    fat32_on_disk_entry_t *items = (fat32_on_disk_entry_t *)media[10];
    memset(&items[index], 0, sizeof(items[index]));
    memcpy(items[index].name, short_name, 11);
    items[index].attr = 0x20;
    items[index].first_cluster_low = (u16)cluster;
    items[index].file_size = size;
}

static void entry_at(u32 directory_cluster, u32 index, const char *name,
                     u32 first, u32 size, u8 attributes)
{
    fat32_on_disk_entry_t *items =
        (fat32_on_disk_entry_t *)media[10 + directory_cluster - 2U];
    memset(&items[index], 0, sizeof(items[index]));
    memcpy(items[index].name, name, 11);
    items[index].first_cluster_low = (u16)first;
    items[index].file_size = size;
    items[index].attr = attributes;
}

static void setup(void)
{
    memset(media, 0, sizeof(media));
    memset(&fs, 0, sizeof(fs));
    memset(&super, 0, sizeof(super));
    memset(&root, 0, sizeof(root));
    memset(&root_state, 0, sizeof(root_state));
    device.id = 87;
    online = true;
    writes = 0;
    fat_reads = directory_reads = 0;
    fail_write = false;
    allocations = fail_allocation = 0;
    live_requested = peak_requested = 0;
    fail_read_lba = ~(u64)0;
    remove_after_reads = replace_after_reads = 0;
    fs.device_id = device.id;
    fs.bytes_per_sector = 512;
    fs.sectors_per_cluster = 1;
    fs.cluster_size_bytes = 512;
    fs.fat_start_lba = 1;
    fs.fat_count = 1;
    fs.fat_size_sectors = 1;
    fs.data_start_lba = 10;
    fs.root_cluster = 2;
    fs.total_clusters = 64;
    fs.next_free_cluster = 4;
    fs.version_confident = true;
    fs.next_inode = 1;
    fs.nodes = &root_state;
    super.dev = &device;
    super.private_data = &fs;
    root.inode = 1;
    root.type = VFS_TYPE_DIRECTORY;
    root.super = &super;
    root.fs_data = &root_state;
    root.ops = &fat32_node_ops;
    root_state.node = &root;
    root_state.first_cluster = 2;
    root_state.generation = 1;
    root_state.alive = true;
    root_state.version_valid = true;
    ((u32 *)media[1])[2] = FAT32_EOC_MIN;
    ((u32 *)media[1])[3] = FAT32_EOC_MIN;
}

static int sink(void *context, u64 offset, const u8 *data, usize length)
{
    memcpy((u8 *)context + offset, data, length);
    return VFS_OK;
}

static void cleanup(void)
{
    for (fat32_node_state_t *state = fs.nodes; state;) {
        fat32_node_state_t *next = state->next;
        if (state != &root_state) { kfree(state->node); kfree(state); }
        state = next;
    }
}

static void namespace_cases(void)
{
    vfs_dirent_t dirent;
    vfs_node_t *file;
    unsigned before;
    setup();
    ((u32 *)media[1])[2] = 4;
    ((u32 *)media[1])[4] = FAT32_EOC_MIN;
    ((u32 *)media[1])[5] = FAT32_EOC_MIN;
    for (u32 i = 0; i < 16U; i++) {
        char name[12];
        snprintf(name, sizeof(name), "F%02u     TXT", i);
        entry(i, name, 0, 0);
    }
    entry_at(4, 0, "LATE    TXT", 5, 4, 0x20);
    memcpy(media[13], "late", 4);
    file = fat32_finddir(&root, "late.txt");
    assert(file && fat32_readdir(&root, 16, &dirent) &&
           !strcmp(dirent.name, "LATE.TXT") && dirent.inode == file->inode);
    before = writes;
    usize baseline = live_requested;
    peak_requested = baseline;
    fat_reads = directory_reads = 0;
    assert(fat32_ownership_preflight_locked(file) == VFS_OK &&
           writes == before);
    assert(live_requested == baseline);
    printf("FAT32 preflight fixture: %zu requested heap bytes, %u FAT reads, %u directory reads\n",
           (size_t)(peak_requested - baseline), fat_reads, directory_reads);
    vfs_node_t *collision = NULL;
    before = writes;
    assert(fat32_create(&root, "LATE.TXT", &collision) ==
           VFS_ERR_INVALID_PARAM && !collision && writes == before);
    assert(fat32_write(file, 4, 1, "!") == 1 && file->size == 5 &&
           ((fat32_on_disk_entry_t *)media[12])[0].file_size == 5);
    assert(fat32_truncate(file) == VFS_OK && file->size == 0 &&
           fat32_ownership_preflight_locked(file) == VFS_OK);
    vfs_node_t *created = NULL;
    assert(fat32_create(&root, "NEW.TXT", &created) == VFS_OK &&
           created && fat32_finddir(&root, "new.txt") == created);
    vfs_node_t *new_directory = NULL;
    assert(fat32_mkdir(&root, "SUB", &new_directory) == VFS_OK &&
           new_directory && fat32_finddir(&root, "sub") == new_directory);
    assert(fat32_rename(&root, "late.txt", &root, "MOVED.TXT") == VFS_OK);
    assert(fat32_finddir(&root, "moved.txt") == file);
    assert(fat32_unlink(&root, "moved.txt") == VFS_OK);
    assert(fat32_ownership_preflight_locked(created) == VFS_OK);
    cleanup();

    setup();
    ((u32 *)media[1])[6] = FAT32_EOC_MIN;
    ((u32 *)media[1])[7] = FAT32_EOC_MIN;
    entry_at(2, 0, "SUBDIR     ", 6, 0, 0x10);
    entry_at(6, 0, ".          ", 6, 0, 0x10);
    entry_at(6, 1, "..         ", 0, 0, 0x10);
    entry_at(6, 2, "CHILD   TXT", 7, 3, 0x20);
    vfs_node_t *subdir = fat32_finddir(&root, "subdir");
    file = subdir ? fat32_finddir(subdir, "child.txt") : NULL;
    assert(file && fat32_ownership_preflight_locked(file) == VFS_OK);
    before = writes;
    entry_at(2, 1, "ALIAS   TXT", 7, 1, 0x20);
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_BAD_FORMAT &&
           !fs.version_confident && writes == before);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    before = writes;
    entry_at(2, 1, "ALIAS   TXT", 2, 1, 0x20);
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_BAD_FORMAT &&
           writes == before); /* file references the root directory */
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    entry_at(2, 1, "DIRONE     ", 4, 0, 0x10);
    entry_at(2, 2, "DIRTWO     ", 4, 0, 0x10);
    ((u32 *)media[1])[4] = FAT32_EOC_MIN;
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_BAD_FORMAT);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    fat32_lfn_entry_t *prefix =
        (fat32_lfn_entry_t *)&((fat32_on_disk_entry_t *)media[10])[1];
    fat32_lfn_entry_t *invalid =
        (fat32_lfn_entry_t *)&((fat32_on_disk_entry_t *)media[10])[2];
    memset(prefix, 0, sizeof(*prefix));
    prefix->order = 0x41;
    prefix->attr = 0x0f;
    prefix->checksum = 7;
    *invalid = *prefix;
    invalid->order = 1;
    invalid->type = 1; /* invalid continuation after a valid prefix */
    entry_at(2, 3, "OTHER   TXT", 0, 0, 0x20);
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_BAD_FORMAT);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    prefix = (fat32_lfn_entry_t *)&((fat32_on_disk_entry_t *)media[10])[1];
    invalid = (fat32_lfn_entry_t *)&((fat32_on_disk_entry_t *)media[10])[2];
    memset(prefix, 0, sizeof(*prefix));
    prefix->order = 0x41;
    prefix->attr = 0x0f;
    prefix->checksum = 7;
    *invalid = *prefix;
    invalid->order = 1; /* duplicate sequence after completed prefix */
    entry_at(2, 3, "OTHER   TXT", 0, 0, 0x20);
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_BAD_FORMAT);
    cleanup();

    setup();
    ((u32 *)media[1])[2] = 9;
    ((u32 *)media[1])[9] = 4;
    ((u32 *)media[1])[4] = FAT32_EOC_MIN;
    ((u32 *)media[1])[3] = 5;
    ((u32 *)media[1])[5] = 7;
    ((u32 *)media[1])[7] = FAT32_EOC_MIN;
    for (u32 i = 0; i < 16U; i++) {
        ((fat32_on_disk_entry_t *)media[10])[i].name[0] = (char)0xe5;
        ((fat32_on_disk_entry_t *)media[17])[i].name[0] = (char)0xe5;
    }
    entry_at(4, 0, "TARGET  TXT", 3, 1025, 0x20);
    file = fat32_finddir(&root, "target.txt");
    assert(file && fat32_ownership_preflight_locked(file) == VFS_OK);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    ((u32 *)media[1])[3] = 4;
    ((u32 *)media[1])[4] = FAT32_EOC_MIN;
    entry_at(2, 1, "TAIL    TXT", 4, 1, 0x20);
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_BAD_FORMAT);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    ((u32 *)media[1])[3] = 0;
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_BAD_FORMAT);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1025, 0x20);
    file = fat32_finddir(&root, "target.txt");
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_BAD_FORMAT);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    ((u32 *)media[1])[2] = 4;
    ((u32 *)media[1])[4] = 2;
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_BAD_FORMAT);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    fail_read_lba = 1;
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_IO &&
           fs.version_confident);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    fail_read_lba = 10;
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_IO &&
           fs.version_confident);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    fs.fat_count = 2;
    memcpy(media[2], media[1], 512);
    ((u32 *)media[2])[3] = 0; /* mirrored FATs disagree */
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_BAD_FORMAT);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    fs.fat_count = 2;
    fs.bpb.ext_flags = 0x81U; /* inactive FAT 0 may differ */
    memcpy(media[2], media[1], 512);
    ((u32 *)media[1])[3] = 0;
    assert(fat32_ownership_preflight_locked(file) == VFS_OK);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    entry_at(2, 1, "DELETED TXT", 3, 1, 0x20);
    ((fat32_on_disk_entry_t *)media[10])[1].name[0] = (char)0xe5;
    assert(fat32_ownership_preflight_locked(file) == VFS_OK);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    fat32_lfn_entry_t *bad_lfn =
        (fat32_lfn_entry_t *)&((fat32_on_disk_entry_t *)media[10])[1];
    bad_lfn->order = 0x41;
    bad_lfn->attr = 0x0f;
    bad_lfn->first_cluster = 3;
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_BAD_FORMAT);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    fs.total_clusters = FAT32_SCAN_MAX_BITMAP_BYTES * 8U + 1U;
    assert(fat32_ownership_preflight_locked(file) ==
           VFS_ERR_BUFFER_TOO_SMALL && fs.version_confident);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    ((u32 *)media[1])[2] = 0; /* referenced root directory is free */
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_BAD_FORMAT);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    ((u32 *)media[1])[2] = 70; /* outside this volume */
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_BAD_FORMAT);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    remove_after_reads = 1;
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_IO &&
           fs.version_confident);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    replace_after_reads = 1;
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_IO &&
           fs.version_confident);
    cleanup();

    setup();
    for (u32 i = 0; i < 16U; i++)
        entry_at(2, i, "FULL    TXT", 0, 0, 0x20);
    before = writes;
    vfs_node_t *unpublished = NULL;
    assert(fat32_create(&root, "NEW.TXT", &unpublished) == VFS_ERR_NO_SPACE &&
           !unpublished && writes == before);
    cleanup();

    setup();
    ((u32 *)media[1])[6] = 7;
    ((u32 *)media[1])[7] = FAT32_EOC_MIN;
    ((u32 *)media[1])[8] = FAT32_EOC_MIN;
    entry_at(2, 0, "SUBDIR     ", 6, 0, 0x10);
    entry_at(6, 0, ".          ", 6, 0, 0x10);
    entry_at(6, 1, "..         ", 0, 0, 0x10);
    for (u32 i = 2; i < 16U; i++)
        ((fat32_on_disk_entry_t *)media[14])[i].name[0] = (char)0xe5;
    entry_at(7, 0, "CHILD   TXT", 8, 1, 0x20);
    assert(fat32_rmdir(&root, "subdir") == VFS_ERR_NOT_EMPTY);
    cleanup();

    setup();
    ((u32 *)media[1])[6] = 7;
    ((u32 *)media[1])[7] = FAT32_EOC_MIN;
    entry_at(2, 0, "SUBDIR     ", 6, 0, 0x10);
    entry_at(6, 0, ".          ", 6, 0, 0x10);
    entry_at(6, 1, "..         ", 0, 0, 0x10);
    assert(fat32_rmdir(&root, "subdir") == VFS_OK &&
           ((u32 *)media[1])[6] == 0 && ((u32 *)media[1])[7] == 0);
    cleanup();

    setup();
    ((u32 *)media[1])[2] = 4;
    ((u32 *)media[1])[4] = FAT32_EOC_MIN;
    ((u32 *)media[1])[5] = FAT32_EOC_MIN;
    for (u32 i = 0; i < 15U; i++) {
        char name[12];
        snprintf(name, sizeof(name), "F%02u     TXT", i);
        entry_at(2, i, name, 0, 0, 0x20);
    }
    fat32_lfn_entry_t *lfn =
        (fat32_lfn_entry_t *)&((fat32_on_disk_entry_t *)media[10])[15];
    memset(lfn, 0xff, sizeof(*lfn));
    lfn->order = 0x41;
    lfn->attr = 0x0f;
    lfn->type = 0;
    lfn->first_cluster = 0;
    lfn->checksum = fat32_lfn_checksum("LONGFI~1TXT");
    const char *long_name = "LongFile.txt";
    u16 units[13] = {0};
    for (u32 i = 0; i < 12U; i++) units[i] = (u8)long_name[i];
    for (u32 i = 0; i < 5U; i++) lfn->name1[i] = units[i];
    for (u32 i = 0; i < 6U; i++) lfn->name2[i] = units[5U + i];
    for (u32 i = 0; i < 2U; i++) lfn->name3[i] = units[11U + i];
    entry_at(4, 0, "LONGFI~1TXT", 5, 1, 0x20);
    file = fat32_finddir(&root, long_name);
    assert(file && fat32_readdir(&root, 15, &dirent) &&
           !strcmp(dirent.name, long_name) &&
           fat32_ownership_preflight_locked(file) == VFS_OK);
    assert(fat32_finddir(&root, "LONGFI~1.TXT") == file);
    before = writes;
    vfs_node_t *alias_collision = NULL;
    assert(fat32_create(&root, "LONGFI~1.TXT", &alias_collision) ==
           VFS_ERR_INVALID_PARAM && !alias_collision && writes == before);
    assert(fat32_unlink(&root, long_name) == VFS_OK);
    vfs_node_t *after_lfn_delete = NULL;
    assert(fat32_create(&root, "NEW.TXT", &after_lfn_delete) == VFS_OK &&
           after_lfn_delete &&
           fat32_state(after_lfn_delete)->entry_index == 1U &&
           fat32_ownership_preflight_locked(after_lfn_delete) == VFS_OK);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    fail_allocation = allocations + 1U;
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_NO_MEM &&
           fs.version_confident);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    usize retained = live_requested;
    fail_allocation = allocations + 2U; /* queue after bitmap */
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_NO_MEM &&
           live_requested == retained && fs.version_confident);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    assert(fat32_unlink(&root, "target.txt") == VFS_OK);
    ((u32 *)media[1])[4] = FAT32_EOC_MIN;
    entry_at(2, 0, "REUSED  TXT", 4, 1, 0x20);
    vfs_node_t *reused = fat32_finddir(&root, "reused.txt");
    assert(reused && reused->inode != file->inode &&
           fat32_ownership_preflight_locked(file) == VFS_ERR_NOT_FOUND &&
           fat32_ownership_preflight_locked(reused) == VFS_OK);
    cleanup();

    setup();
    entry_at(2, 0, "TARGET  TXT", 3, 1, 0x20);
    file = fat32_finddir(&root, "target.txt");
    device.id++;
    assert(fat32_ownership_preflight_locked(file) == VFS_ERR_IO);
    cleanup();
}

static void disposable_image_case(const char *path)
{
    image_file = fopen(path, "rb");
    assert(image_file && fseeko(image_file, 0, SEEK_END) == 0);
    off_t bytes = ftello(image_file);
    assert(bytes > 0 && bytes % 512 == 0);
    image_sectors = (u64)bytes / 512U;
    device.id = 901;
    device.sector_size = 512;
    device.sector_count = image_sectors;
    online = true;
    assert(fat32_probe_for_id(&device, device.id));
    vfs_super_t *mounted = NULL;
    assert(fat32_mount(&fat32_fs_type, &device, &mounted, true) == VFS_OK);
    vfs_node_t *directory = fat32_finddir(mounted->root_node, "MANY");
    vfs_node_t *last = directory ? fat32_finddir(directory, "F48.TXT") : NULL;
    assert(last);
    vfs_dirent_t dirent;
    assert(fat32_readdir(directory, 47, &dirent) &&
           !strcmp(dirent.name, "F48.TXT"));
    u8 contents[8];
    assert(fat32_read(last, 0, sizeof(contents), contents) == sizeof(contents));
    assert(!memcmp(contents, "file-48\n", sizeof(contents)));
    usize existing = live_requested;
    peak_requested = existing;
    image_reads = 0;
    assert(fat32_ownership_preflight_locked(last) == VFS_OK &&
           live_requested == existing);
    printf("FAT32 disposable preflight: %zu requested heap bytes, %u sector reads\n",
           (size_t)(peak_requested - existing), image_reads);
    fat32_fs_t *mounted_fs = (fat32_fs_t *)mounted->private_data;
    vfs_node_t *nodes[4];
    u32 node_count = 0;
    for (fat32_node_state_t *state = mounted_fs->nodes; state;
         state = state->next) {
        assert(node_count < 4U);
        nodes[node_count++] = state->node;
    }
    assert(fat32_unmount(mounted) == VFS_OK);
    for (u32 i = 0; i < node_count; i++) kfree(nodes[i]);
    kfree(mounted);
    fclose(image_file);
    image_file = NULL;
    puts("FAT32 disposable image: production mount, later-cluster lookup, read and ownership passed");
}

int main(int argc, char **argv)
{
    setup();
    entry(0, "A       TXT", 0, 0);
    entry(1, "B       TXT", 0, 0);
    entry(2, "DATA    TXT", 3, 3);
    memcpy(media[11], "old", 3);
    vfs_node_t *a = fat32_finddir(&root, "a.txt");
    vfs_node_t *b = fat32_finddir(&root, "b.txt");
    vfs_node_t *file = fat32_finddir(&root, "data.txt");
    assert(a && b && file && a != b && a->inode != b->inode);
    assert(fat32_finddir(&root, "A.TXT") == a);
    assert(fat32_finddir(&root, "DATA.TXT") == file);
    vfs_file_facts_t facts;
    assert(fat32_file_facts(a, &facts) == VFS_OK && facts.generation == 1);
    assert(fat32_rename(&root, "a.txt", &root, "c.txt") == VFS_OK);
    assert(fat32_finddir(&root, "c.txt") == a);
    assert(fat32_file_facts(a, &facts) == VFS_OK && facts.generation == 2);
    assert(fat32_write(file, 3, 1, "!") == 1);
    assert(file->size == 4 && root_state.generation == 2);
    assert(fat32_file_facts(file, &facts) == VFS_OK && facts.generation == 2);
    u8 snapshot[8] = {0};
    mg_file_lines_state_t snap;
    assert(fat32_snapshot_stream(file, sizeof(snapshot), sink, snapshot,
                                 &snap) == VFS_OK);
    assert(snap.identifier == file->inode && snap.revision == 2 &&
           snap.length == 4 && !memcmp(snapshot, "old!", 4));
    assert(fat32_truncate(file) == VFS_OK);
    assert(file->size == 0 && fat32_file_facts(file, &facts) == VFS_OK &&
           facts.generation == 3);
    assert(fat32_unlink(&root, "c.txt") == VFS_OK);
    assert(fat32_file_facts(a, &facts) == VFS_ERR_NOT_FOUND);
    entry(0, "NEW     TXT", 0, 0);
    vfs_node_t *replacement = fat32_finddir(&root, "new.txt");
    assert(replacement && replacement->inode != a->inode);
    assert(fat32_read(a, 0, 1, snapshot) == 0);
    fail_write = true;
    assert(fat32_write(file, 0, 1, "x") == 0);
    assert(!fs.version_confident);
    assert(fat32_file_facts(file, &facts) == VFS_ERR_UNCERTAIN);
    unsigned attempted = writes;
    device.id++;
    assert(fat32_mounted_read(&super, 10, 1, snapshot) == false);
    assert(writes == attempted);
    cleanup();
    setup();
    entry(0, "ONE     TXT", 3, 1);
    entry(1, "TWO     TXT", 3, 1);
    assert(fat32_finddir(&root, "one.txt"));
    assert(!fat32_finddir(&root, "two.txt"));
    assert(!fs.version_confident);
    cleanup();
    setup();
    entry(0, "SHORT   TXT", 3, 513);
    vfs_node_t *short_file = fat32_finddir(&root, "short.txt");
    assert(short_file && fat32_file_facts(short_file, &facts) ==
           VFS_ERR_BAD_FORMAT && !fs.version_confident);
    cleanup();
    setup();
    entry(0, "CYCLE   TXT", 3, 1);
    ((u32 *)media[1])[3] = 4;
    ((u32 *)media[1])[4] = 3;
    vfs_node_t *cycle = fat32_finddir(&root, "cycle.txt");
    assert(cycle && fat32_file_facts(cycle, &facts) == VFS_ERR_BAD_FORMAT);
    cleanup();
    setup();
    entry(0, "FREE    TXT", 3, 1);
    ((u32 *)media[1])[3] = 0;
    vfs_node_t *free_marked = fat32_finddir(&root, "free.txt");
    assert(free_marked && fat32_file_facts(free_marked, &facts) ==
           VFS_ERR_BAD_FORMAT);
    cleanup();
    setup();
    entry(0, "RETRY   TXT", 3, 1);
    vfs_node_t *retry = fat32_finddir(&root, "retry.txt");
    fail_read_lba = 1; /* FAT read during read-only mapping check. */
    assert(fat32_file_facts(retry, &facts) == VFS_ERR_IO &&
           fs.version_confident);
    fail_read_lba = ~(u64)0;
    assert(fat32_file_facts(retry, &facts) == VFS_OK);
    cleanup();
    setup();
    unsigned before = writes;
    fail_allocation = 2; /* first prepared identity object */
    vfs_node_t *unpublished = NULL;
    assert(fat32_create(&root, "new.txt", &unpublished) == VFS_ERR_NO_MEM);
    assert(!unpublished && writes == before);
    cleanup();
    setup();
    before = writes;
    fail_read_lba = 10;
    assert(fat32_create(&root, "new.txt", &unpublished) == VFS_ERR_IO);
    assert(!unpublished && writes == before);
    cleanup();
    setup();
    before = writes;
    fail_read_lba = 1; /* Allocation FAT read after admission. */
    assert(fat32_create(&root, "new.txt", &unpublished) == VFS_ERR_IO);
    assert(!unpublished && writes == before);
    cleanup();
    namespace_cases();
    if (argc > 1) disposable_image_case(argv[1]);
    puts("FAT32 mount identities, directory chains and namespace ownership passed");
    return 0;
}
