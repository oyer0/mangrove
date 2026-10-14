/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production FAT32 functions with a controlled mounted block device. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../kernel/src/storage/fat32.c"

#define TEST_MEDIA_SECTORS 8500U
static u8 media[TEST_MEDIA_SECTORS][512];
static block_device_t device;
static bool online;
static bool fail_write;
static unsigned fail_write_at;
static u64 fail_write_lba;
static bool fail_write_definite;
static unsigned fail_write_partial_bytes;
static unsigned remove_after_writes;
static unsigned writes;
static unsigned directory_writes;
static unsigned flushes, fail_flush_at;
static unsigned reads;
static unsigned fat_reads, directory_reads;
static unsigned allocations, fail_allocation;
static bool poison_allocations;
static usize live_requested, peak_requested;
static u64 fail_read_lba;
static unsigned fail_fat_read_after_writes;
static FILE *image_file;
static bool image_writable;
static u64 image_sectors;
static unsigned image_reads;
static unsigned remove_after_reads;
static unsigned replace_after_reads;
static bool (*scaling_read)(block_device_t *, u64, u64, u32, void *);
static bool (*scaling_write)(block_device_t *, u64, u64, u32, const void *);
static bool (*flush_hook)(block_device_t *, u64);
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
    if (poison_allocations) memset(base + 1, 0xa5, size);
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
    if (scaling_read) return scaling_read(dev, id, lba, count, buffer);
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
        !count || lba >= TEST_MEDIA_SECTORS ||
        count > TEST_MEDIA_SECTORS - lba ||
        lba == fail_read_lba) return false;
    if (lba == 1U && fail_fat_read_after_writes &&
        writes == fail_fat_read_after_writes) {
        fail_fat_read_after_writes = 0U;
        return false;
    }
    memcpy(buffer, media[lba], count * 512U);
    reads += count;
    if (lba == 1U) fat_reads++;
    else directory_reads++;
    if (remove_after_reads && --remove_after_reads == 0U) online = false;
    if (replace_after_reads && --replace_after_reads == 0U) device.id++;
    return true;
}
bool block_write_for_id(block_device_t *dev, u64 id, u64 lba, u32 count,
                        const void *buffer)
{
    if (scaling_write) return scaling_write(dev, id, lba, count, buffer);
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
            memcpy(media[lba], buffer, fail_write_partial_bytes);
        return false;
    }
    memcpy(media[lba], buffer, count * 512U);
    if (remove_after_writes && --remove_after_writes == 0U)
        online = false;
    return true;
}
bool block_device_id_is_live(u64 id) { return online && id == device.id; }
bool block_flush_for_id(block_device_t *dev, u64 id)
{
    if (flush_hook) return flush_hook(dev, id);
    flushes++;
    if (image_file && fflush(image_file) != 0) return false;
    return online && dev == &device && id == device.id &&
           (!fail_flush_at || flushes != fail_flush_at);
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
    scaling_read = NULL;
    scaling_write = NULL;
    flush_hook = NULL;
    device.id = 87;
    online = true;
    writes = 0;
    directory_writes = 0;
    flushes = fail_flush_at = 0;
    reads = 0;
    fat_reads = directory_reads = 0;
    fail_write = false;
    fail_write_at = 0;
    fail_write_lba = ~(u64)0;
    fail_write_definite = false;
    fail_write_partial_bytes = 128U;
    remove_after_writes = 0;
    allocations = fail_allocation = 0;
    poison_allocations = false;
    live_requested = peak_requested = 0;
    fail_read_lba = ~(u64)0;
    fail_fat_read_after_writes = 0U;
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

/* Simulate VFS dispatch and its post-operation-lock resolved-pin handoff. */
static int create_node_finished(vfs_node_t *dir, const char *name,
                                 vfs_node_t **out, bool directory)
{
    vfs_node_t *node = kmalloc(sizeof(*node));
    *out = NULL;
    if (!node) return VFS_ERR_NO_MEM;
    memset(node, 0, sizeof(*node));
    u64 pin = 0U;
    int result = directory ? fat32_mkdir_pinned(dir, node, name, &pin) :
                             fat32_create_pinned(dir, node, name, &pin);
    if (pin && vfs_recovery_pin_finish_handoff(pin) != VFS_OK)
        result = VFS_ERR_UNCERTAIN;
    if (result == VFS_OK) *out = node;
    else {
        if (node->fs_data) fat32_discard_created_node(node, dir->super);
        kfree(node);
    }
    return result;
}
static int create_finished(vfs_node_t *dir, const char *name, vfs_node_t **out)
{ return create_node_finished(dir, name, out, false); }
static int mkdir_finished(vfs_node_t *dir, const char *name, vfs_node_t **out)
{ return create_node_finished(dir, name, out, true); }

static u64 write_finished(vfs_node_t *node, u64 at, u64 size, const void *data)
{
    u64 bytes = 0U, pin = 0U;
    int result = fat32_write_pinned(node, at, size, data, &bytes, &pin);
    if (pin && vfs_recovery_pin_finish_handoff(pin) != VFS_OK) return 0U;
    return result == VFS_ERR_UNCERTAIN ? 0U : bytes;
}

static int truncate_finished(vfs_node_t *node)
{
    u64 pin = 0U;
    int result = fat32_truncate_pinned(node, &pin);
    if (pin && vfs_recovery_pin_finish_handoff(pin) != VFS_OK)
        return VFS_ERR_UNCERTAIN;
    return result;
}

static int unlink_finished(vfs_node_t *dir, const char *name)
{
    u64 pin = 0U;
    int result = fat32_unlink_pinned(dir, name, &pin);
    if (pin && vfs_recovery_pin_finish_handoff(pin) != VFS_OK)
        return VFS_ERR_UNCERTAIN;
    return result;
}

static int rmdir_finished(vfs_node_t *dir, const char *name)
{
    u64 pin = 0U;
    int result = fat32_rmdir_pinned(dir, name, &pin);
    if (pin && vfs_recovery_pin_finish_handoff(pin) != VFS_OK)
        return VFS_ERR_UNCERTAIN;
    return result;
}

static int rename_finished(vfs_node_t *src, const char *name,
                            vfs_node_t *dst, const char *new_name)
{
    u64 pin = 0U;
    int result = fat32_rename_pinned(pin_super, src, name, dst, new_name, &pin);
    if (pin && vfs_recovery_pin_finish_handoff(pin) != VFS_OK)
        return VFS_ERR_UNCERTAIN;
    return result;
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
    assert(create_finished(&root, "LATE.TXT", &collision) ==
           VFS_ERR_ALREADY_EXISTS && !collision && writes == before);
    assert(write_finished(file, 4, 1, "!") == 1 && file->size == 5 &&
           ((fat32_on_disk_entry_t *)media[12])[0].file_size == 5);
    assert(truncate_finished(file) == VFS_OK && file->size == 0 &&
           fat32_ownership_preflight_locked(file) == VFS_OK);
    vfs_node_t *created = NULL;
    assert(create_finished(&root, "NEW.TXT", &created) == VFS_OK &&
           created && fat32_finddir(&root, "new.txt") == created);
    vfs_node_t *new_directory = NULL;
    assert(mkdir_finished(&root, "SUB", &new_directory) == VFS_OK &&
           new_directory && fat32_finddir(&root, "sub") == new_directory);
    assert(rename_finished(&root, "late.txt", &root, "MOVED.TXT") == VFS_OK);
    assert(fat32_finddir(&root, "moved.txt") == file);
    assert(unlink_finished(&root, "moved.txt") == VFS_OK);
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
    fs.total_clusters = 8U * 1024U * 1024U + 1U;
    assert(fat32_ownership_preflight_locked(file) == VFS_OK && fs.version_confident);
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
    assert(create_finished(&root, "NEW.TXT", &unpublished) == VFS_OK &&
           unpublished && writes > before &&
           fat32_finddir(&root, "NEW.TXT") == unpublished);
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
    assert(rmdir_finished(&root, "subdir") == VFS_ERR_NOT_EMPTY);
    cleanup();

    setup();
    ((u32 *)media[1])[6] = 7;
    ((u32 *)media[1])[7] = FAT32_EOC_MIN;
    entry_at(2, 0, "SUBDIR     ", 6, 0, 0x10);
    entry_at(6, 0, ".          ", 6, 0, 0x10);
    entry_at(6, 1, "..         ", 0, 0, 0x10);
    assert(rmdir_finished(&root, "subdir") == VFS_OK &&
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
    assert(create_finished(&root, "LONGFI~1.TXT", &alias_collision) ==
           VFS_ERR_ALREADY_EXISTS && !alias_collision && writes == before);
    u64 retire_pin = 0U;
    assert(fat32_unlink(&root, long_name) == VFS_ERR_UNSUPPORTED);
    assert(fat32_unlink_pinned(&root, long_name, &retire_pin) == VFS_OK &&
           retire_pin && active_pin && fs.replace_transaction &&
           media[10][15U * 32U] == 0xe5U &&
           media[12][0] == 0xe5U);
    assert(vfs_recovery_pin_finish_handoff(retire_pin) == VFS_OK);
    vfs_node_t *after_lfn_delete = NULL;
    assert(create_finished(&root, "NEW.TXT", &after_lfn_delete) == VFS_OK &&
           after_lfn_delete &&
           fat32_state(after_lfn_delete)->entry_index == 15U &&
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
    assert(unlink_finished(&root, "target.txt") == VFS_OK);
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
    assert(mounted->read_only);
    vfs_node_t *directory = fat32_finddir(mounted->root_node, "MANY");
    vfs_node_t *last = directory ? fat32_finddir(directory, "F48.TXT") : NULL;
    assert(last);
    unsigned before_writes = writes;
    u64 pin;
    assert(fat32_truncate_pinned(last, &pin) == VFS_ERR_ACCESS_DENIED && !pin);
    assert(fat32_unlink_pinned(directory, "F48.TXT", &pin) == VFS_ERR_ACCESS_DENIED && !pin);
    assert(writes == before_writes && !active_pin);

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
    vfs_node_t *long_created = (vfs_node_t *)kmalloc(sizeof(*long_created));
    assert(long_created);
    memset(long_created, 0, sizeof(*long_created));
    pin = 0;
    assert(fat32_create_contents_locked(mounted->root_node, long_created,
           "R\xc3\xa9sum\xc3\xa9 from Mangrove.txt", 3U,
           replace_source, &source, &facts, &pin) == VFS_OK);
    long_created->inode = facts.record_id;
    long_created->type = VFS_TYPE_FILE;
    long_created->size = facts.size;
    long_created->super = mounted;
    long_created->ops = &fat32_node_ops;
    assert(pin && vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_finddir(mounted->root_node,
                         "R\xc3\x89SUM\xc3\x89 FROM MANGROVE.TXT") == long_created &&
           fat32_read(long_created, 2U, 1U, final) == 1U && final[0] == 2U);
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
           truncate_finished(file) == VFS_ERR_UNCERTAIN);
    vfs_node_t *blocked_node = NULL;
    assert(write_finished(file, 0U, 1U, "x") == 0U &&
           create_finished(&root, "OTHER.TXT", &blocked_node) ==
               VFS_ERR_UNCERTAIN && !blocked_node &&
           unlink_finished(&root, "target.txt") == VFS_ERR_UNCERTAIN &&
           rename_finished(&root, "target.txt", &root, "OTHER.TXT") ==
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
    assert(rename_finished(&root, "target.txt", &root, "RENAMED.TXT") ==
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
    if (length == FAT32_REPLACE_MAX_BYTES) assert(writes - before < 8500);
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
    assert(fat32_create_contents_locked(&root, &prepared, "BAD/NAME.TXT",
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
    for (u32 cluster = 4U; cluster < fs.total_clusters; cluster++)
        ((u32 *)media[1])[cluster] = FAT32_EOC_MIN;
    before = writes;
    assert(fat32_create_contents_locked(&root, &prepared, "NEW.TXT",
           0, replace_source, &source, &facts, &pin) == VFS_ERR_NO_SPACE &&
           pin && writes == before);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
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

static void create_lfn_cases(void)
{
    replace_source_state_t source = {.short_at = ~(u64)0};
    vfs_file_facts_t facts = {0};
    u64 pin = 0;
    vfs_node_t prepared = {0};
    const char *name = "R\xc3\xa9sum\xc3\xa9 report.txt";
    setup();
    vfs_node_t *created = (vfs_node_t *)kmalloc(sizeof(*created));
    assert(created);
    memset(created, 0, sizeof(*created));
    usize baseline = live_requested;
    peak_requested = baseline;
    unsigned before = writes;
    unsigned before_reads = reads;
    assert(fat32_create_contents_locked(&root, created, name, 3U,
           replace_source, &source, &facts, &pin) == VFS_OK);
    unsigned creation_writes = writes - before;
    unsigned creation_reads = reads - before_reads;
    assert(pin && directory_writes == 1U && facts.size == 3U &&
           facts.generation == 1U);
    fat32_replace_transaction_t *tx = fs.replace_transaction;
    assert(tx && tx->directory_sector == 10U &&
           tx->fresh_count == 1U && !tx->old_sector[0]);
    fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)media[10];
    fat32_on_disk_entry_t *short_entry =
        &((fat32_on_disk_entry_t *)media[10])[2];
    assert(lfn[0].order == 0x42U && lfn[1].order == 1U &&
           lfn[0].attr == 0x0fU && lfn[1].attr == 0x0fU &&
           lfn[0].checksum == fat32_lfn_checksum(short_entry->name) &&
           lfn[1].checksum == lfn[0].checksum &&
           !memcmp(short_entry->name, "R_SUM_~1TXT", 11U) &&
           short_entry->file_size == 3U && media[10][3U * 32U] == 0U);
    assert(!memcmp(media[10] + 3U * 32U,
                   tx->old_sector + 3U * 32U, 512U - 3U * 32U));
    usize retained = live_requested - baseline;
    created->super = &super;
    created->inode = facts.record_id;
    created->type = VFS_TYPE_FILE;
    created->size = facts.size;
    created->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    assert(fat32_finddir(&root, "R\xc3\x89SUM\xc3\x89 REPORT.TXT") == created);
    assert(fat32_finddir(&root, "R_SUM_~1.TXT") == created);
    u8 bytes[3];
    assert(fat32_read(created, 0U, 3U, bytes) == 3U &&
           bytes[0] == 0U && bytes[1] == 1U && bytes[2] == 2U);
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
           "r\xc3\xa9sum\xc3\xa9 REPORT.txt", 0U, replace_source,
           &source, &facts, &pin) == VFS_ERR_ALREADY_EXISTS);
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
           "R_SUM_~1.TXT", 0U, replace_source, &source, &facts,
           &pin) == VFS_ERR_ALREADY_EXISTS);
    assert(unlink_finished(&root, name) == VFS_OK &&
           media[10][0] == 0xe5U && media[10][32U] == 0xe5U &&
           media[10][64U] == 0xe5U && root_state.version_valid &&
           fs.version_confident);
    assert(fat32_check_name_available_locked_view(&root, name, NULL) ==
           VFS_OK);
    printf("FAT32 Unicode LFN create: %zu peak, %zu retained requested bytes, %u writes, %u reads\n",
           (size_t)(peak_requested - baseline),
           (size_t)retained, creation_writes, creation_reads);
    cleanup();

    setup();
    created = (vfs_node_t *)kmalloc(sizeof(*created));
    assert(created);
    memset(created, 0, sizeof(*created));
    entry(0, "REPORT~1TXT", 0U, 0U);
    before = writes;
    assert(fat32_create_contents_locked(&root, created, "Report draft.txt",
           0U, replace_source, &source, &facts, &pin) == VFS_OK);
    assert(!memcmp(((fat32_on_disk_entry_t *)media[10])[0].name,
                   "REPORT~1TXT", 11U) &&
           !memcmp(((fat32_on_disk_entry_t *)media[10])[3].name,
                   "REPORT~2TXT", 11U));
    created->super = &super;
    created->inode = facts.record_id;
    created->type = VFS_TYPE_FILE;
    created->size = facts.size;
    created->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    cleanup();

    setup();
    for (u32 number = 1U; number <= 9U; number++) {
        char alias[12];
        snprintf(alias, sizeof(alias), "REPORT~%uTXT", number);
        entry(number - 1U, alias, 0U, 0U);
    }
    created = (vfs_node_t *)kmalloc(sizeof(*created));
    assert(created);
    memset(created, 0, sizeof(*created));
    assert(fat32_create_contents_locked(&root, created, "Report draft.txt",
           0U, replace_source, &source, &facts, &pin) == VFS_OK &&
           !memcmp(((fat32_on_disk_entry_t *)media[10])[11].name,
                   "REPOR~10TXT", 11U));
    created->super = &super;
    created->inode = facts.record_id;
    created->type = VFS_TYPE_FILE;
    created->size = facts.size;
    created->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    cleanup();

    setup();
    for (u32 i = 0U; i < 15U; i++) {
        char short_name[12];
        snprintf(short_name, sizeof(short_name), "F%02u     TXT", i);
        entry(i, short_name, 0U, 0U);
    }
    created = (vfs_node_t *)kmalloc(sizeof(*created));
    assert(created);
    memset(created, 0, sizeof(*created));
    before = writes;
    assert(fat32_create_contents_locked(&root, created, "Long name.txt",
           0U, replace_source, &source, &facts, &pin) == VFS_OK &&
           pin && writes > before && ((u32 *)media[1])[2] == 4U &&
           media[10][15U * 32U] == 0xe5U);
    created->super = &super;
    created->inode = facts.record_id;
    created->type = VFS_TYPE_FILE;
    created->size = facts.size;
    created->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_finddir(&root, "Long name.txt") == created);
    cleanup();

    setup();
    before = writes;
    created = (vfs_node_t *)kmalloc(sizeof(*created));
    assert(created); memset(created, 0, sizeof(*created));
    assert(fat32_create_contents_locked(&root, created,
           "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
           "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
           "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
           "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
           0U, replace_source, &source, &facts, &pin) ==
           VFS_OK && pin && writes > before);
    created->super = &super; created->inode = facts.record_id;
    created->size = facts.size; created->type = VFS_TYPE_FILE;
    created->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    assert(fat32_create_contents_locked(&root, &prepared, "bad\xc0\xaf.txt",
           0U, replace_source, &source, &facts, &pin) ==
           VFS_ERR_INVALID_PARAM && !pin);
    cleanup();

    setup();
    char longest[197];
    memset(longest, 'a', 196U);
    longest[196] = 0;
    before = writes;
    created = (vfs_node_t *)kmalloc(sizeof(*created));
    assert(created); memset(created, 0, sizeof(*created));
    assert(fat32_create_contents_locked(&root, created, longest,
           0U, replace_source, &source, &facts, &pin) == VFS_OK && pin);
    created->super = &super; created->inode = facts.record_id;
    created->size = facts.size; created->type = VFS_TYPE_FILE;
    created->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    cleanup();
    setup();
    longest[195] = 0;
    for (u32 slot = 0U; slot < 16U; slot++) media[10][slot * 32U] = 0xe5U;
    created = (vfs_node_t *)kmalloc(sizeof(*created));
    assert(created);
    memset(created, 0, sizeof(*created));
    assert(fat32_create_contents_locked(&root, created, longest,
           0U, replace_source, &source, &facts, &pin) == VFS_OK);
    assert(((fat32_lfn_entry_t *)media[10])[0].order == (0x40U | 15U) &&
           ((fat32_lfn_entry_t *)media[10])[14].order == 1U &&
           ((fat32_on_disk_entry_t *)media[10])[15].attr == 0x20U);
    created->super = &super;
    created->inode = facts.record_id;
    created->type = VFS_TYPE_FILE;
    created->size = facts.size;
    created->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_finddir(&root, longest) == created);
    cleanup();

    setup();
    source.short_at = 0U;
    before = writes;
    assert(fat32_create_contents_locked(&root, &prepared, "Long name.txt",
           3U, replace_source, &source, &facts, &pin) == VFS_ERR_IO &&
           pin && !media[10][0] && writes > before);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           !fs.replace_transaction && fs.version_confident);
    source.short_at = ~(u64)0;
    cleanup();

    setup();
    remove_after_writes = 1U;
    assert(fat32_create_contents_locked(&root, &prepared, "Long name.txt",
           3U, replace_source, &source, &facts, &pin) == VFS_ERR_UNCERTAIN &&
           !pin && !online && active_pin && pin_unresolved &&
           fs.replace_transaction && fs.replace_transaction->creating &&
           !media[10][0]);
    cleanup();

    setup();
    before = writes;
    fail_write_lba = 10U;
    assert(fat32_create_contents_locked(&root, &prepared, "Long name.txt",
           3U, replace_source, &source, &facts, &pin) == VFS_ERR_UNCERTAIN &&
           !pin && active_pin && pin_unresolved && fs.replace_transaction &&
           fs.replace_transaction->phase == FAT32_REPLACE_PUBLISHING &&
           fs.replace_transaction->fresh_count == 1U &&
           memcmp(fs.replace_transaction->old_sector,
                  fs.replace_transaction->proposed_sector, 512U) &&
           writes > before);
    cleanup();
    setup(); /* Reset the deliberately unresolved fixture pin. */
}

