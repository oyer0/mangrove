/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production FAT32 functions with a controlled mounted block device. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../kernel/src/storage/fat32.c"

#define TEST_MEDIA_SECTORS 8500U
static u8 media[TEST_MEDIA_SECTORS][512];
static block_device_t device;
static bool online;
static bool fail_write;
static unsigned fail_write_at;
static u64 fail_write_lba;
static bool fail_write_definite;
static unsigned remove_after_writes;
static unsigned writes;
static unsigned directory_writes;
static unsigned reads;
static unsigned fat_reads, directory_reads;
static unsigned allocations, fail_allocation;
static usize live_requested, peak_requested;
static u64 fail_read_lba;
static FILE *image_file;
static bool image_writable;
static u64 image_sectors;
static unsigned image_reads;
static unsigned remove_after_reads;
static unsigned replace_after_reads;
static fat32_fs_t fs;
static vfs_super_t super;
static vfs_node_t root;
static fat32_node_state_t root_state;
static vfs_super_t *pin_super;
static u64 active_pin;
static bool pin_unresolved, fail_pin_finish;
static bool fail_pin_begin;
static vfs_recovery_handoff_t pin_handoff;
static void *pin_context;
static unsigned pin_begins, pin_finishes;

int vfs_recovery_pin_begin_handoff(vfs_super_t *sb,
                                   vfs_recovery_handoff_t handoff,
                                   void *context, u64 *out_id)
{
    assert(sb == pin_super && handoff && out_id && !active_pin);
    if (!online) return VFS_ERR_DEVICE_GONE;
    if (fail_pin_begin) return VFS_ERR_NO_SPACE;
    active_pin = ++pin_begins;
    pin_handoff = handoff;
    pin_context = context;
    *out_id = active_pin;
    return VFS_OK;
}
int vfs_recovery_pin_mark_unresolved(u64 id)
{
    assert(id && id == active_pin && !pin_unresolved);
    pin_unresolved = true;
    return VFS_OK;
}
int vfs_recovery_pin_finish_handoff(u64 id)
{
    assert(id && id == active_pin);
    if (fail_pin_finish) return VFS_ERR_BUSY;
    if (pin_unresolved) return VFS_ERR_UNCERTAIN;
    if (!pin_handoff(pin_super, pin_context)) {
        pin_unresolved = true;
        return VFS_ERR_UNCERTAIN;
    }
    active_pin = 0;
    pin_finishes++;
    return VFS_OK;
}

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
bool vfs_check_access(const vfs_node_t *node, u32 permission)
{ return node && (node->permissions & permission) == permission; }
void vfs_node_set_security(vfs_node_t *node, u32 owner, u32 permissions)
{ node->owner_uid = owner; node->permissions = permissions; }
void vfs_node_register(vfs_node_t *node)
{ assert(node->super && node->super->private_data); }
void vfs_nodes_update_size(vfs_super_t *sb, u64 inode, u64 size)
{
    fat32_fs_t *mounted_fs = (fat32_fs_t *)sb->private_data;
    assert(mounted_fs);
    for (fat32_node_state_t *state = mounted_fs->nodes; state;
         state = state->next)
        if (state->node->inode == inode) state->node->size = size;
}
bool block_read_for_id(block_device_t *dev, u64 id, u64 lba, u32 count,
                       void *buffer)
{
    if (image_file) {
        if (!online || dev != &device || id != device.id ||
            !count || lba >= image_sectors ||
            count > image_sectors - lba ||
            fseeko(image_file, (off_t)(lba * 512U), SEEK_SET) != 0)
            return false;
        if (fread(buffer, 1U, count * 512U, image_file) != count * 512U)
            return false;
        reads += count;
        image_reads += count;
        return true;
    }
    if (!online || dev != &device || id != device.id ||
        lba >= TEST_MEDIA_SECTORS || count != 1 ||
        lba == fail_read_lba) return false;
    memcpy(buffer, media[lba], 512);
    reads++;
    if (lba == 1U) fat_reads++;
    else directory_reads++;
    if (remove_after_reads && --remove_after_reads == 0U) online = false;
    if (replace_after_reads && --replace_after_reads == 0U) device.id++;
    return true;
}
bool block_write_for_id(block_device_t *dev, u64 id, u64 lba, u32 count,
                        const void *buffer)
{
    if (image_file) {
        if (!image_writable || !online || dev != &device ||
            id != device.id || !count || lba >= image_sectors ||
            count > image_sectors - lba ||
            fseeko(image_file, (off_t)(lba * 512U), SEEK_SET) != 0)
            return false;
        writes++;
        return fwrite(buffer, 1U, count * 512U, image_file) ==
                   count * 512U && fflush(image_file) == 0;
    }
    if (!online || dev != &device || id != device.id ||
        lba >= TEST_MEDIA_SECTORS ||
        count > TEST_MEDIA_SECTORS - lba) return false;
    writes++;
    if (lba == 10U && count == 1U) directory_writes++;
    if (fail_write ||
        (fail_write_at && writes == fail_write_at) ||
        lba == fail_write_lba) {
        if (!fail_write_definite)
            memcpy(media[lba], buffer, 128); /* ambiguous sector write */
        return false;
    }
    memcpy(media[lba], buffer, count * 512U);
    if (remove_after_writes && --remove_after_writes == 0U)
        online = false;
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
    directory_writes = 0;
    reads = 0;
    fat_reads = directory_reads = 0;
    fail_write = false;
    fail_write_at = 0;
    fail_write_lba = ~(u64)0;
    fail_write_definite = false;
    remove_after_writes = 0;
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
    active_pin = 0;
    pin_unresolved = fail_pin_finish = false;
    fail_pin_begin = false;
    pin_handoff = NULL;
    pin_context = NULL;
    pin_begins = pin_finishes = 0;
    super.dev = &device;
    super.private_data = &fs;
    pin_super = &super;
    root.inode = 1;
    root.type = VFS_TYPE_DIRECTORY;
    root.super = &super;
    root.fs_data = &root_state;
    root.ops = &fat32_node_ops;
    root.permissions = FAT32_VOLUME_PERMISSIONS;
    super.root_node = &root;
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
    /* End the controlled fixture after inspecting an intentionally retained
     * unresolved transaction; production never retires it automatically. */
    if (fs.replace_transaction) fat32_replace_free(fs.replace_transaction);
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

typedef struct { u64 short_at; } replace_source_state_t;
static u32 replace_source(void *context, u64 offset, u8 *buffer,
                          u32 requested);

/* The caller supplies a disposable image with TARGET.TXT in its root. */
static void replacement_image_case(const char *path)
{
    replace_source_state_t source = {.short_at = ~(u64)0};
    const u64 length = 512U * 1024U + 1U;
    image_file = fopen(path, "r+b");
    assert(image_file && fseeko(image_file, 0, SEEK_END) == 0);
    off_t bytes = ftello(image_file);
    assert(bytes > 0 && bytes % 512 == 0);
    image_sectors = (u64)bytes / 512U;
    image_writable = true;
    device.id = 902;
    device.sector_size = 512U;
    device.sector_count = image_sectors;
    online = true;
    vfs_super_t *mounted = NULL;
    assert(fat32_mount(&fat32_fs_type, &device, &mounted, false) == VFS_OK);
    pin_super = mounted;
    vfs_node_t *file = fat32_finddir(mounted->root_node, "TARGET.TXT");
    assert(file);
    vfs_file_facts_t facts;
    assert(fat32_file_facts(file, &facts) == VFS_OK);
    u64 pin = 0;
    assert(fat32_replace_locked(mounted, file->inode, facts.generation,
           length, replace_source, &source, &pin) == VFS_OK);
    assert(pin && vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    u8 first[2], final[2];
    assert(fat32_read(file, 0U, 2U, first) == 2U &&
           first[0] == 0U && first[1] == 1U);
    assert(fat32_read(file, length - 1U, 1U, final) == 1U &&
           final[0] == (u8)(length - 1U));
    assert(fat32_file_facts(file, &facts) == VFS_OK &&
           facts.size == length && facts.generation == 2U);
    vfs_node_t *created = (vfs_node_t *)kmalloc(sizeof(*created));
    assert(created);
    memset(created, 0, sizeof(*created));
    const u64 created_length = 512U * 1024U + 7U;
    pin = 0;
    assert(fat32_create_contents_locked(mounted->root_node, created,
           "CREATED.BIN", created_length, replace_source, &source,
           &facts, &pin) == VFS_OK);
    created->inode = facts.record_id;
    created->type = VFS_TYPE_FILE;
    created->size = facts.size;
    created->super = mounted;
    created->ops = &fat32_node_ops;
    assert(pin && vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    assert(fat32_finddir(mounted->root_node, "created.bin") == created &&
           fat32_read(created, created_length - 1U, 1U, final) == 1U &&
           final[0] == (u8)(created_length - 1U));
    fat32_fs_t *mounted_fs = (fat32_fs_t *)mounted->private_data;
    vfs_node_t *nodes[3];
    u32 node_count = 0;
    for (fat32_node_state_t *state = mounted_fs->nodes; state;
         state = state->next) {
        assert(node_count < 3U);
        nodes[node_count++] = state->node;
    }
    assert(fat32_unmount(mounted) == VFS_OK);
    for (u32 i = 0; i < node_count; i++) kfree(nodes[i]);
    kfree(mounted);
    fclose(image_file);
    image_file = NULL;
    image_writable = false;
    pin_super = &super;
    puts("FAT32 disposable image: replacement, safe creation and remount-ready metadata passed");
}

static u32 replace_source(void *context, u64 offset, u8 *buffer,
                          u32 requested)
{
    replace_source_state_t *state = (replace_source_state_t *)context;
    if (offset >= state->short_at) return 0;
    for (u32 i = 0; i < requested; i++)
        buffer[i] = (u8)(offset + i);
    return requested;
}

static vfs_node_t *replacement_file(void)
{
    entry(0, "TARGET  TXT", 3, 3);
    memcpy(media[11], "old", 3);
    vfs_node_t *file = fat32_finddir(&root, "target.txt");
    assert(file);
    return file;
}

static void replacement_cases(void)
{
    replace_source_state_t source = {.short_at = ~(u64)0};
    u64 pin = 0;
    unsigned before;
    vfs_node_t *file;

    setup();
    file = replacement_file();
    ((fat32_on_disk_entry_t *)media[10])[1].name[0] = (char)0xe5;
    before = writes;
    assert(fat32_replace_locked(&super, file->inode, 2, 3,
           replace_source, &source, &pin) == VFS_ERR_STALE_GENERATION);
    assert(writes == before && !pin && !active_pin);
    assert(fat32_replace_locked(&super, file->inode, 1, 3,
           replace_source, &source, &pin) == VFS_OK);
    assert(pin == active_pin && fs.replace_transaction &&
           !fs.version_confident &&
           fs.replace_transaction->phase ==
               FAT32_REPLACE_HANDOFF_PUBLISHED &&
           fat32_truncate(file) == VFS_ERR_UNCERTAIN);
    vfs_node_t *blocked_node = NULL;
    assert(fat32_write(file, 0U, 1U, "x") == 0U &&
           fat32_create(&root, "OTHER.TXT", &blocked_node) ==
               VFS_ERR_UNCERTAIN && !blocked_node &&
           fat32_unlink(&root, "target.txt") == VFS_ERR_UNCERTAIN &&
           fat32_rename(&root, "target.txt", &root, "OTHER.TXT") ==
               VFS_ERR_UNCERTAIN);
    assert(file->size == 3 && fat32_state(file)->generation == 2 &&
           fat32_state(file)->first_cluster == 4);
    assert(((u32 *)media[1])[3] == 0 &&
           ((u32 *)media[1])[4] == FAT32_EOC_MIN &&
           directory_writes == 1U &&
           !memcmp(media[11], "old", 3) &&
           media[12][0] == 0 && media[12][1] == 1 &&
           ((fat32_on_disk_entry_t *)media[10])[1].name[0] == (char)0xe5);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    assert(!active_pin && !fs.replace_transaction && fs.version_confident);
    assert(fat32_rename(&root, "target.txt", &root, "RENAMED.TXT") ==
           VFS_OK && fat32_finddir(&root, "renamed.txt") == file);
    u64 renamed_generation = fat32_state(file)->generation;
    assert(fat32_replace_locked(&super, file->inode, renamed_generation, 3,
           replace_source, &source, &pin) == VFS_OK &&
           fat32_finddir(&root, "renamed.txt") == NULL);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    assert(fat32_finddir(&root, "renamed.txt") == file);
    assert(fat32_state(file)->generation == renamed_generation + 1U);
    assert(fat32_replace_locked(&super, file->inode, renamed_generation + 1U, 0,
           NULL, NULL, &pin) == VFS_OK);
    assert(file->size == 0 && fat32_state(file)->first_cluster == 0 &&
           fat32_state(file)->generation == renamed_generation + 2U &&
           ((u32 *)media[1])[4] == 0);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    cleanup();

    setup();
    file = replacement_file();
    for (u32 cluster = 6U; cluster < fs.total_clusters; cluster++)
        ((u32 *)media[1])[cluster] = FAT32_EOC_MIN;
    assert(fat32_replace_locked(&super, file->inode, 1, 1536U,
           replace_source, &source, &pin) == VFS_ERR_NO_SPACE);
    assert(pin && fs.replace_transaction &&
           fs.replace_transaction->cancellation_cursor == 2U &&
           ((u32 *)media[1])[4] == 0U && ((u32 *)media[1])[5] == 0U &&
           ((u32 *)media[1])[3] == FAT32_EOC_MIN);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    cleanup();

    setup();
    file = replacement_file();
    fail_write_at = 2U; /* torn first fresh-data write */
    assert(fat32_replace_locked(&super, file->inode, 1, 3,
           replace_source, &source, &pin) == VFS_ERR_UNCERTAIN);
    assert(pin_unresolved && fs.replace_transaction &&
           fs.replace_transaction->phase == FAT32_REPLACE_STAGING &&
           fs.replace_transaction->fresh_count == 1U &&
           ((u32 *)media[1])[3] == FAT32_EOC_MIN);
    cleanup();

    setup();
    file = replacement_file();
    source.short_at = 0U;
    fail_write_at = 2U; /* torn FAT update during cancellation */
    assert(fat32_replace_locked(&super, file->inode, 1, 3,
           replace_source, &source, &pin) == VFS_ERR_UNCERTAIN);
    assert(pin_unresolved && fs.replace_transaction &&
           fs.replace_transaction->phase == FAT32_REPLACE_CANCELLING &&
           fs.replace_transaction->cancellation_cursor == 0U &&
           fs.replace_transaction->metadata_write_pending &&
           ((u32 *)media[1])[3] == FAT32_EOC_MIN);
    cleanup();
    source.short_at = ~(u64)0;

    setup();
    file = replacement_file();
    fs.fat_count = 2U;
    fs.bpb.ext_flags = 0x81U; /* nonmirrored, active copy one */
    memcpy(media[2], media[1], 512U);
    assert(fat32_replace_locked(&super, file->inode, 1, 3,
           replace_source, &source, &pin) == VFS_OK);
    assert(((u32 *)media[1])[3] == FAT32_EOC_MIN &&
           ((u32 *)media[1])[4] == 0U &&
           ((u32 *)media[2])[3] == 0U &&
           ((u32 *)media[2])[4] == FAT32_EOC_MIN);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    cleanup();

    setup();
    file = replacement_file();
    for (u32 cluster = 4U; cluster < fs.total_clusters; cluster++)
        ((u32 *)media[1])[cluster] = FAT32_EOC_MIN;
    before = writes;
    assert(fat32_replace_locked(&super, file->inode, 1, 3,
           replace_source, &source, &pin) == VFS_ERR_NO_SPACE);
    assert(pin && writes == before && fs.replace_transaction &&
           fs.replace_transaction->fresh_count == 0U &&
           ((u32 *)media[1])[3] == FAT32_EOC_MIN);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    cleanup();

    setup();
    file = replacement_file();
    source.short_at = 0;
    assert(fat32_replace_locked(&super, file->inode, 1, 20,
           replace_source, &source, &pin) == VFS_ERR_IO);
    assert(pin == active_pin && fs.replace_transaction &&
           fs.replace_transaction->phase ==
               FAT32_REPLACE_HANDOFF_CANCELLED &&
           ((u32 *)media[1])[4] == 0 &&
           ((u32 *)media[1])[3] == FAT32_EOC_MIN &&
           !memcmp(media[11], "old", 3) && file->size == 3);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    assert(fs.version_confident);
    cleanup();

    setup();
    file = replacement_file();
    source.short_at = ~(u64)0;
    fail_write_lba = 10;
    assert(fat32_replace_locked(&super, file->inode, 1, 3,
           replace_source, &source, &pin) == VFS_ERR_UNCERTAIN);
    assert(!pin && pin_unresolved && fs.replace_transaction &&
           fs.replace_transaction->phase == FAT32_REPLACE_PUBLISHING &&
           directory_writes == 1U &&
           fs.replace_transaction->old_count == 1 &&
           fs.replace_transaction->fresh_count == 1 &&
           !memcmp(fs.replace_transaction->old_sector + 32U,
                   fs.replace_transaction->proposed_sector + 32U, 32U) &&
           memcmp(fs.replace_transaction->old_sector,
                  fs.replace_transaction->proposed_sector, 32U) != 0 &&
           ((u32 *)media[1])[3] == FAT32_EOC_MIN &&
           ((u32 *)media[1])[4] == FAT32_EOC_MIN &&
           !fs.version_confident && fat32_read(file, 0, 3, media[50]) == 0);
    cleanup();

    setup();
    file = replacement_file();
    fail_write_at = 4; /* old-chain free after FAT, data, publication */
    assert(fat32_replace_locked(&super, file->inode, 1, 3,
           replace_source, &source, &pin) == VFS_ERR_UNCERTAIN);
    assert(pin_unresolved && fs.replace_transaction &&
           fs.replace_transaction->phase == FAT32_REPLACE_RECLAIMING &&
           fs.replace_transaction->reclamation_cursor == 0 &&
           fs.replace_transaction->metadata_write_pending &&
           fs.replace_transaction->metadata_cluster == 3U &&
           memcmp(fs.replace_transaction->metadata_before,
                  fs.replace_transaction->metadata_proposed, 512U) != 0 &&
           file->size == 3 && fat32_state(file)->generation == 2 &&
           fat32_state(file)->first_cluster == 4);
    cleanup();

    setup();
    file = replacement_file();
    fs.fat_count = 2;
    memcpy(media[2], media[1], 512);
    fail_write_at = 2; /* second mirrored FAT copy */
    assert(fat32_replace_locked(&super, file->inode, 1, 3,
           replace_source, &source, &pin) == VFS_ERR_UNCERTAIN);
    assert(pin_unresolved && fs.replace_transaction &&
           fs.replace_transaction->phase == FAT32_REPLACE_STAGING &&
           fs.replace_transaction->fresh_count == 1 &&
           fs.replace_transaction->metadata_write_pending &&
           fs.replace_transaction->metadata_copy == 1U &&
           fs.replace_transaction->metadata_copies_completed == 1U &&
           fs.replace_transaction->metadata_sector == 2U &&
           memcmp(fs.replace_transaction->metadata_before,
                  fs.replace_transaction->metadata_proposed, 512U) != 0 &&
           ((u32 *)media[1])[3] == FAT32_EOC_MIN &&
           ((u32 *)media[1])[4] == FAT32_EOC_MIN);
    cleanup();

    setup();
    file = replacement_file();
    before = writes;
    entry(1, "SHARED  TXT", 3, 1);
    assert(fat32_replace_locked(&super, file->inode, 1, 3,
           replace_source, &source, &pin) == VFS_ERR_BAD_FORMAT);
    assert(writes == before && !active_pin && !fs.version_confident);
    cleanup();

    setup();
    file = replacement_file();
    fail_allocation = allocations + 1U;
    before = writes;
    assert(fat32_replace_locked(&super, file->inode, 1, 3,
           replace_source, &source, &pin) == VFS_ERR_NO_MEM);
    assert(writes == before && !active_pin && fs.version_confident);
    cleanup();

    setup();
    file = replacement_file();
    remove_after_writes = 1;
    assert(fat32_replace_locked(&super, file->inode, 1, 3,
           replace_source, &source, &pin) == VFS_ERR_UNCERTAIN);
    assert(pin_unresolved && fs.replace_transaction && !online);
    device.id++;
    before = writes;
    assert(!fat32_mounted_write(&super, 10, 1U, media[10]) && writes == before);
    cleanup();

    setup();
    file = replacement_file();
    assert(fat32_replace_locked(&super, file->inode, 1, 3,
           replace_source, &source, &pin) == VFS_OK);
    fail_pin_finish = true;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_ERR_BUSY &&
           fs.replace_transaction && !fs.version_confident);
    fail_pin_finish = false;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fs.version_confident && !fs.replace_transaction);
    cleanup();

    setup();
    file = replacement_file();
    fail_pin_begin = true;
    before = writes;
    assert(fat32_replace_locked(&super, file->inode, 1, 3,
           replace_source, &source, &pin) == VFS_ERR_NO_SPACE);
    assert(!pin && !active_pin && !fs.replace_transaction &&
           writes == before && fs.version_confident);
    cleanup();

    setup();
    file = replacement_file();
    source.short_at = 512U;
    assert(fat32_replace_locked(&super, file->inode, 1, 600U,
           replace_source, &source, &pin) == VFS_ERR_IO);
    assert(pin && fs.replace_transaction &&
           fs.replace_transaction->cancellation_cursor == 2U &&
           ((u32 *)media[1])[4] == 0U && ((u32 *)media[1])[5] == 0U &&
           ((u32 *)media[1])[3] == FAT32_EOC_MIN &&
           file->size == 3U);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    cleanup();

    setup();
    file = replacement_file();
    source.short_at = ~(u64)0;
    ((u32 *)media[1])[3] = 4U;
    ((u32 *)media[1])[4] = FAT32_EOC_MIN;
    fs.next_free_cluster = 5U;
    fail_write_at = 5U; /* second old-cluster free */
    assert(fat32_replace_locked(&super, file->inode, 1, 3,
           replace_source, &source, &pin) == VFS_ERR_UNCERTAIN);
    assert(pin_unresolved && fs.replace_transaction &&
           fs.replace_transaction->phase == FAT32_REPLACE_RECLAIMING &&
           fs.replace_transaction->reclamation_cursor == 1U &&
           fs.replace_transaction->old_count == 2U &&
           ((u32 *)media[1])[3] == 0U &&
           fat32_state(file)->first_cluster == 5U);
    cleanup();

    setup();
    file = replacement_file();
    fs.fat_count = 2U;
    memcpy(media[2], media[1], 512U);
    assert(fat32_replace_locked(&super, file->inode, 1, 3,
           replace_source, &source, &pin) == VFS_OK);
    assert(((u32 *)media[1])[3] == 0U && ((u32 *)media[2])[3] == 0U &&
           ((u32 *)media[1])[4] == FAT32_EOC_MIN &&
           ((u32 *)media[2])[4] == FAT32_EOC_MIN);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    cleanup();

    setup();
    file = replacement_file();
    fs.reserved_sector_count = 5U;
    fs.fs_info_sector = 3U;
    fat32_store_le32(media[3], 0x41615252U);
    fat32_store_le32(media[3] + 484U, 0x61417272U);
    fat32_store_le32(media[3] + 508U, 0xaa550000U);
    fail_write_lba = 3U;
    assert(fat32_replace_locked(&super, file->inode, 1, 3,
           replace_source, &source, &pin) == VFS_ERR_UNCERTAIN);
    assert(pin_unresolved && fs.replace_transaction &&
           fs.replace_transaction->phase == FAT32_REPLACE_FSINFO &&
           fs.replace_transaction->metadata_write_pending &&
           fs.replace_transaction->metadata_sector == 3U &&
           memcmp(fs.replace_transaction->metadata_before,
                  fs.replace_transaction->metadata_proposed, 512U) != 0 &&
           fat32_state(file)->generation == 2U);
    cleanup();

    setup();
    file = replacement_file();
    fs.reserved_sector_count = 5U;
    fs.fs_info_sector = 3U;
    before = writes;
    assert(fat32_replace_locked(&super, file->inode, 1, 3,
           replace_source, &source, &pin) == VFS_ERR_BAD_FORMAT);
    assert(!pin && !active_pin && writes == before &&
           !fs.version_confident);
    cleanup();

    setup();
    file = replacement_file();
    fs.reserved_sector_count = 5U;
    fs.fs_info_sector = 3U;
    fail_read_lba = 3U;
    before = writes;
    assert(fat32_replace_locked(&super, file->inode, 1, 3,
           replace_source, &source, &pin) == VFS_ERR_IO);
    assert(!pin && !active_pin && writes == before &&
           fs.version_confident);
    cleanup();
}

static void replacement_large_case(bool fragmented, u32 length)
{
    replace_source_state_t source = {.short_at = ~(u64)0};
    u64 pin = 0;
    setup();
    fs.total_clusters = fragmented ? 8300U : 4200U;
    fs.fat_size_sectors = fragmented ? 65U : 33U;
    fs.data_start_lba = fragmented ? 80U : 40U;
    fat32_on_disk_entry_t *disk =
        (fat32_on_disk_entry_t *)media[fs.data_start_lba];
    memcpy(disk[0].name, "TARGET  TXT", 11U);
    disk[0].attr = 0x20U;
    disk[0].first_cluster_low = 3U;
    disk[0].file_size = 3U;
    memcpy(media[fs.data_start_lba + 1U], "old", 3U);
    if (fragmented) {
        for (u32 c = 4U; c < 8196U; c += 2U)
            ((u32 *)media[fs.fat_start_lba + c / 128U])[c % 128U] =
                FAT32_EOC_MIN;
    }
    vfs_node_t *file = fat32_finddir(&root, "target.txt");
    assert(file);
    unsigned before = writes;
    assert(fat32_replace_locked(&super, file->inode, 1,
           FAT32_REPLACE_MAX_BYTES + 1ULL, replace_source, &source,
           &pin) == VFS_ERR_NO_SPACE && writes == before && !pin);
    usize baseline = live_requested;
    peak_requested = baseline;
    fat_reads = directory_reads = reads = 0;
    assert(length && length <= FAT32_REPLACE_MAX_BYTES);
    assert(fat32_replace_locked(&super, file->inode, 1,
           length, replace_source, &source,
           &pin) == VFS_OK);
    u32 blocks = length / 512U + (length % 512U != 0U);
    assert(pin && file->size == length &&
           fat32_state(file)->generation == 2 &&
           fat32_state(file)->first_cluster == (fragmented ? 5U : 4U) &&
           fs.replace_transaction->fresh_count == blocks &&
           fs.replace_transaction->old_count == 1U &&
           ((u32 *)media[1])[3] == 0U);
    u32 last = fragmented ? 2U * blocks + 3U : blocks + 3U;
    assert(((u32 *)media[fs.fat_start_lba + last / 128U])[last % 128U] ==
           FAT32_EOC_MIN);
    u32 last_lba = fs.data_start_lba + last - 2U;
    assert(media[last_lba][(length - 1U) % 512U] ==
               (u8)(length - 1U) &&
           (length % 512U == 0U ||
            media[last_lba][length % 512U] == 0U) &&
           !memcmp(media[fs.data_start_lba + 1U], "old", 3U));
    printf("FAT32 %s %u-byte replacement: %zu peak requested bytes, %u writes, %u reads\n",
           fragmented ? "fragmented" : "contiguous", length,
           (size_t)(peak_requested - baseline), writes - before, reads);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    assert(live_requested == baseline && fs.version_confident);
    cleanup();
}

static void replacement_overallocated_old_case(void)
{
    replace_source_state_t source = {.short_at = ~(u64)0};
    u64 pin = 0;
    setup();
    fs.total_clusters = 8300U;
    fs.fat_size_sectors = 65U;
    fs.data_start_lba = 80U;
    fs.next_free_cluster = 4099U;
    fat32_on_disk_entry_t *disk =
        (fat32_on_disk_entry_t *)media[fs.data_start_lba];
    memcpy(disk[0].name, "TARGET  TXT", 11U);
    disk[0].attr = 0x20U;
    disk[0].first_cluster_low = 3U;
    disk[0].file_size = 3U;
    for (u32 cluster = 3U; cluster <= 4098U; cluster++)
        ((u32 *)media[fs.fat_start_lba + cluster / 128U])
            [cluster % 128U] = cluster == 4098U ?
                FAT32_EOC_MIN : cluster + 1U;
    vfs_node_t *file = fat32_finddir(&root, "target.txt");
    assert(file);
    usize baseline = live_requested;
    peak_requested = baseline;
    reads = writes = 0;
    assert(fat32_replace_locked(&super, file->inode, 1,
           FAT32_REPLACE_MAX_BYTES, replace_source, &source,
           &pin) == VFS_OK);
    fat32_replace_transaction_t *tx = fs.replace_transaction;
    assert(tx && tx->old_count == 4096U && tx->fresh_count == 4096U &&
           tx->data_buffer == NULL && tx->reclamation_cursor == 4096U &&
           ((u32 *)media[1])[3] == 0U && file->size ==
           FAT32_REPLACE_MAX_BYTES);
    printf("FAT32 2 MiB old/new inventory: %zu peak, %zu retained requested bytes, %u writes, %u reads\n",
           (size_t)(peak_requested - baseline),
           (size_t)(live_requested - baseline), writes, reads);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           live_requested == baseline);
    cleanup();
}