/* Coalescing does not change the failure contract or neighboring bytes. */
static void metadata_pair_regressions(void)
{
    for (u32 fault = 0; fault < 6; fault++) {
        setup(); fs.fat_count = 2;
        ((u32 *)media[1])[31] = 0xafffffffU;
        ((u32 *)media[1])[32] = 0xb0000000U;
        memcpy(media[2], media[1], 512);
        u8 original[512]; memcpy(original, media[1], 512);
        fat32_replace_transaction_t tx = {0};
        fs.total_clusters = 300;
        assert(fat32_replace_fat_update_pair(&super, &fs, &tx,
               32, 0, FAT32_EOC_MIN, 128) == VFS_ERR_BAD_FORMAT);
        assert(fat32_replace_fat_update_pair(&super, &fs, &tx,
               32, 0, FAT32_EOC_MIN, 32) == VFS_ERR_BAD_FORMAT);
        assert(!writes && !reads);
        fs.total_clusters = 64;
        if (fault == 1) fail_read_lba = 2;
        if (fault == 2) ((u32 *)media[2])[31] = 7; /* partner mismatch */
        if (fault == 3) fail_write_at = 1;
        if (fault == 4) fail_write_at = 2;
        if (fault == 5) remove_after_writes = 1;
        int result = fat32_replace_fat_update_pair(&super, &fs, &tx,
                                                   32, 0, FAT32_EOC_MIN, 31);
        if (!fault) {
            assert(result == VFS_OK && writes == 2);
            assert(((u32 *)media[1])[31] == 0xa0000020U &&
                   ((u32 *)media[1])[32] == 0xbffffff8U);
            assert(!memcmp(media[1], media[2], 512));
            assert(!memcmp(original, media[1], 124) &&
                   !memcmp(original + 132, media[1] + 132, 380));
        } else if (fault < 3) {
            assert(result == (fault == 1 ? VFS_ERR_IO : VFS_ERR_BAD_FORMAT));
            assert(!writes && !tx.metadata_write_pending);
        } else {
            assert(result == VFS_ERR_UNCERTAIN && tx.metadata_write_pending);
            assert(tx.metadata_cluster == 32 && tx.metadata_partner == 31);
            assert(tx.metadata_copies_completed == (fault == 3 ? 0U : 1U));
            assert(!memcmp(tx.metadata_before, original, 512));
            assert(((u32 *)tx.metadata_proposed)[31] == 0xa0000020U &&
                   ((u32 *)tx.metadata_proposed)[32] == 0xbffffff8U);
            if (fault == 3) {
                /* The torn prefix changes the link, but not candidate entry. */
                assert(((u32 *)media[1])[31] == 0xa0000020U &&
                       ((u32 *)media[1])[32] == 0xb0000000U);
            }
        }
        cleanup();
    }
    setup(); fs.fat_count = 2; fs.bpb.ext_flags = 0x81;
    ((u32 *)media[2])[31] = FAT32_EOC_MIN;
    ((u32 *)media[1])[31] = 7; /* inactive copy is not authoritative */
    fat32_replace_transaction_t active = {0};
    assert(fat32_replace_fat_update_pair(&super, &fs, &active,
               32, 0, FAT32_EOC_MIN, 31) == VFS_OK);
    assert(writes == 1 && ((u32 *)media[1])[31] == 7 &&
           ((u32 *)media[1])[32] == 0 && ((u32 *)media[2])[31] == 32 &&
           active.metadata_copy == 1);
    cleanup();
    setup();
    vfs_node_t *file = replacement_file();
    replace_source_state_t source = {.short_at = 512};
    u64 pin = 0;
    assert(fat32_replace_locked(&super, file->inode, 1, 1024,
                                replace_source, &source, &pin) == VFS_ERR_IO);
    assert(pin && !pin_unresolved && file->size == 3 &&
           ((u32 *)media[1])[4] == 0 && ((u32 *)media[1])[5] == 0 &&
           !memcmp(media[11], "old", 3));
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    cleanup();
    setup();
    file = replacement_file();
    source.short_at = ~(u64)0;
    fail_write_at = 3; /* second allocation plus previous link, torn */
    pin = 0;
    assert(fat32_replace_locked(&super, file->inode, 1, 1024,
                                replace_source, &source, &pin) == VFS_ERR_UNCERTAIN);
    assert(!pin && pin_unresolved && !fs.version_confident && file->size == 3 &&
           fs.replace_transaction->fresh_count == 2 &&
           fs.replace_transaction->metadata_partner == 4 &&
           fs.replace_transaction->metadata_cluster == 5 &&
           fs.replace_transaction->metadata_write_pending &&
           !memcmp(media[11], "old", 3));
    unsigned before = writes;
    assert(fat32_replace_locked(&super, file->inode, 1, 1024,
                                replace_source, &source, &pin) == VFS_ERR_UNCERTAIN);
    assert(writes == before);
    cleanup();
}

static void metadata_view_regressions(void)
{
    setup();
    fat32_read_view_t view = {0};
    u32 next;
    unsigned before = reads, before_allocations = allocations;
    assert(fat32_get_next_checked_view(&super, &fs, 2, &next, &view) == VFS_OK);
    assert(fat32_get_next_checked_view(&super, &fs, 3, &next, &view) == VFS_OK);
    assert(reads == before + 1 && allocations == before_allocations);
    fat32_fs_t other = fs;
    super.private_data = &other;
    assert(fat32_get_next_checked_view(&super, &other, 2, &next, &view) == VFS_OK);
    assert(reads == before + 2 && view.owner == &other);
    super.private_data = &fs;
    assert(fat32_get_next_checked_view(&super, &fs, 2, &next, &view) == VFS_OK);
    before = reads - 1;
    online = false;
    assert(fat32_get_next_checked_view(&super, &fs, 2, &next, &view) == VFS_ERR_IO);
    assert(!view.valid && reads == before + 1);
    online = true;
    fail_read_lba = 1;
    assert(fat32_get_next_checked_view(&super, &fs, 2, &next, &view) == VFS_ERR_IO);
    assert(!view.valid);
    fail_read_lba = ~(u64)0;
    assert(fat32_get_next_checked_view(&super, &fs, 2, &next, &view) == VFS_OK);
    device.id++;
    assert(fat32_get_next_checked_view(&super, &fs, 2, &next, &view) == VFS_ERR_IO);
    assert(!view.valid);
    cleanup();

    setup(); fs.fat_count = 2;
    memcpy(media[2], media[1], 512);
    view.valid = 0;
    before = reads;
    assert(fat32_get_next_checked_view(&super, &fs, 2, &next, &view) == VFS_OK);
    assert(fat32_get_next_checked_view(&super, &fs, 3, &next, &view) == VFS_OK);
    assert(reads == before + 2);
    /* Discard the operation view before a separate metadata mutation. */
    view.valid = 0;
    ((u32 *)media[2])[3] = 8;
    assert(fat32_get_next_checked_view(&super, &fs, 3, &next, &view) ==
           VFS_ERR_BAD_FORMAT);
    view.valid = 0;
    fail_read_lba = 2;
    assert(fat32_get_next_checked_view(&super, &fs, 3, &next, &view) == VFS_ERR_IO);
    assert(!view.valid);
    fail_read_lba = ~(u64)0;
    fs.bpb.ext_flags = 0x81; /* Only the explicitly active FAT is authoritative. */
    view.valid = 0;
    assert(fat32_get_next_checked_view(&super, &fs, 3, &next, &view) == VFS_OK);
    assert(next == 8);
    assert(fat32_get_next_checked_view(&super, &fs, fs.total_clusters,
                                       &next, &view) == VFS_ERR_BAD_FORMAT);
    super.private_data = NULL;
    assert(fat32_get_next_checked_view(&super, &fs, 3, &next, &view) == VFS_ERR_IO);
    assert(!view.valid);
    super.private_data = &fs;
    cleanup();
}

static void metadata_directory_workload(bool fragmented)
{
    setup();
    fs.total_clusters = 500; fs.fat_size_sectors = 4;
    u32 clusters[128];
    for (u32 i = 0; i < 128; i++)
        clusters[i] = i == 0 ? 2U : fragmented ? 499U - 2U * i : 2U + i;
    for (u32 i = 0; i < 128; i++) {
        u32 c = clusters[i];
        ((u32 *)media[1 + c / 128])[c % 128] =
            i + 1 == 128 ? FAT32_EOC_MIN : clusters[i + 1];
        memset(media[10 + c - 2], 0, 512);
        for (u32 at = 0; at < 512; at += 32)
            media[10 + c - 2][at] = 0xe5;
    }
    memset(media[10 + clusters[127] - 2] + 480, 0, 32);
    unsigned before_reads = reads, before_writes = writes;
    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    assert(node); memset(node, 0, sizeof(*node));
    vfs_file_facts_t facts;
    u64 pin = 0;
    assert(fat32_create_contents_locked(&root, node, "BIGDIR.TXT", 0,
           NULL, NULL, &facts, &pin) == VFS_OK);
    assert(pin && facts.size == 0 && root_state.generation == 2);
    printf("FAT32 %s 128-cluster directory create: %u reads, %u writes\n",
           fragmented ? "fragmented" : "contiguous",
           reads - before_reads, writes - before_writes);
    assert(reads - before_reads < 300);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    node->inode = facts.record_id; node->size = facts.size;
    node->super = &super; node->type = VFS_TYPE_FILE;
    node->ops = &fat32_node_ops;
    assert(fat32_finddir(&root, "bigdir.txt") == node);
    before_writes = writes;
    vfs_node_t *other = kmalloc(sizeof(vfs_node_t));
    assert(other); memset(other, 0, sizeof(*other));
    assert(fat32_create_contents_locked(&root, other, "BIGDIR.TXT", 0,
           NULL, NULL, &facts, &pin) == VFS_ERR_ALREADY_EXISTS);
    assert(writes == before_writes);
    kfree(other);
    /* A cycle must still invalidate ownership, not reuse the previous view. */
    u32 c = clusters[126];
    ((u32 *)media[1 + c / 128])[c % 128] = clusters[1];
    assert(fat32_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident);
    cleanup();
}

static void growth_fill_root(bool end_marker)
{
    for (u32 i = 0; i < (end_marker ? 15U : 16U); i++) {
        char short_name[12];
        snprintf(short_name, sizeof(short_name), "F%02u     TXT", i);
        entry(i, short_name, 0U, 0U);
    }
}