static void create_contents_case(u32 length)
{
    replace_source_state_t source = {.short_at = ~(u64)0};
    vfs_file_facts_t facts = {0};
    u64 pin = 0;
    setup();
    if (length > 60U * 512U) {
        fs.total_clusters = 4200U;
        fs.fat_size_sectors = 33U;
        fs.data_start_lba = 40U;
    }
    vfs_node_t *created = (vfs_node_t *)kmalloc(sizeof(*created));
    assert(created);
    memset(created, 0, sizeof(*created));
    usize baseline = live_requested;
    peak_requested = baseline;
    reads = 0;
    unsigned before = writes;
    int result = fat32_create_contents_locked(&root, created, "NEW.TXT", length,
           replace_source, &source, &facts, &pin);
    if (result != VFS_OK) fprintf(stderr, "create %u failed: %d\n", length, result);
    assert(result == VFS_OK);
    assert(pin && active_pin && fs.replace_transaction &&
           fs.replace_transaction->creating &&
           facts.record_id == 2U && facts.generation == 1U &&
           facts.size == length && created->fs_data &&
           fs.replace_transaction->fresh_count ==
               length / 512U + (length % 512U != 0U));
    usize retained = live_requested - baseline;
    created->inode = facts.record_id;
    created->type = VFS_TYPE_FILE;
    created->size = facts.size;
    created->super = &super;
    created->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    vfs_node_t *found = fat32_finddir(&root, "new.txt");
    assert(found == created && fs.version_confident);
    if (length) {
        u8 first = 0, last = 0;
        assert(fat32_read(created, 0, 1, &first) == 1U && first == 0U);
        assert(fat32_read(created, length - 1U, 1U, &last) == 1U &&
               last == (u8)(length - 1U));
    }
    printf("FAT32 %u-byte create: %zu peak, %zu retained requested bytes, %u writes, %u reads\n",
           length, (size_t)(peak_requested - baseline),
           (size_t)retained, writes - before, reads);
    cleanup();
}