static void directory_growth_cases(void)
{
    replace_source_state_t source = {.short_at = ~(u64)0};
    vfs_file_facts_t facts;
    u64 pin;
    setup();
    growth_fill_root(false);
    vfs_node_t *created = (vfs_node_t *)kmalloc(sizeof(*created));
    assert(created);
    memset(created, 0, sizeof(*created));
    unsigned before_reads = reads, before_writes = writes;
    usize baseline = live_requested;
    assert(fat32_create_contents_locked(&root, created, "NEW.TXT", 3U,
           replace_source, &source, &facts, &pin) == VFS_OK);
    assert(pin && fs.replace_transaction->growth &&
           fs.replace_transaction->growth->link_confirmed &&
           ((u32 *)media[1])[2] == 5U && ((u32 *)media[1])[5] == FAT32_EOC_MIN &&
           !memcmp(media[10], "F00     TXT", 11U));
    usize retained = live_requested - baseline;
    unsigned operation_reads = reads - before_reads;
    unsigned operation_writes = writes - before_writes;
    created->super = &super; created->inode = facts.record_id;
    created->size = facts.size; created->type = VFS_TYPE_FILE;
    created->ops = &fat32_node_ops;
    fail_pin_finish = true;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_ERR_BUSY &&
           fs.replace_transaction && !fs.version_confident && active_pin &&
           !fat32_finddir(&root, "new.txt"));
    fail_pin_finish = false;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_finddir(&root, "new.txt") == created &&
           fat32_ownership_preflight_locked(&root) == VFS_OK);
    printf("FAT32 full-directory growth: %zu peak, %zu retained bytes, %u reads, %u writes, %u flushes\n",
           (size_t)(peak_requested - baseline), (size_t)retained,
           operation_reads, operation_writes, flushes);
    cleanup();

    setup();
    growth_fill_root(true);
    ((u32 *)media[1])[2] = 8U;
    ((u32 *)media[1])[8] = FAT32_EOC_MIN;
    created = (vfs_node_t *)kmalloc(sizeof(*created));
    assert(created);
    memset(created, 0, sizeof(*created));
    int result = fat32_create_contents_locked(&root, created, "Long name.txt", 0U,
           NULL, NULL, &facts, &pin);
    assert(result == VFS_OK);
    assert(fs.replace_transaction->growth);
    assert(!fs.replace_transaction->growth->append);
    assert(fs.replace_transaction->growth->entry_written);
    assert(fs.replace_transaction->growth->marker_attempted);
    assert(media[10][15U * 32U] == 0xe5U);
    assert(((fat32_on_disk_entry_t *)media[16])[1].attr == 0x20U);
    created->super = &super; created->inode = facts.record_id;
    created->size = facts.size; created->type = VFS_TYPE_FILE;
    created->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_finddir(&root, "Long name.txt") == created &&
           fat32_ownership_preflight_locked(&root) == VFS_OK);
    cleanup();

    setup();
    fs.sectors_per_cluster = 2U;
    fs.cluster_size_bytes = 1024U;
    growth_fill_root(true);
    created = (vfs_node_t *)kmalloc(sizeof(*created));
    assert(created); memset(created, 0, sizeof(*created));
    assert(fat32_create_contents_locked(&root, created, "Long name.txt", 0U,
           NULL, NULL, &facts, &pin) == VFS_OK &&
           !fs.replace_transaction->growth->append &&
           fs.replace_transaction->growth->target_cluster == 2U &&
           fs.replace_transaction->growth->target_slot == 17U &&
           ((u32 *)media[1])[2] == FAT32_EOC_MIN &&
           media[10][15U * 32U] == 0xe5U);
    created->super = &super; created->inode = facts.record_id;
    created->size = facts.size; created->type = VFS_TYPE_FILE;
    created->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_finddir(&root, "Long name.txt") == created);
    cleanup();

    setup(); growth_fill_root(true);
    char longest[196];
    memset(longest, 'a', 195U); longest[195] = 0;
    created = (vfs_node_t *)kmalloc(sizeof(*created));
    assert(created); memset(created, 0, sizeof(*created));
    assert(fat32_create_contents_locked(&root, created, longest, 0U,
           NULL, NULL, &facts, &pin) == VFS_OK &&
           fs.replace_transaction->growth->append &&
           ((fat32_on_disk_entry_t *)media[12])[15].attr == 0x20U);
    created->super = &super; created->inode = facts.record_id;
    created->size = facts.size; created->type = VFS_TYPE_FILE;
    created->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_finddir(&root, longest) == created &&
           fat32_ownership_preflight_locked(&root) == VFS_OK);
    cleanup();

    for (u32 failed = 1U; failed <= 5U; failed++) {
        setup(); growth_fill_root(true);
        fail_write_at = failed;
        assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
               "Long name.txt", 0U, NULL, NULL, &facts, &pin) ==
               VFS_ERR_UNCERTAIN && !pin && active_pin && pin_unresolved &&
               fs.replace_transaction && fs.replace_transaction->growth &&
               !fs.version_confident);
        if (failed == 3U)
            assert(fs.replace_transaction->growth->link_attempted &&
                   fs.replace_transaction->growth->candidate_allocated &&
                   ((u32 *)media[1])[2] == 4U &&
                   ((u32 *)media[1])[4] == FAT32_EOC_MIN);
        assert(!fat32_finddir(&root, "Long name.txt"));
        cleanup();
    }
    setup(); growth_fill_root(true);
    fail_write_at = 5U;
    fail_write_partial_bytes = 481U; /* Torn marker write makes entry visible. */
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
           "Long name.txt", 0U, NULL, NULL, &facts, &pin) ==
           VFS_ERR_UNCERTAIN && !pin && pin_unresolved &&
           media[10][15U * 32U] == 0xe5U &&
           fs.replace_transaction->growth->marker_attempted &&
           !fs.version_confident && !fat32_finddir(&root, "Long name.txt"));
    assert(fs.replace_transaction->growth->marker_before[480U] == 0U &&
           fs.replace_transaction->growth->marker_proposed[480U] == 0xe5U &&
           fs.replace_transaction->growth->fat_sector[0][0] == 1U &&
           fs.replace_transaction->growth->fat_sector[1][0] == 1U &&
           memcmp(fs.replace_transaction->old_sector,
                  fs.replace_transaction->proposed_sector, 512U));
    cleanup();

    setup(); growth_fill_root(true);
    fs.reserved_sector_count = 8U;
    fs.fs_info_sector = 5U;
    fat32_store_le32(media[5], 0x41615252U);
    fat32_store_le32(media[5] + 484U, 0x61417272U);
    fat32_store_le32(media[5] + 508U, 0xaa550000U);
    fail_write_lba = 5U; /* Advisory cleanup after confirmed visibility. */
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
           "Long name.txt", 0U, NULL, NULL, &facts, &pin) ==
           VFS_ERR_UNCERTAIN && !pin && pin_unresolved &&
           media[10][15U * 32U] == 0xe5U &&
           fs.replace_transaction->phase == FAT32_REPLACE_FSINFO &&
           fs.replace_transaction->growth->entry_written &&
           fs.replace_transaction->growth->marker_attempted &&
           fs.replace_transaction->metadata_sector == 5U);
    cleanup();
    for (u32 failed = 1U; failed <= 5U; failed++) {
        setup(); growth_fill_root(true);
        fail_flush_at = failed;
        assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
               "Long name.txt", 0U, NULL, NULL, &facts, &pin) ==
               VFS_ERR_UNCERTAIN && !pin && active_pin && pin_unresolved &&
               fs.replace_transaction && !fs.version_confident);
        cleanup();
    }
    setup(); growth_fill_root(true);
    remove_after_writes = 3U; /* Link confirmed, flush sees removal. */
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
           "Long name.txt", 0U, NULL, NULL, &facts, &pin) ==
           VFS_ERR_UNCERTAIN && !pin && active_pin && pin_unresolved &&
           fs.replace_transaction->growth->link_confirmed && !online);
    cleanup();

    setup(); growth_fill_root(false);
    fs.fat_count = 2U;
    memcpy(media[2], media[1], 512U);
    fail_write_at = 2U; /* Second FAT copy of directory allocation. */
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
           "NEW.TXT", 0U, NULL, NULL, &facts, &pin) ==
           VFS_ERR_UNCERTAIN && !pin && pin_unresolved &&
           fs.replace_transaction->growth->candidate_recorded &&
           fs.replace_transaction->growth->fat_copies_completed[0] == 1U);
    cleanup();

    setup(); growth_fill_root(false);
    fs.fat_count = 2U;
    memcpy(media[2], media[1], 512U);
    fail_write_at = 5U; /* Second mirror of the old directory tail link. */
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
           "NEW.TXT", 0U, NULL, NULL, &facts, &pin) ==
           VFS_ERR_UNCERTAIN && !pin && pin_unresolved &&
           fs.replace_transaction->growth->link_attempted &&
           !fs.replace_transaction->growth->link_confirmed &&
           fs.replace_transaction->growth->fat_copies_completed[1] == 1U &&
           fs.replace_transaction->growth->candidate_allocated);
    cleanup();

    setup(); growth_fill_root(false);
    fs.fat_count = 2U;
    memcpy(media[2], media[1], 512U);
    ((u32 *)media[2])[4] = FAT32_EOC_MIN; /* Candidate disagrees across mirrors. */
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
           "NEW.TXT", 0U, NULL, NULL, &facts, &pin) ==
           VFS_ERR_UNCERTAIN && !pin && pin_unresolved && !writes &&
           fs.replace_transaction->growth && !fs.version_confident);
    cleanup();

    setup(); growth_fill_root(false);
    fail_fat_read_after_writes = 2U; /* Link preparation, before its write. */
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
           "NEW.TXT", 0U, NULL, NULL, &facts, &pin) == VFS_ERR_IO &&
           pin && !((u32 *)media[1])[4] &&
           ((u32 *)media[1])[2] == FAT32_EOC_MIN);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           !fs.replace_transaction && fs.version_confident);
    cleanup();

    setup(); growth_fill_root(false);
    fail_fat_read_after_writes = 2U;
    fail_write_at = 3U; /* Candidate cancellation after definite link-read fault. */
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
           "NEW.TXT", 0U, NULL, NULL, &facts, &pin) == VFS_ERR_UNCERTAIN &&
           !pin && active_pin && pin_unresolved &&
           fs.replace_transaction->growth->candidate_recorded &&
           !fs.replace_transaction->growth->link_attempted);
    cleanup();

    setup(); growth_fill_root(false);
    source.short_at = 0U;
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
           "NEW.TXT", 3U, replace_source, &source, &facts, &pin) ==
           VFS_ERR_IO && pin && ((u32 *)media[1])[2] == FAT32_EOC_MIN);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fs.version_confident);
    source.short_at = ~(u64)0;
    created = (vfs_node_t *)kmalloc(sizeof(*created));
    assert(created); memset(created, 0, sizeof(*created));
    assert(fat32_create_contents_locked(&root, created, "NEW.TXT", 0U,
           NULL, NULL, &facts, &pin) == VFS_OK && pin);
    created->super = &super; created->inode = facts.record_id;
    created->size = facts.size; created->type = VFS_TYPE_FILE;
    created->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_finddir(&root, "NEW.TXT") == created);
    cleanup();
    puts("FAT32 directory-growth publication, flush and removal faults passed");
}

static void directory_growth_large_case(void)
{
    replace_source_state_t source = {.short_at = ~(u64)0};
    vfs_file_facts_t facts;
    u64 pin = 0;
    setup();
    growth_fill_root(false);
    memcpy(media[40], media[10], 512U);
    memset(media[10], 0, 512U);
    fs.total_clusters = 4200U;
    fs.fat_size_sectors = 33U;
    fs.data_start_lba = 40U;
    vfs_node_t *created = (vfs_node_t *)kmalloc(sizeof(*created));
    assert(created); memset(created, 0, sizeof(*created));
    usize baseline = live_requested;
    peak_requested = baseline;
    reads = writes = flushes = 0U;
    clock_t began = clock();
    assert(fat32_create_contents_locked(&root, created, "BIG.TXT",
           FAT32_REPLACE_MAX_BYTES, replace_source, &source,
           &facts, &pin) == VFS_OK && pin &&
           fs.replace_transaction->fresh_count == 4096U &&
           fs.replace_transaction->growth->candidate == 4100U &&
           ((u32 *)media[1])[2] == 4100U);
    double cpu_ms = 1000.0 * (double)(clock() - began) / CLOCKS_PER_SEC;
    usize peak = peak_requested - baseline;
    usize retained = live_requested - baseline;
    unsigned operation_reads = reads, operation_writes = writes;
    created->super = &super; created->inode = facts.record_id;
    created->size = facts.size; created->type = VFS_TYPE_FILE;
    created->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    u8 last;
    assert(fat32_read(created, FAT32_REPLACE_MAX_BYTES - 1U, 1U, &last) == 1U &&
           last == (u8)(FAT32_REPLACE_MAX_BYTES - 1U));
    printf("FAT32 2 MiB growth: %zu peak, %zu retained bytes, %u reads, %u writes, %u flushes, %.3f host CPU ms\n",
           (size_t)peak, (size_t)retained, operation_reads,
           operation_writes, flushes, cpu_ms);
    cleanup();
}

static void directory_growth_large_cluster_case(void)
{
    setup();
    fs.sectors_per_cluster = 128U;
    fs.cluster_size_bytes = 65536U;
    for (u32 index = 0U; index < 2048U; index++) {
        char name[12];
        snprintf(name, sizeof(name), "F%07uTXT", index);
        fat32_on_disk_entry_t *entry =
            &((fat32_on_disk_entry_t *)media[10U + index / 16U])[index % 16U];
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->name, name, 11U);
        entry->attr = 0x20U;
    }
    vfs_node_t *created = (vfs_node_t *)kmalloc(sizeof(*created));
    assert(created); memset(created, 0, sizeof(*created));
    usize baseline = live_requested;
    peak_requested = baseline;
    vfs_file_facts_t facts;
    u64 pin = 0U;
    assert(fat32_create_contents_locked(&root, created, "BIG.TXT", 0U,
           NULL, NULL, &facts, &pin) == VFS_OK && pin &&
           fs.replace_transaction->growth->append &&
           fs.replace_transaction->growth->zero_sectors_completed == 128U);
    printf("FAT32 64 KiB-cluster growth: %zu peak, %zu retained requested bytes\n",
           (size_t)(peak_requested - baseline),
           (size_t)(live_requested - baseline));
    created->super = &super; created->inode = facts.record_id;
    created->size = facts.size; created->type = VFS_TYPE_FILE;
    created->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_finddir(&root, "BIG.TXT") == created);
    cleanup();
}

/* The caller supplies a disposable, externally formatted image whose root
 * has no free sector-sized run. This path uses production mount and create. */
static void directory_growth_image_case(const char *path)
{
    replace_source_state_t source = {.short_at = ~(u64)0};
    image_file = fopen(path, "r+b");
    assert(image_file && fseeko(image_file, 0, SEEK_END) == 0);
    off_t bytes = ftello(image_file);
    assert(bytes > 0 && bytes % 512 == 0);
    image_sectors = (u64)bytes / 512U;
    image_writable = true;
    device.id = 904U;
    device.sector_size = 512U;
    device.sector_count = image_sectors;
    online = true;
    vfs_super_t *mounted = NULL;
    assert(fat32_mount(&fat32_fs_type, &device, &mounted, false) == VFS_OK);
    pin_super = mounted;
    vfs_node_t *created = (vfs_node_t *)kmalloc(sizeof(*created));
    assert(created); memset(created, 0, sizeof(*created));
    vfs_file_facts_t facts;
    u64 pin = 0U;
    assert(fat32_create_contents_locked(mounted->root_node, created,
           "Mangrove grown.txt", 9U, replace_source, &source,
           &facts, &pin) == VFS_OK && pin &&
           ((fat32_fs_t *)mounted->private_data)->replace_transaction->growth);
    created->super = mounted; created->inode = facts.record_id;
    created->size = facts.size; created->type = VFS_TYPE_FILE;
    created->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_finddir(mounted->root_node, "MANGROVE GROWN.TXT") == created);
    u8 data[9];
    assert(fat32_read(created, 0U, sizeof(data), data) == sizeof(data));
    for (u32 i = 0; i < sizeof(data); i++) assert(data[i] == i);
    fat32_fs_t *mounted_fs = (fat32_fs_t *)mounted->private_data;
    vfs_node_t *nodes[4];
    u32 count = 0U;
    for (fat32_node_state_t *state = mounted_fs->nodes; state;
         state = state->next) {
        assert(count < 4U);
        nodes[count++] = state->node;
    }
    assert(fat32_unmount(mounted) == VFS_OK);
    for (u32 i = 0U; i < count; i++) kfree(nodes[i]);
    kfree(mounted);
    fclose(image_file);
    image_file = NULL;
    image_writable = false;
    pin_super = &super;
    puts("FAT32 disposable image: directory growth, LFN and readback passed");
}

/* Build an imported LFN whose physical slots cross a sector boundary and,
 * for the long form, a cluster boundary. Every entry is formatter-style
 * input; unlink and validation below execute the production driver. */
static void retirement_fixture(char name[256], bool long_form,
                               bool two_sector_cluster)
{
    u16 units[255];
    u32 unit_count;
    setup();
    if (two_sector_cluster) {
        fs.sectors_per_cluster = 2U;
        fs.cluster_size_bytes = 1024U;
    }
    if (long_form) {
        memset(name, 'a', 255U);
        name[255] = 0;
    } else {
        strcpy(name, "Cross sector long filename.txt");
    }
    assert(fat32_create_lfn_units(name, units, &unit_count));
    u32 lfn_count = (unit_count + 12U) / 13U;
    u32 needed_sectors = (14U + lfn_count + 1U + 15U) / 16U;
    u32 needed_clusters = (needed_sectors + fs.sectors_per_cluster - 1U) /
                          fs.sectors_per_cluster;
    u32 chain[3] = {2U, 4U, 6U};
    for (u32 i = 0U; i < needed_clusters; i++)
        ((u32 *)media[1])[chain[i]] = i + 1U == needed_clusters
            ? FAT32_EOC_MIN : chain[i + 1U];
    ((u32 *)media[1])[5] = FAT32_EOC_MIN;
    media[fat32_cluster_to_lba(&fs, 5U)][0] = 0x5aU;
    for (u32 i = 0U; i < 14U; i++) {
        char alias[12];
        snprintf(alias, sizeof(alias), "F%02u     TXT", i);
        entry(i, alias, 0U, 0U);
    }
    const char alias[11] = {'L','O','N','G','F','I','~','1','T','X','T'};
    u8 checksum = fat32_lfn_checksum(alias);
    for (u32 i = 0U; i < lfn_count; i++) {
        u32 physical = 14U + i;
        u32 sector_index = physical / 16U;
        u32 cluster = chain[sector_index / fs.sectors_per_cluster];
        u32 part = sector_index % fs.sectors_per_cluster;
        fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)(
            media[fat32_cluster_to_lba(&fs, cluster) + part] +
            (physical % 16U) * 32U);
        u32 sequence = lfn_count - i;
        u16 words[13];
        memset(lfn, 0, sizeof(*lfn));
        for (u32 j = 0U; j < 13U; j++) {
            u32 offset = (sequence - 1U) * 13U + j;
            words[j] = offset < unit_count ? units[offset] :
                       offset == unit_count ? 0U : 0xffffU;
        }
        lfn->order = (u8)sequence | (i == 0U ? 0x40U : 0U);
        lfn->attr = 0x0fU;
        lfn->checksum = checksum;
        memcpy(lfn->name1, words, 5U * sizeof(u16));
        memcpy(lfn->name2, words + 5U, 6U * sizeof(u16));
        memcpy(lfn->name3, words + 11U, 2U * sizeof(u16));
    }
    u32 physical = 14U + lfn_count;
    u32 sector_index = physical / 16U;
    u32 cluster = chain[sector_index / fs.sectors_per_cluster];
    u32 part = sector_index % fs.sectors_per_cluster;
    fat32_on_disk_entry_t *short_entry = (fat32_on_disk_entry_t *)(
        media[fat32_cluster_to_lba(&fs, cluster) + part] +
        (physical % 16U) * 32U);
    memset(short_entry, 0, sizeof(*short_entry));
    memcpy(short_entry->name, alias, 11U);
    short_entry->attr = 0x20U;
    short_entry->first_cluster_low = 5U;
    short_entry->file_size = 1U;
}

static void retirement_directory_fixture(char name[256], bool long_form,
                                         bool two_sector_cluster,
                                         bool multiple_clusters)
{
    retirement_fixture(name, long_form, two_sector_cluster);
    fat32_walk_entry_t match;
    assert(fat32_dir_find(&root, name, &match) == VFS_OK);
    fat32_on_disk_entry_t *short_entry = (fat32_on_disk_entry_t *)(
        media[fat32_cluster_to_lba(&fs, match.cluster) + match.slot / 16U] +
        (match.slot % 16U) * 32U);
    short_entry->attr = 0x10U;
    short_entry->file_size = 0U;
    u64 first_lba = fat32_cluster_to_lba(&fs, 5U);
    memset(media[first_lba], 0, fs.cluster_size_bytes);
    fat32_on_disk_entry_t *dot = (fat32_on_disk_entry_t *)media[first_lba];
    memcpy(dot[0].name, ".          ", 11U);
    dot[0].attr = 0x10U;
    dot[0].first_cluster_low = 5U;
    memcpy(dot[1].name, "..         ", 11U);
    dot[1].attr = 0x10U;
    if (multiple_clusters) {
        ((u32 *)media[1])[5] = 7U;
        ((u32 *)media[1])[7] = 8U;
        ((u32 *)media[1])[8] = FAT32_EOC_MIN;
    }
}

static void large_empty_directory_fixture(void)
{
    setup();
    fs.sectors_per_cluster = 2U;
    fs.cluster_size_bytes = 1024U;
    fs.fat_size_sectors = 17U;
    fs.data_start_lba = 32U;
    fs.total_clusters = 2060U;
    const u32 last = 2053U; /* 2,098,176 mapped directory bytes. */
    for (u32 cluster = 2U; cluster <= last; cluster++) {
        u32 value = cluster == 2U || cluster == last ? FAT32_EOC_MIN :
                    cluster >= 5U ? cluster + 1U : 0U;
        if (value)
            ((u32 *)media[1U + cluster / 128U])[cluster % 128U] = value;
    }
    u64 root_lba = fat32_cluster_to_lba(&fs, 2U);
    fat32_on_disk_entry_t *entry = (fat32_on_disk_entry_t *)media[root_lba];
    memcpy(entry[0].name, "LARGE   DIR", 11U);
    entry[0].attr = 0x10U;
    entry[0].first_cluster_low = 5U;
    u64 first_lba = fat32_cluster_to_lba(&fs, 5U);
    fat32_on_disk_entry_t *dot = (fat32_on_disk_entry_t *)media[first_lba];
    memcpy(dot[0].name, ".          ", 11U);
    dot[0].attr = 0x10U;
    dot[0].first_cluster_low = 5U;
    memcpy(dot[1].name, "..         ", 11U);
    dot[1].attr = 0x10U;
}

static void same_sector_directory_fixture(const char *name)
{
    setup();
    u16 units[255];
    u32 count;
    assert(fat32_create_lfn_units(name, units, &count));
    u32 total = (count + 12U) / 13U;
    assert(total && total < 15U);
    const char alias[11] = {'D','I','R','E','C','T','~','1','T','X','T'};
    for (u32 i = 0U; i < total; i++) {
        fat32_lfn_entry_t entry = fat32_create_lfn_entry(units, count,
            total - i, total, alias);
        memcpy(media[10] + i * 32U, &entry, 32U);
    }
    fat32_on_disk_entry_t *short_entry =
        &((fat32_on_disk_entry_t *)media[10])[total];
    memcpy(short_entry->name, alias, 11U);
    short_entry->attr = 0x10U;
    short_entry->first_cluster_low = 5U;
    ((u32 *)media[1])[5] = FAT32_EOC_MIN;
    fat32_on_disk_entry_t *dot = (fat32_on_disk_entry_t *)media[13];
    memcpy(dot[0].name, ".          ", 11U);
    dot[0].attr = 0x10U;
    dot[0].first_cluster_low = 5U;
    memcpy(dot[1].name, "..         ", 11U);
    dot[1].attr = 0x10U;
}

static void cross_lfn_retirement_cases(void)
{
    char name[256];
    u64 pin;
    for (u32 layout = 0U; layout < 3U; layout++) {
        retirement_fixture(name, layout != 0U, layout == 2U);
        vfs_node_t *file = fat32_finddir(&root, name);
        assert(file && fat32_ownership_preflight_locked(file) == VFS_OK);
        usize baseline = live_requested;
        peak_requested = baseline;
        unsigned before_reads = reads, before_writes = writes;
        clock_t cpu_start = clock();
        pin = 0U;
        assert(fat32_unlink_pinned(&root, name, &pin) == VFS_OK && pin &&
               fs.replace_transaction &&
               fs.replace_transaction->retirement->count ==
                   (layout == 0U ? 2U : 3U) &&
               ((u32 *)media[1])[5] == 0U);
        usize retained = live_requested - baseline;
        double cpu_ms = (double)(clock() - cpu_start) * 1000.0 /
                        (double)CLOCKS_PER_SEC;
        printf("FAT32 cross-sector LFN unlink layout %u: peak=%zu retained=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
               layout, (size_t)(peak_requested - baseline), (size_t)retained,
               reads - before_reads, writes - before_writes, flushes,
               cpu_ms);
        assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
               fat32_finddir(&root, name) == NULL &&
               fat32_ownership_preflight_locked(&root) == VFS_OK);
        assert(fat32_unlink_pinned(&root, name, &pin) == VFS_ERR_NOT_FOUND &&
               !pin);
        vfs_node_t *reused = NULL;
        assert(create_finished(&root, "REUSED.TXT", &reused) == VFS_OK && reused &&
               fat32_finddir(&root, name) == NULL &&
               fat32_ownership_preflight_locked(reused) == VFS_OK);
        cleanup();
    }
    for (u32 failure = 1U; failure <= 4U; failure++) {
        retirement_fixture(name, true, false);
        assert(fat32_finddir(&root, name));
        fail_write_at = failure;
        pin = 0U;
        assert(fat32_unlink_pinned(&root, name, &pin) == VFS_ERR_UNCERTAIN &&
               !pin && active_pin && pin_unresolved &&
               fs.replace_transaction && fs.replace_transaction->retirement &&
               !fs.version_confident);
        if (failure <= 3U) assert(((u32 *)media[1])[5] == FAT32_EOC_MIN);
        assert(fs.replace_transaction->retirement->write_attempted ==
               (failure < 4U ? failure : 3U));
        cleanup();
    }
    for (u32 failure = 1U; failure <= 3U; failure++) {
        retirement_fixture(name, true, false);
        assert(fat32_finddir(&root, name));
        fail_flush_at = failure;
        pin = 0U;
        assert(fat32_unlink_pinned(&root, name, &pin) == VFS_ERR_UNCERTAIN &&
               !pin && active_pin && pin_unresolved &&
               fs.replace_transaction && !fs.version_confident);
        if (failure <= 2U) assert(((u32 *)media[1])[5] == FAT32_EOC_MIN);
        cleanup();
    }
    for (u32 failure = 1U; failure <= 7U; failure++) {
        retirement_fixture(name, true, false);
        fail_allocation = failure;
        pin = 0U;
        unsigned before = writes;
        int result = fat32_unlink_pinned(&root, name, &pin);
        assert(result == VFS_ERR_NO_MEM && !pin && !active_pin &&
               !fs.replace_transaction && writes == before &&
               fs.version_confident);
        cleanup();
    }
    retirement_fixture(name, true, false);
    fail_pin_begin = true;
    pin = 0U;
    unsigned before_pin_writes = writes;
    assert(fat32_unlink_pinned(&root, name, &pin) == VFS_ERR_NO_SPACE &&
           !pin && !active_pin && !fs.replace_transaction &&
           writes == before_pin_writes && fs.version_confident);
    cleanup();

    retirement_fixture(name, true, false);
    fail_read_lba = fat32_cluster_to_lba(&fs, 6U);
    pin = 0U;
    unsigned before_read_writes = writes;
    assert(fat32_unlink_pinned(&root, name, &pin) == VFS_ERR_IO &&
           !pin && !active_pin && !fs.replace_transaction &&
           writes == before_read_writes && fs.version_confident);
    cleanup();

    retirement_fixture(name, true, false);
    fs.fat_count = 2U;
    memcpy(media[2], media[1], 512U);
    fail_write_at = 5U; /* Three directory sectors, then FAT copy 0 and 1. */
    pin = 0U;
    assert(fat32_unlink_pinned(&root, name, &pin) == VFS_ERR_UNCERTAIN &&
           !pin && active_pin && pin_unresolved &&
           fs.replace_transaction->reclamation_cursor == 0U &&
           fs.replace_transaction->metadata_copy == 1U &&
           fs.replace_transaction->metadata_copies_completed == 1U);
    cleanup();

    retirement_fixture(name, true, false);
    assert(fat32_finddir(&root, name));
    remove_after_writes = 1U;
    pin = 0U;
    assert(fat32_unlink_pinned(&root, name, &pin) == VFS_ERR_UNCERTAIN &&
           !pin && !online && active_pin && pin_unresolved &&
           ((u32 *)media[1])[5] == FAT32_EOC_MIN);
    cleanup();

    retirement_fixture(name, true, false);
    assert(fat32_finddir(&root, name));
    u32 directory_short_slot = 14U + 20U;
    fat32_on_disk_entry_t *directory_short =
        (fat32_on_disk_entry_t *)(media[fat32_cluster_to_lba(&fs, 6U)] +
                                  (directory_short_slot % 16U) * 32U);
    directory_short->attr = 0x10U;
    unsigned before_directory = writes;
    assert(fat32_rmdir(&root, name) == VFS_ERR_UNSUPPORTED &&
           writes == before_directory && fs.version_confident);
    cleanup();

    retirement_fixture(name, true, false);
    assert(fat32_finddir(&root, name));
    u32 short_slot = 14U + 20U;
    u32 short_cluster = 6U;
    media[fat32_cluster_to_lba(&fs, short_cluster) +
          short_slot / 16U - 2U][(short_slot % 16U) * 32U] = 0xe5U;
    unsigned before = writes;
    assert(fat32_check_name_available_locked_view(&root, "NEW.TXT", NULL) ==
           VFS_ERR_BAD_FORMAT && !fs.version_confident && writes == before);
    cleanup();
}