static void create_contents_failure_cases(void)
{
    replace_source_state_t source = {.short_at = ~(u64)0};
    vfs_file_facts_t facts = {0};
    u64 pin = 0;
    vfs_node_t prepared = {0};
    unsigned before;
    setup();
    before = writes;
    assert(fat32_create_contents_locked(&root, &prepared, "LONGERNAME.TXT",
           0, replace_source, &source, &facts, &pin) ==
           VFS_ERR_INVALID_PARAM);
    assert(fat32_create_contents_locked(&root, &prepared, "NEW.TXT",
           FAT32_REPLACE_MAX_BYTES + 1ULL, replace_source, &source,
           &facts, &pin) == VFS_ERR_NO_SPACE);
    assert(writes == before && !pin);
    entry(0, "NEW     TXT", 0, 0);
    assert(fat32_create_contents_locked(&root, &prepared, "new.txt",
           0, replace_source, &source, &facts, &pin) ==
           VFS_ERR_ALREADY_EXISTS && writes == before);
    cleanup();

    setup();
    root.permissions = 0U;
    before = writes;
    assert(fat32_create_contents_locked(&root, &prepared, "NEW.TXT",
           0, replace_source, &source, &facts, &pin) ==
           VFS_ERR_ACCESS_DENIED && !pin && writes == before);
    cleanup();

    setup();
    root_state.first_cluster = 3U;
    before = writes;
    assert(fat32_create_contents_locked(&root, &prepared, "NEW.TXT",
           0, replace_source, &source, &facts, &pin) ==
           VFS_ERR_BAD_FORMAT && !pin && writes == before &&
           !fs.version_confident);
    cleanup();

    setup();
    source.short_at = 0;
    before = writes;
    assert(fat32_create_contents_locked(&root, &prepared, "NEW.TXT",
           3, replace_source, &source, &facts, &pin) == VFS_ERR_IO);
    assert(pin && writes > before && !fs.version_confident &&
           !((fat32_on_disk_entry_t *)media[10])[0].name[0]);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fs.version_confident && !fs.replace_transaction &&
           !((fat32_on_disk_entry_t *)media[10])[0].name[0]);
    cleanup();

    setup();
    source.short_at = ~(u64)0;
    fail_write_lba = 12U; /* first fresh data cluster */
    assert(fat32_create_contents_locked(&root, &prepared, "NEW.TXT",
           3, replace_source, &source, &facts, &pin) == VFS_ERR_UNCERTAIN);
    assert(!pin && active_pin && pin_unresolved && fs.replace_transaction &&
           !((fat32_on_disk_entry_t *)media[10])[0].name[0]);
    cleanup();

    setup();
    fail_write_lba = 10U; /* publication sector */
    assert(fat32_create_contents_locked(&root, &prepared, "NEW.TXT",
           3, replace_source, &source, &facts, &pin) == VFS_ERR_UNCERTAIN);
    assert(!pin && active_pin && pin_unresolved && fs.replace_transaction &&
           fs.replace_transaction->phase == FAT32_REPLACE_PUBLISHING &&
           fs.replace_transaction->fresh_count == 1U);
    cleanup();

    setup();
    fail_write_lba = 10U;
    fail_write_definite = true;
    assert(fat32_create_contents_locked(&root, &prepared, "NEW.TXT",
           3, replace_source, &source, &facts, &pin) == VFS_ERR_UNCERTAIN &&
           !pin && active_pin && pin_unresolved &&
           !((fat32_on_disk_entry_t *)media[10])[0].name[0] &&
           fs.replace_transaction->phase == FAT32_REPLACE_PUBLISHING);
    cleanup();

    setup();
    fail_allocation = 1U;
    before = writes;
    assert(fat32_create_contents_locked(&root, &prepared, "NEW.TXT",
           3, replace_source, &source, &facts, &pin) == VFS_ERR_NO_MEM &&
           !pin && writes == before && !active_pin);
    cleanup();

    setup();
    fail_pin_begin = true;
    before = writes;
    assert(fat32_create_contents_locked(&root, &prepared, "NEW.TXT",
           3, replace_source, &source, &facts, &pin) == VFS_ERR_NO_SPACE &&
           !pin && writes == before && !active_pin && !fs.replace_transaction);
    cleanup();

    setup();
    for (u32 i = 0; i < 16U; i++) {
        char short_name[12];
        snprintf(short_name, sizeof(short_name), "F%02u     TXT", i);
        entry(i, short_name, 0, 0);
    }
    before = writes;
    assert(fat32_create_contents_locked(&root, &prepared, "NEW.TXT",
           0, replace_source, &source, &facts, &pin) == VFS_ERR_NO_SPACE &&
           !pin && writes == before);
    cleanup();

    setup();
    fs.total_clusters = 4U; /* root and cluster 3 exhaust the fixture. */
    before = writes;
    assert(fat32_create_contents_locked(&root, &prepared, "NEW.TXT",
           3, replace_source, &source, &facts, &pin) == VFS_ERR_NO_SPACE &&
           pin && writes == before);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fs.version_confident);
    cleanup();

    setup();
    fs.fat_count = 2U;
    fs.fat_size_sectors = 1U;
    fs.data_start_lba = 10U; /* The controlled fixture maps cluster 2 here. */
    memcpy(media[2], media[1], 512U);
    fail_write_lba = 2U; /* second FAT copy after the first was written */
    assert(fat32_create_contents_locked(&root, &prepared, "NEW.TXT",
           3, replace_source, &source, &facts, &pin) == VFS_ERR_UNCERTAIN &&
           !pin && active_pin && pin_unresolved &&
           fs.replace_transaction->metadata_copy == 1U &&
           fs.replace_transaction->fresh_count == 1U &&
           !((fat32_on_disk_entry_t *)media[10])[0].name[0]);
    cleanup();

    setup();
    fs.fs_info_sector = 2U;
    fs.reserved_sector_count = 3U;
    fat32_store_le32(media[2], 0x41615252U);
    fat32_store_le32(media[2] + 484U, 0x61417272U);
    fat32_store_le32(media[2] + 508U, 0xaa550000U);
    fail_write_lba = 2U; /* confirmed publication, uncertain FSInfo */
    assert(fat32_create_contents_locked(&root, &prepared, "NEW.TXT",
           3, replace_source, &source, &facts, &pin) == VFS_ERR_UNCERTAIN &&
           !pin && active_pin && pin_unresolved &&
           fs.replace_transaction->phase == FAT32_REPLACE_FSINFO &&
           !memcmp(((fat32_on_disk_entry_t *)media[10])[0].name,
                   "NEW     TXT", 11U));
    cleanup();

    setup();
    remove_after_writes = 1U;
    assert(fat32_create_contents_locked(&root, &prepared, "NEW.TXT",
           3, replace_source, &source, &facts, &pin) == VFS_ERR_UNCERTAIN &&
           !online && active_pin && pin_unresolved &&
           fs.replace_transaction && !pin);
    cleanup();

    setup();
    assert(fat32_create_contents_locked(&root, &prepared, "NEW.TXT",
           3, replace_source, &source, &facts, &pin) == VFS_OK);
    prepared.super = &super;
    prepared.inode = facts.record_id;
    prepared.type = VFS_TYPE_FILE;
    prepared.size = facts.size;
    prepared.ops = &fat32_node_ops;
    fail_pin_finish = true;
    online = false; /* forced removal during VFS finish handoff */
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_ERR_BUSY &&
           vfs_recovery_pin_mark_unresolved(pin) == VFS_OK);
    fat32_discard_created_node(&prepared, &super);
    assert(active_pin && pin_unresolved && fs.replace_transaction &&
           fs.replace_transaction->creating && !fs.version_confident &&
           fs.nodes == &root_state && prepared.fs_data == NULL);
    cleanup();
}