static void pinned_directory_retirement_cases(void)
{
    char name[256];
    u64 pin;
    same_sector_directory_fixture("Directory long.txt");
    assert(fat32_finddir(&root, "Directory long.txt"));
    assert(fat32_rmdir_pinned(&root, "Directory long.txt", &pin) ==
           VFS_OK && pin && fs.replace_transaction->retirement->count == 1U &&
           vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_ownership_preflight_locked(&root) == VFS_OK);
    cleanup();

    for (u32 layout = 0U; layout < 3U; layout++) {
        retirement_directory_fixture(name, layout != 0U, layout == 2U,
                                     layout == 2U);
        vfs_node_t *opened = fat32_finddir(&root, name);
        assert(opened && opened->type == VFS_TYPE_DIRECTORY);
        assert(fat32_ownership_preflight_locked(&root) == VFS_OK);
        usize baseline = live_requested;
        peak_requested = baseline;
        unsigned before_reads = reads, before_writes = writes;
        clock_t started = clock();
        pin = 0U;
        assert(fat32_rmdir_pinned(&root, name, &pin) == VFS_OK && pin &&
               fs.namespace_hidden && !fat32_finddir(&root, name) &&
               fs.replace_transaction->retirement->count ==
                   (layout == 0U ? 2U : 3U) &&
               fs.replace_transaction->old_count ==
                   (layout == 2U ? 3U : 1U));
        usize retained = live_requested - baseline;
        printf("FAT32 pinned LFN rmdir layout %u: peak=%zu retained=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
               layout, (size_t)(peak_requested - baseline),
               (size_t)retained, reads - before_reads, writes - before_writes,
               flushes, (double)(clock() - started) * 1000.0 / CLOCKS_PER_SEC);
        assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
               !fs.namespace_hidden && !fat32_state(opened)->alive &&
               !fat32_readdir(opened, 0U, &(vfs_dirent_t){0}) &&
               fat32_ownership_preflight_locked(&root) == VFS_OK);
        assert(fat32_rmdir_pinned(&root, name, &pin) == VFS_ERR_NOT_FOUND &&
               !pin);
        vfs_node_t *reused = NULL;
        assert(create_finished(&root, "REUSED.TXT", &reused) == VFS_OK && reused &&
               reused->inode != opened->inode &&
               !fat32_readdir(opened, 0U, &(vfs_dirent_t){0}));
        cleanup();
    }

    retirement_directory_fixture(name, true, false, true);
    entry_at(5U, 2U, "CHILD   TXT", 0U, 0U, 0x20U);
    unsigned before = writes;
    assert(fat32_rmdir_pinned(&root, name, &pin) == VFS_ERR_NOT_EMPTY &&
           !pin && writes == before && fs.version_confident);
    cleanup();

    retirement_directory_fixture(name, true, false, true);
    media[10][0] = 0x82U; /* Imported OEM alias of unknown encoding. */
    before = writes;
    assert(fat32_rmdir_pinned(&root, name, &pin) == VFS_ERR_UNSUPPORTED &&
           !pin && writes == before && fs.version_confident);
    cleanup();

    retirement_directory_fixture(name, true, false, true);
    fail_read_lba = fat32_cluster_to_lba(&fs, 5U);
    before = writes;
    assert(fat32_rmdir_pinned(&root, name, &pin) == VFS_ERR_IO &&
           !pin && writes == before && fs.version_confident);
    cleanup();

    retirement_directory_fixture(name, true, false, true);
    fat32_walk_entry_t match;
    assert(fat32_dir_find(&root, name, &match) == VFS_OK);
    media[fat32_cluster_to_lba(&fs, match.lfn.cluster[0]) +
          match.lfn.slot[0] / 16U]
         [(match.lfn.slot[0] % 16U) * 32U + 13U] ^= 1U;
    before = writes;
    assert(fat32_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT &&
           writes == before && !fs.version_confident);
    cleanup();

    retirement_directory_fixture(name, true, false, true);
    ((fat32_on_disk_entry_t *)media[13])[0].first_cluster_low = 7U;
    before = writes;
    assert(fat32_rmdir_pinned(&root, name, &pin) == VFS_ERR_BAD_FORMAT &&
           !pin && writes == before && !fs.version_confident);
    cleanup();

    retirement_directory_fixture(name, true, false, true);
    vfs_node_t *stale = fat32_finddir(&root, name);
    assert(stale);
    fat32_state(stale)->first_cluster = 7U;
    before = writes;
    assert(fat32_rmdir_pinned(&root, name, &pin) == VFS_ERR_BAD_FORMAT &&
           !pin && writes == before && !fs.version_confident);
    cleanup();

    for (u32 failure = 1U; failure <= 6U; failure++) {
        retirement_directory_fixture(name, true, false, true);
        assert(fat32_finddir(&root, name));
        fail_write_at = failure;
        fail_write_partial_bytes = 128U;
        pin = 0U;
        assert(fat32_rmdir_pinned(&root, name, &pin) == VFS_ERR_UNCERTAIN &&
               !pin && active_pin && pin_unresolved &&
               fs.namespace_hidden && fs.replace_transaction &&
               fs.replace_transaction->old_count == 3U &&
               fs.replace_transaction->retirement->count == 3U &&
               fs.replace_transaction->reclamation_cursor ==
                   (failure > 3U ? failure - 4U : 0U));
        unsigned attempts = writes;
        assert(fat32_rmdir_pinned(&root, name, &pin) == VFS_ERR_UNCERTAIN &&
               !pin && writes == attempts && fs.replace_transaction);
        cleanup();
    }
    for (u32 failure = 1U; failure <= 3U; failure++) {
        retirement_directory_fixture(name, true, false, true);
        fail_flush_at = failure;
        assert(fat32_rmdir_pinned(&root, name, &pin) == VFS_ERR_UNCERTAIN &&
               !pin && active_pin && pin_unresolved &&
               fs.namespace_hidden && fs.replace_transaction);
        cleanup();
    }
    retirement_directory_fixture(name, true, false, true);
    fs.fat_count = 2U;
    fs.fat_size_sectors = 1U;
    memcpy(media[2], media[1], 512U);
    fail_write_at = 5U;
    assert(fat32_rmdir_pinned(&root, name, &pin) == VFS_ERR_UNCERTAIN &&
           !pin && active_pin && pin_unresolved &&
           fs.replace_transaction->reclamation_cursor == 0U);
    cleanup();

    retirement_directory_fixture(name, true, false, true);
    remove_after_writes = 1U;
    assert(fat32_rmdir_pinned(&root, name, &pin) == VFS_ERR_UNCERTAIN &&
           !pin && !online && active_pin && pin_unresolved &&
           fs.replace_transaction && fs.namespace_hidden);
    cleanup();

    retirement_directory_fixture(name, true, false, true);
    fail_fat_read_after_writes = 3U;
    assert(fat32_rmdir_pinned(&root, name, &pin) == VFS_ERR_UNCERTAIN &&
           !pin && active_pin && pin_unresolved &&
           fs.replace_transaction->reclamation_cursor == 0U);
    cleanup();

    retirement_directory_fixture(name, true, false, true);
    fs.reserved_sector_count = 8U;
    fs.fs_info_sector = 5U;
    fat32_store_le32(media[5], 0x41615252U);
    fat32_store_le32(media[5] + 484U, 0x61417272U);
    fat32_store_le32(media[5] + 508U, 0xaa550000U);
    fail_write_lba = 5U;
    assert(fat32_rmdir_pinned(&root, name, &pin) == VFS_ERR_UNCERTAIN &&
           !pin && active_pin && pin_unresolved &&
           fs.replace_transaction->phase == FAT32_REPLACE_FSINFO &&
           fs.replace_transaction->reclamation_cursor == 3U);
    cleanup();

    retirement_directory_fixture(name, true, false, true);
    assert(fat32_rmdir_pinned(&root, name, &pin) == VFS_OK && pin &&
           fs.replace_transaction && fs.namespace_hidden);
    fail_pin_finish = true;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_ERR_BUSY &&
           fs.replace_transaction && fs.namespace_hidden);
    fail_pin_finish = false;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           !fs.replace_transaction && !fs.namespace_hidden);
    cleanup();

    for (u32 failure = 1U; failure <= 9U; failure++) {
        retirement_directory_fixture(name, true, false, true);
        fail_allocation = allocations + failure;
        before = writes;
        int result = fat32_rmdir_pinned(&root, name, &pin);
        if (result == VFS_ERR_NO_MEM)
            assert(!pin && !active_pin && !fs.replace_transaction &&
                   writes == before && fs.version_confident);
        else
            assert(result == VFS_OK && pin &&
                   vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
        cleanup();
    }

    large_empty_directory_fixture();
    assert(fat32_finddir(&root, "LARGE.DIR"));
    assert(fat32_ownership_preflight_locked(&root) == VFS_OK);
    usize baseline = live_requested;
    peak_requested = baseline;
    unsigned before_reads = reads, before_writes = writes;
    clock_t started = clock();
    assert(fat32_rmdir_pinned(&root, "LARGE.DIR", &pin) == VFS_OK && pin &&
           fs.replace_transaction->old_count == 2049U);
    printf("FAT32 >2 MiB mapped rmdir: peak=%zu retained=%zu reads=%u writes=%u cpu_ms=%.3f\n",
           (size_t)(peak_requested - baseline),
           (size_t)(live_requested - baseline), reads - before_reads,
           writes - before_writes,
           (double)(clock() - started) * 1000.0 / CLOCKS_PER_SEC);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_ownership_preflight_locked(&root) == VFS_OK);
    cleanup();
}

static void cross_lfn_creation_cases(void)
{
    replace_source_state_t source = {.short_at = ~(u64)0};
    vfs_file_facts_t facts;
    u64 pin;
    char maximum[256];
    memset(maximum, 'm', 255U);
    maximum[255] = 0;

    setup();
    for (u32 i = 0U; i < 14U; i++) {
        char alias[12];
        snprintf(alias, sizeof(alias), "F%02u     TXT", i);
        entry(i, alias, 0U, 0U);
    }
    media[10][14U * 32U] = media[10][15U * 32U] = 0xe5U;
    ((u32 *)media[1])[2] = 4U;
    ((u32 *)media[1])[4] = FAT32_EOC_MIN;
    media[12][0] = 0xe5U;
    for (u32 i = 1U; i < 16U; i++) {
        char alias[12];
        snprintf(alias, sizeof(alias), "G%02u     TXT", i);
        entry_at(4U, i, alias, 0U, 0U, 0x20U);
    }
    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(*node));
    assert(node); memset(node, 0, sizeof(*node));
    pin = 0U;
    int cross_result = fat32_create_contents_locked(&root, node,
           "Cross sector.txt", 3U, replace_source, &source, &facts, &pin);
    assert(cross_result == VFS_OK && pin &&
           fs.replace_transaction->lfn_creation &&
           fs.replace_transaction->lfn_creation->count == 2U &&
           !fs.replace_transaction->lfn_creation->marker_visibility &&
           !fat32_finddir(&root, "Cross sector.txt"));
    node->super = &super; node->inode = facts.record_id;
    node->size = facts.size; node->type = VFS_TYPE_FILE;
    node->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_finddir(&root, "Cross sector.txt") == node);
    u8 content[3];
    assert(fat32_read(node, 0U, 3U, content) == 3U &&
           content[0] == 0U && content[2] == 2U);
    assert(fat32_unlink_pinned(&root, "Cross sector.txt", &pin) == VFS_OK &&
           pin && vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_ownership_preflight_locked(&root) == VFS_OK);
    cleanup();

    setup();
    for (u32 i = 0U; i < 13U; i++) {
        char alias[12];
        snprintf(alias, sizeof(alias), "F%02u     TXT", i);
        entry(i, alias, 0U, 0U);
    }
    for (u32 i = 13U; i < 16U; i++) media[10][i * 32U] = 0xe5U;
    ((u32 *)media[1])[2] = 4U;
    ((u32 *)media[1])[4] = FAT32_EOC_MIN;
    media[12][0] = 0xe5U;
    for (u32 i = 1U; i < 16U; i++) {
        char alias[12];
        snprintf(alias, sizeof(alias), "G%02u     TXT", i);
        entry_at(4U, i, alias, 0U, 0U, 0x20U);
    }
    const char *unicode = "R\xc3\xa9sum\xc3\xa9 across directory sectors.txt";
    node = (vfs_node_t *)kmalloc(sizeof(*node));
    assert(node); memset(node, 0, sizeof(*node));
    assert(fat32_create_contents_locked(&root, node, unicode, 0U,
           NULL, NULL, &facts, &pin) == VFS_OK && pin &&
           fs.replace_transaction->lfn_creation &&
           fs.replace_transaction->lfn_creation->count == 2U);
    node->super = &super; node->inode = facts.record_id;
    node->size = facts.size; node->type = VFS_TYPE_FILE;
    node->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_finddir(&root, "R\xc3\x89SUM\xc3\x89 ACROSS DIRECTORY SECTORS.TXT") == node);
    unsigned before_collision = writes;
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
           "r\xc3\xa9sum\xc3\xa9 ACROSS DIRECTORY SECTORS.txt", 0U,
           NULL, NULL, &facts, &pin) == VFS_ERR_ALREADY_EXISTS &&
           !pin && writes == before_collision);
    fat32_walk_entry_t unicode_entry;
    assert(fat32_dir_find(&root, unicode, &unicode_entry) == VFS_OK);
    char alias_name[13];
    assert(fat32_format_83_name(&unicode_entry.disk, alias_name) &&
           fat32_create_contents_locked(&root, &(vfs_node_t){0},
               alias_name, 0U, NULL, NULL, &facts, &pin) ==
               VFS_ERR_ALREADY_EXISTS && !pin && writes == before_collision);
    assert(fat32_unlink_pinned(&root, unicode, &pin) == VFS_OK && pin &&
           vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    cleanup();

    setup();
    growth_fill_root(false);
    node = (vfs_node_t *)kmalloc(sizeof(*node));
    assert(node); memset(node, 0, sizeof(*node));
    usize baseline = live_requested;
    peak_requested = baseline;
    unsigned before_reads = reads, before_writes = writes;
    clock_t started = clock();
    pin = 0U;
    assert(fat32_create_contents_locked(&root, node, maximum, 0U,
           NULL, NULL, &facts, &pin) == VFS_OK && pin &&
           fs.replace_transaction->growth->append_count == 2U &&
           fs.replace_transaction->growth->links_confirmed == 2U &&
           fs.replace_transaction->lfn_creation->count == 2U &&
           !fat32_finddir(&root, maximum));
    usize retained = live_requested - baseline;
    printf("FAT32 255-byte cross-sector create: peak=%zu retained=%zu reads=%u writes=%u flushes=%u cpu_ms=%.3f\n",
           (size_t)(peak_requested - baseline), (size_t)retained,
           reads - before_reads, writes - before_writes, flushes,
           (double)(clock() - started) * 1000.0 / CLOCKS_PER_SEC);
    node->super = &super; node->inode = facts.record_id;
    node->size = facts.size; node->type = VFS_TYPE_FILE;
    node->ops = &fat32_node_ops;
    fail_pin_finish = true;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_ERR_BUSY &&
           fs.replace_transaction && fs.namespace_hidden &&
           !fat32_finddir(&root, maximum));
    fail_pin_finish = false;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_finddir(&root, maximum) == node &&
           fat32_ownership_preflight_locked(&root) == VFS_OK);
    assert(fat32_unlink_pinned(&root, maximum, &pin) == VFS_OK && pin &&
           vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_ownership_preflight_locked(&root) == VFS_OK);
    node = (vfs_node_t *)kmalloc(sizeof(*node));
    assert(node); memset(node, 0, sizeof(*node));
    assert(fat32_create_contents_locked(&root, node, maximum, 0U,
           NULL, NULL, &facts, &pin) == VFS_OK && pin &&
           fs.replace_transaction->lfn_creation &&
           !fs.replace_transaction->growth);
    node->super = &super; node->inode = facts.record_id;
    node->size = facts.size; node->type = VFS_TYPE_FILE;
    node->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_finddir(&root, maximum) == node &&
           fat32_ownership_preflight_locked(&root) == VFS_OK);
    cleanup();

    setup();
    for (u32 i = 0U; i < 14U; i++) {
        char alias[12];
        snprintf(alias, sizeof(alias), "H%02u     TXT", i);
        entry(i, alias, 0U, 0U);
    }
    ((u32 *)media[1])[2] = 4U;
    ((u32 *)media[1])[4] = FAT32_EOC_MIN;
    node = (vfs_node_t *)kmalloc(sizeof(*node));
    assert(node); memset(node, 0, sizeof(*node));
    assert(fat32_create_contents_locked(&root, node, maximum, 0U,
           NULL, NULL, &facts, &pin) == VFS_OK && pin &&
           fs.replace_transaction->lfn_creation->count == 3U &&
           fs.replace_transaction->lfn_creation->marker_visibility &&
           fs.replace_transaction->growth->append_count == 1U &&
           fs.replace_transaction->lfn_creation->confirmed_mask == 7U &&
           !fat32_finddir(&root, maximum));
    node->super = &super; node->inode = facts.record_id;
    node->size = facts.size; node->type = VFS_TYPE_FILE;
    node->ops = &fat32_node_ops;
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_finddir(&root, maximum) == node &&
           fat32_ownership_preflight_locked(&root) == VFS_OK);
    assert(fat32_unlink_pinned(&root, maximum, &pin) == VFS_OK && pin &&
           vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
    cleanup();

    setup(); growth_fill_root(false);
    unsigned before_reject = writes;
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0}, maximum,
           (u64)FAT32_REPLACE_MAX_BYTES + 1U, replace_source, &source,
           &facts, &pin) == VFS_ERR_NO_SPACE && !pin &&
           writes == before_reject);
    cleanup();

    setup(); growth_fill_root(false);
    source.short_at = 0U;
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0}, maximum,
           3U, replace_source, &source, &facts, &pin) == VFS_ERR_IO &&
           pin && fs.namespace_hidden && !fat32_finddir(&root, maximum));
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           !fs.namespace_hidden && fs.version_confident &&
           !fs.replace_transaction);
    source.short_at = ~(u64)0;
    cleanup();

    for (u32 failure = 1U; failure <= 8U; failure++) {
        setup(); growth_fill_root(false);
        fail_write_at = failure;
        pin = 0U;
        assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
               maximum, 0U, NULL, NULL, &facts, &pin) ==
               VFS_ERR_UNCERTAIN && !pin && active_pin && pin_unresolved &&
               fs.replace_transaction && !fs.version_confident &&
               fs.namespace_hidden && !fat32_finddir(&root, maximum));
        assert(fs.replace_transaction->growth->append_count == 2U &&
               fs.replace_transaction->lfn_creation->count == 2U);
        unsigned attempts = writes;
        assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
               maximum, 0U, NULL, NULL, &facts, &pin) ==
               VFS_ERR_UNCERTAIN && !pin && writes == attempts);
        cleanup();
    }
    for (u32 failure = 1U; failure <= 7U; failure++) {
        setup(); growth_fill_root(false);
        fail_flush_at = failure;
        pin = 0U;
        assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
               maximum, 0U, NULL, NULL, &facts, &pin) ==
               VFS_ERR_UNCERTAIN && !pin && active_pin && pin_unresolved &&
               fs.namespace_hidden && !fat32_finddir(&root, maximum));
        cleanup();
    }
    for (u32 failure = 4U; failure <= 6U; failure++) {
        setup();
        for (u32 i = 0U; i < 14U; i++) {
            char alias[12];
            snprintf(alias, sizeof(alias), "H%02u     TXT", i);
            entry(i, alias, 0U, 0U);
        }
        ((u32 *)media[1])[2] = 4U;
        ((u32 *)media[1])[4] = FAT32_EOC_MIN;
        fail_write_at = failure;
        fail_write_partial_bytes = 128U;
        pin = 0U;
        assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
               maximum, 0U, NULL, NULL, &facts, &pin) ==
               VFS_ERR_UNCERTAIN && !pin && active_pin && pin_unresolved &&
               fs.namespace_hidden &&
               fs.replace_transaction->lfn_creation->count == 3U &&
               fs.replace_transaction->lfn_creation->attempted_mask ==
                   (failure == 4U ? 2U : failure == 5U ? 6U : 7U) &&
               fs.replace_transaction->lfn_creation->confirmed_mask ==
                   (failure == 4U ? 0U : failure == 5U ? 2U : 6U));
        cleanup();
    }
    setup(); growth_fill_root(false);
    fs.fat_count = 2U;
    memcpy(media[2], media[1], 512U);
    fail_write_at = 2U;
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0}, maximum,
           0U, NULL, NULL, &facts, &pin) == VFS_ERR_UNCERTAIN &&
           !pin && active_pin && pin_unresolved &&
           fs.replace_transaction->growth->fat_copies_completed[0] == 1U &&
           fs.namespace_hidden);
    cleanup();

    setup(); growth_fill_root(false);
    fs.fat_count = 2U;
    memcpy(media[2], media[1], 512U);
    fail_write_at = 7U; /* Second FAT copy of the second cluster allocation. */
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0}, maximum,
           0U, NULL, NULL, &facts, &pin) == VFS_ERR_UNCERTAIN &&
           !pin && active_pin && pin_unresolved &&
           fs.replace_transaction->growth->second_candidate_recorded &&
           fs.replace_transaction->growth->links_confirmed == 1U &&
           fs.replace_transaction->growth->fat_copies_completed[2] == 1U &&
           fs.replace_transaction->lfn_creation->sector[
               fs.replace_transaction->lfn_creation->short_image] ==
               fat32_cluster_to_lba(&fs,
                   fs.replace_transaction->growth->second_candidate));
    cleanup();

    setup(); growth_fill_root(false);
    fs.reserved_sector_count = 8U;
    fs.fs_info_sector = 5U;
    fat32_store_le32(media[5], 0x41615252U);
    fat32_store_le32(media[5] + 484U, 0x61417272U);
    fat32_store_le32(media[5] + 508U, 0xaa550000U);
    fail_write_lba = 5U;
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0}, maximum,
           0U, NULL, NULL, &facts, &pin) == VFS_ERR_UNCERTAIN &&
           !pin && active_pin && pin_unresolved && fs.namespace_hidden &&
           fs.replace_transaction->phase == FAT32_REPLACE_FSINFO &&
           fs.replace_transaction->lfn_creation->confirmed_mask == 3U);
    cleanup();

    for (u32 failure = 1U; failure <= 12U; failure++) {
        setup(); growth_fill_root(false);
        node = (vfs_node_t *)kmalloc(sizeof(*node));
        assert(node); memset(node, 0, sizeof(*node));
        fail_allocation = allocations + failure;
        unsigned before = writes;
        pin = 0U;
        int result = fat32_create_contents_locked(&root, node, maximum,
            0U, NULL, NULL, &facts, &pin);
        if (result == VFS_ERR_NO_MEM) {
            assert(!pin && !active_pin && !fs.replace_transaction &&
                   writes == before && fs.version_confident);
            kfree(node);
        } else {
            assert(result == VFS_OK && pin);
            node->super = &super; node->inode = facts.record_id;
            node->size = facts.size; node->type = VFS_TYPE_FILE;
            node->ops = &fat32_node_ops;
            assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK);
        }
        cleanup();
    }
    setup(); growth_fill_root(false);
    remove_after_writes = 7U;
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0}, maximum,
           0U, NULL, NULL, &facts, &pin) == VFS_ERR_UNCERTAIN &&
           !pin && !online && active_pin && pin_unresolved &&
           fs.replace_transaction->lfn_creation->attempted_mask);
    cleanup();

    retirement_fixture(maximum, true, false);
    u32 orphan_slot = 14U + 20U;
    media[fat32_cluster_to_lba(&fs, 6U) + orphan_slot / 16U - 2U]
         [(orphan_slot % 16U) * 32U] = 0xe5U;
    unsigned before_orphan = writes;
    assert(fat32_create_contents_locked(&root, &(vfs_node_t){0},
           "Another spanning long filename.txt", 0U, NULL, NULL,
           &facts, &pin) == VFS_ERR_BAD_FORMAT && !pin &&
           writes == before_orphan && !fs.version_confident);
    cleanup();
}