static void create_fragmented_directory_case(void)
{
    replace_source_state_t source = {.short_at = ~(u64)0};
    vfs_file_facts_t facts = {0};
    u64 pin = 0;
    setup();
    ((u32 *)media[1])[2] = 4U;
    ((u32 *)media[1])[4] = FAT32_EOC_MIN;
    for (u32 i = 0; i < 16U; i++) {
        char short_name[12];
        snprintf(short_name, sizeof(short_name), "F%02u     TXT", i);
        entry(i, short_name, 0U, 0U);
    }
    ((u32 *)media[1])[6] = FAT32_EOC_MIN;
    ((u32 *)media[1])[8] = FAT32_EOC_MIN;
    vfs_node_t *created = (vfs_node_t *)kmalloc(sizeof(*created));
    assert(created);
    memset(created, 0, sizeof(*created));
    assert(fat32_create_contents_locked(&root, created, "FRAG.TXT", 1536U,
           replace_source, &source, &facts, &pin) == VFS_OK);
    assert(fs.replace_transaction->directory_sector == 12U &&
           fs.replace_transaction->fresh_count == 3U &&
           fs.replace_transaction->fresh_clusters[0] == 5U &&
           fs.replace_transaction->fresh_clusters[1] == 7U &&
           fs.replace_transaction->fresh_clusters[2] == 9U &&
           !memcmp(((fat32_on_disk_entry_t *)media[12])[0].name,
                   "FRAG    TXT", 11U));
    created->inode = facts.record_id;
    created->type = VFS_TYPE_FILE;
    created->size = facts.size;
    created->super = &super;
    created->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_finddir(&root, "frag.txt") == created);
    cleanup();
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
    replacement_cases();
    replacement_large_case(false, 1U);
    replacement_large_case(false, 512U * 1024U);
    replacement_large_case(false, 512U * 1024U + 1U);
    replacement_large_case(false, FAT32_REPLACE_MAX_BYTES - 1U);
    replacement_large_case(false, FAT32_REPLACE_MAX_BYTES);
    replacement_large_case(true, FAT32_REPLACE_MAX_BYTES);
    replacement_overallocated_old_case();
    create_contents_case(0U);
    create_contents_case(3U);
    create_contents_case(512U * 1024U);
    create_contents_case(FAT32_REPLACE_MAX_BYTES);
    create_contents_failure_cases();
    create_fragmented_directory_case();
    if (argc > 1) disposable_image_case(argv[1]);
    if (argc > 2) replacement_image_case(argv[2]);
    puts("FAT32 mount identities, directory chains and namespace ownership passed");
    return 0;
}