static void retirement_image_case(const char *path)
{
    const char *name = "External long filename spanning FAT32 sectors.txt";
    image_file = fopen(path, "r+b");
    assert(image_file && fseeko(image_file, 0, SEEK_END) == 0);
    off_t bytes = ftello(image_file);
    assert(bytes > 0 && bytes % 512 == 0);
    image_sectors = (u64)bytes / 512U;
    image_writable = true;
    device.id = 905U;
    device.sector_size = 512U;
    device.sector_count = image_sectors;
    online = true;
    vfs_super_t *mounted = NULL;
    assert(fat32_mount(&fat32_fs_type, &device, &mounted, false) == VFS_OK);
    pin_super = mounted;
    fat32_walk_entry_t match;
    assert(fat32_dir_find(mounted->root_node, name, &match) == VFS_OK &&
           match.has_lfn && match.lfn.total > match.slot % 16U);
    vfs_node_t *file = fat32_finddir(mounted->root_node, name);
    assert(file && fat32_ownership_preflight_locked(file) == VFS_OK);
    u64 pin = 0U;
    assert(fat32_unlink_pinned(mounted->root_node, name, &pin) == VFS_OK && pin);
    assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_finddir(mounted->root_node, name) == NULL &&
           fat32_ownership_preflight_locked(mounted->root_node) == VFS_OK);
    fat32_fs_t *mounted_fs = (fat32_fs_t *)mounted->private_data;
    vfs_node_t *nodes[32];
    u32 count = 0U;
    for (fat32_node_state_t *state = mounted_fs->nodes; state;
         state = state->next) {
        assert(count < 32U);
        nodes[count++] = state->node;
    }
    assert(fat32_unmount(mounted) == VFS_OK);
    for (u32 i = 0U; i < count; i++) kfree(nodes[i]);
    kfree(mounted);
    fclose(image_file);
    image_file = NULL;
    image_writable = false;
    pin_super = &super;
    puts("FAT32 external-image cross-sector LFN retirement passed");
}

static void cross_creation_image_case(const char *path, bool remove_created)
{
    char name[201];
    memset(name, 'p', 200U);
    name[200] = 0;
    image_file = fopen(path, "r+b");
    assert(image_file && fseeko(image_file, 0, SEEK_END) == 0);
    off_t bytes = ftello(image_file);
    assert(bytes > 0 && bytes % 512 == 0);
    image_sectors = (u64)bytes / 512U;
    image_writable = true;
    device.id = 906U;
    device.sector_size = 512U;
    device.sector_count = image_sectors;
    online = true;
    vfs_super_t *mounted = NULL;
    assert(fat32_mount(&fat32_fs_type, &device, &mounted, false) == VFS_OK);
    pin_super = mounted;
    if (!remove_created) {
        vfs_node_t *created = (vfs_node_t *)kmalloc(sizeof(*created));
        assert(created); memset(created, 0, sizeof(*created));
        replace_source_state_t source = {.short_at = ~(u64)0};
        vfs_file_facts_t facts;
        u64 pin = 0U;
        assert(fat32_create_contents_locked(mounted->root_node, created,
               name, 3U, replace_source, &source, &facts, &pin) == VFS_OK &&
               pin && ((fat32_fs_t *)mounted->private_data)->replace_transaction
                   ->lfn_creation->count > 1U);
        created->super = mounted;
        created->inode = facts.record_id;
        created->size = facts.size;
        created->type = VFS_TYPE_FILE;
        created->ops = &fat32_node_ops;
        assert(vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
               fat32_finddir(mounted->root_node, name) == created &&
               fat32_ownership_preflight_locked(mounted->root_node) == VFS_OK);
    } else {
        fat32_walk_entry_t match;
        assert(fat32_dir_find(mounted->root_node, name, &match) == VFS_OK &&
               match.has_lfn && match.lfn.total > match.slot % 16U);
        u64 pin = 0U;
        assert(fat32_unlink_pinned(mounted->root_node, name, &pin) == VFS_OK &&
               pin && vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
               fat32_ownership_preflight_locked(mounted->root_node) == VFS_OK);
    }
    fat32_fs_t *mounted_fs = (fat32_fs_t *)mounted->private_data;
    vfs_node_t *nodes[32];
    u32 count = 0U;
    for (fat32_node_state_t *state = mounted_fs->nodes; state;
         state = state->next) {
        assert(count < 32U);
        nodes[count++] = state->node;
    }
    assert(fat32_unmount(mounted) == VFS_OK);
    for (u32 i = 0U; i < count; i++) kfree(nodes[i]);
    kfree(mounted);
    fclose(image_file);
    image_file = NULL;
    image_writable = false;
    pin_super = &super;
    puts(remove_created ? "FAT32 external-image cross-sector create/unlink passed" :
                          "FAT32 external-image cross-sector creation passed");
}

static void directory_retirement_image_case(const char *path)
{
    char name[201];
    memset(name, 'q', 200U);
    name[200] = 0;
    image_file = fopen(path, "r+b");
    assert(image_file && fseeko(image_file, 0, SEEK_END) == 0);
    off_t bytes = ftello(image_file);
    assert(bytes > 0 && bytes % 512 == 0);
    image_sectors = (u64)bytes / 512U;
    image_writable = true;
    device.id = 907U;
    device.sector_size = 512U;
    device.sector_count = image_sectors;
    online = true;
    vfs_super_t *mounted = NULL;
    assert(fat32_mount(&fat32_fs_type, &device, &mounted, false) == VFS_OK);
    pin_super = mounted;
    fat32_walk_entry_t match;
    assert(fat32_dir_find(mounted->root_node, name, &match) == VFS_OK &&
           match.has_lfn && match.lfn.total > match.slot % 16U &&
           fat32_finddir(mounted->root_node, name) &&
           fat32_ownership_preflight_locked(mounted->root_node) == VFS_OK);
    u64 pin = 0U;
    assert(fat32_rmdir_pinned(mounted->root_node, name, &pin) == VFS_OK &&
           pin && vfs_recovery_pin_finish_handoff(pin) == VFS_OK &&
           fat32_ownership_preflight_locked(mounted->root_node) == VFS_OK);
    fat32_fs_t *mounted_fs = (fat32_fs_t *)mounted->private_data;
    vfs_node_t *nodes[16];
    u32 count = 0U;
    for (fat32_node_state_t *state = mounted_fs->nodes; state;
         state = state->next) {
        assert(count < 16U);
        nodes[count++] = state->node;
    }
    assert(fat32_unmount(mounted) == VFS_OK);
    for (u32 i = 0U; i < count; i++) kfree(nodes[i]);
    kfree(mounted);
    fclose(image_file);
    image_file = NULL;
    image_writable = false;
    pin_super = &super;
    puts("FAT32 external-image cross-sector LFN directory removal passed");
}

static void checked_enumeration_regression(void)
{
    setup(); entry(0U, "CHECK   TXT", 0U, 0U); vfs_dirent_t out, before; memset(&out, 0x55, sizeof(out)); before = out;
    online = false;
    assert(fat32_readdir_checked(&root, 0U, &out) == VFS_ERR_DEVICE_GONE && !memcmp(&out, &before, sizeof(out)));
    online = true; fs.namespace_hidden = true;
    assert(fat32_readdir_checked(&root, 0U, &out) == VFS_ERR_UNCERTAIN);
    fs.namespace_hidden = false;
    assert(fat32_readdir_checked(&root, 0U, &out) == 1);
    before = out; assert(!fat32_readdir_checked(&root, 1U, &out) && !memcmp(&out, &before, sizeof(out)));
    cleanup(); puts("FAT32 checked enumeration: entry, EOF, removal and uncertainty passed");
}

int main(int argc, char **argv)
{
    if (argc == 1) checked_enumeration_regression();
    if (argc == 3 && !strcmp(argv[1], "--retire-directory-image")) {
        directory_retirement_image_case(argv[2]);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--cross-create-image")) {
        cross_creation_image_case(argv[2], false);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--cross-delete-image")) {
        cross_creation_image_case(argv[2], true);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--retire-image")) {
        retirement_image_case(argv[2]);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--growth-image")) {
        directory_growth_image_case(argv[2]);
        return 0;
    }
    metadata_pair_regressions();
    metadata_view_regressions();
    metadata_directory_workload(false);
    metadata_directory_workload(true);
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
    assert(rename_finished(&root, "a.txt", &root, "c.txt") == VFS_OK);
    assert(fat32_finddir(&root, "c.txt") == a);
    assert(fat32_file_facts(a, &facts) == VFS_OK && facts.generation == 2);
    assert(write_finished(file, 3, 1, "!") == 1);
    assert(file->size == 4 && root_state.generation == 2);
    assert(fat32_file_facts(file, &facts) == VFS_OK && facts.generation == 2);
    u8 snapshot[8] = {0};
    mg_file_lines_state_t snap;
    assert(fat32_snapshot_stream(file, sizeof(snapshot), sink, snapshot,
                                 &snap) == VFS_OK);
    assert(snap.identifier == file->inode && snap.revision == 2 &&
           snap.length == 4 && !memcmp(snapshot, "old!", 4));
    assert(truncate_finished(file) == VFS_OK);
    assert(file->size == 0 && fat32_file_facts(file, &facts) == VFS_OK &&
           facts.generation == 3);
    assert(unlink_finished(&root, "c.txt") == VFS_OK);
    assert(fat32_file_facts(a, &facts) == VFS_ERR_NOT_FOUND);
    entry(0, "NEW     TXT", 0, 0);
    vfs_node_t *replacement = fat32_finddir(&root, "new.txt");
    assert(replacement && replacement->inode != a->inode);
    assert(fat32_read(a, 0, 1, snapshot) == 0);
    fail_write = true;
    assert(write_finished(file, 0, 1, "x") == 0);
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
    fail_allocation = 2; /* admission allocation after the private return node */
    vfs_node_t *unpublished = NULL;
    assert(create_finished(&root, "new.txt", &unpublished) == VFS_ERR_NO_MEM);
    assert(!unpublished && writes == before);
    cleanup();
    setup();
    before = writes;
    fail_read_lba = 10;
    assert(create_finished(&root, "new.txt", &unpublished) == VFS_ERR_IO);
    assert(!unpublished && writes == before);
    cleanup();
    setup();
    before = writes;
    fail_read_lba = 1; /* Allocation FAT read after admission. */
    assert(create_finished(&root, "new.txt", &unpublished) == VFS_ERR_IO);
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
    create_lfn_cases();
    directory_growth_cases();
    directory_growth_large_case();
    directory_growth_large_cluster_case();
    cross_lfn_retirement_cases();
    pinned_directory_retirement_cases();
    cross_lfn_creation_cases();
    if (argc > 1) disposable_image_case(argv[1]);
    if (argc > 2) replacement_image_case(argv[2]);
    puts("FAT32 mount identities, directory chains and namespace ownership passed");
    return 0;
}
