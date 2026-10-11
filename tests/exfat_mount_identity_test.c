/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production exFAT functions with an in-memory mounted block device. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../kernel/src/storage/exfat.c"

static u8 media[128][512];
static block_device_t device;
static exfat_fs_t fs;
static vfs_super_t super;
static vfs_node_t root;
static exfat_node_t root_state;
static bool online;
static bool fail_write;
static unsigned writes;
static unsigned allocations, fail_allocation;
static u64 fail_read_lba;
static unsigned reads;
static unsigned replace_after_reads;

void *kmalloc(usize size)
{
    if (++allocations == fail_allocation) return NULL;
    return malloc(size);
}
void kfree(void *ptr) { free(ptr); }
int vfs_register_fs(vfs_fs_type_t *type) { (void)type; return VFS_OK; }
bool vfs_current_uid(u32 *out) { *out = 7; return true; }
void vfs_node_set_security(vfs_node_t *node, u32 owner, u32 permissions)
{ node->owner_uid = owner; node->permissions = permissions; }
void vfs_node_register(vfs_node_t *node) { assert(node->super == &super); }
void vfs_nodes_update_size(vfs_super_t *sb, u64 inode, u64 size)
{
    assert(sb == &super);
    for (exfat_node_t *state = fs.nodes; state; state = state->next)
        if (state->vfs_node->inode == inode) state->vfs_node->size = size;
}
bool block_read_for_id(block_device_t *dev, u64 id, u64 lba, u32 count,
                       void *buffer)
{
    if (!online || dev != &device || id != device.id ||
        lba >= 128 || count != 1 || lba == fail_read_lba) return false;
    reads++;
    memcpy(buffer, media[lba], 512);
    if (reads == replace_after_reads) device.id++;
    return true;
}
bool block_write_for_id(block_device_t *dev, u64 id, u64 lba, u32 count,
                        const void *buffer)
{
    if (!online || dev != &device || id != device.id ||
        lba >= 128 || count != 1) return false;
    writes++;
    if (fail_write) {
        memcpy(media[lba], buffer, 128);
        return false;
    }
    memcpy(media[lba], buffer, 512);
    return true;
}
bool block_flush_for_id(block_device_t *dev, u64 id)
{ return online && dev == &device && id == device.id; }
bool block_device_id_is_live(u64 id)
{ return online && id == device.id; }

static void setup(void)
{
    memset(media, 0, sizeof(media));
    memset(&fs, 0, sizeof(fs));
    memset(&super, 0, sizeof(super));
    memset(&root, 0, sizeof(root));
    memset(&root_state, 0, sizeof(root_state));
    device.id = 93;
    device.sector_count = 128;
    online = true;
    fail_write = false;
    allocations = fail_allocation = 0;
    fail_read_lba = ~(u64)0;
    writes = 0;
    reads = 0;
    replace_after_reads = 0;
    fs.dev = &device;
    fs.device_id = device.id;
    fs.boot.cluster_count = 64;
    fs.boot.heap_offset = 40;
    fs.boot.fat_offset = 24;
    fs.boot.fat_length = 1;
    fs.boot.fat_count = 1;
    fs.boot.volume_length = 128;
    fs.sector_size = 512;
    fs.sectors_per_cluster = 1;
    fs.cluster_bytes = 512;
    fs.root_cluster = 2;
    fs.bitmap_cluster = 3;
    fs.bitmap_length = 512;
    fs.bitmap_no_fat = true;
    fs.upcase_no_fat = true;
    fs.version_confident = true;
    fs.next_inode = 1;
    fs.nodes = &root_state;
    super.dev = &device;
    super.private_data = &fs;
    super.root_node = &root;
    root.inode = 1;
    root.type = VFS_TYPE_DIRECTORY;
    root.size = 512;
    root.super = &super;
    root.fs_data = &root_state;
    root.ops = &exfat_node_ops;
    root_state.vfs_node = &root;
    root_state.first_cluster = 2;
    root_state.data_length = 512;
    root_state.valid_data_length = 512;
    root_state.generation = 1;
    root_state.alive = true;
    root_state.version_valid = true;
    ((u32 *)media[24])[2] = EXFAT_CLUSTER_EOC;
    ((u32 *)media[24])[3] = EXFAT_CLUSTER_EOC;
    media[41][0] = 3; /* root and bitmap clusters allocated */
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count;
    assert(exfat_build_file_set(&fs, "old.txt", 0, 0, 0, 0, false,
                                set, &count));
    memcpy(media[40], set, count * EXFAT_ENTRY_BYTES);
}

static int sink(void *context, u64 offset, const u8 *data, usize length)
{
    memcpy((u8 *)context + offset, data, length);
    return VFS_OK;
}

static void cleanup(void)
{
    for (exfat_node_t *state = fs.nodes; state;) {
        exfat_node_t *next = state->next;
        if (state != &root_state) { free(state->vfs_node); free(state); }
        state = next;
    }
}

static void add_system_entries(void)
{
    u8 *bitmap = media[40] + 96U;
    u8 *upcase = media[40] + 128U;
    bitmap[0] = EXFAT_ENTRY_BITMAP;
    exfat_put32(bitmap + 20U, 3U);
    exfat_put64(bitmap + 24U, 512U);
    upcase[0] = EXFAT_ENTRY_UPCASE;
    exfat_put32(upcase + 20U, 4U);
    exfat_put64(upcase + 24U, 512U);
    fs.upcase_cluster = 4U;
    fs.upcase_length = 512U;
    media[41][0] |= 4U;
}

static void add_entry(u64 offset, const char *name, u16 attributes,
                      u32 first, u64 length, bool contiguous)
{
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count;
    assert(exfat_build_file_set(&fs, name, attributes, first, length,
                                length, contiguous, set, &count));
    assert(offset + (u64)count * EXFAT_ENTRY_BYTES <= 512U);
    memcpy(media[40] + offset, set, count * EXFAT_ENTRY_BYTES);
}

static void ownership_regressions(void)
{
    unsigned initial;
    setup();
    add_system_entries();
    initial = writes;
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    printf("exFAT minimal ownership: %u requested bytes, %u reads\n",
           (unsigned)(((fs.boot.cluster_count + 7U) / 8U) +
           sizeof(exfat_scan_directory_t) * 16U), reads);
    vfs_node_t *owned_file = exfat_finddir(&root, "old.txt");
    assert(owned_file && exfat_ownership_preflight_locked(owned_file) == VFS_OK);
    ((exfat_node_t *)owned_file->fs_data)->entry_offset = 256U;
    assert(exfat_ownership_preflight_locked(owned_file) == VFS_ERR_NOT_FOUND);
    ((exfat_node_t *)owned_file->fs_data)->entry_offset = 0U;
    assert(writes == initial);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "one", 0, 5, 600, false);
    ((u32 *)media[24])[5] = 9U;
    ((u32 *)media[24])[9] = EXFAT_CLUSTER_EOC;
    media[41][0] |= 0x88U;
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    add_entry(256, "two", 0, 9, 1, true);
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "short", 0, 5, 1024, false);
    media[41][0] |= 0x08U;
    ((u32 *)media[24])[5] = EXFAT_CLUSTER_EOC;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "invalid", 0, 5, 1024, false);
    media[41][0] |= 0x08U;
    ((u32 *)media[24])[5] = 100U;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "folder", EXFAT_ATTR_DIRECTORY, 5, 512, true);
    media[41][0] |= 0x08U;
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    add_entry(256, "alias", EXFAT_ATTR_DIRECTORY, 5, 512, true);
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "folder", EXFAT_ATTR_DIRECTORY, 5, 512, true);
    add_entry(256, "file", 0, 5, 1, true);
    media[41][0] |= 0x08U;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "folder", EXFAT_ATTR_DIRECTORY, 5, 512, true);
    media[41][0] |= 0x18U;
    u8 nested[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 nested_count;
    assert(exfat_build_file_set(&fs, "inside", 0, 6, 1, 1, true,
                                nested, &nested_count));
    memcpy(media[43], nested, nested_count * EXFAT_ENTRY_BYTES);
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "linked", EXFAT_ATTR_DIRECTORY, 5, 1024, false);
    ((u32 *)media[24])[5] = 6U;
    ((u32 *)media[24])[6] = EXFAT_CLUSTER_EOC;
    media[41][0] |= 0x38U;
    u8 linked_child[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 linked_count;
    assert(exfat_build_file_set(&fs, "child", 0, 7, 1, 1, true,
                                linked_child, &linked_count));
    memcpy(media[43], linked_child, linked_count * EXFAT_ENTRY_BYTES);
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    cleanup();

    setup();
    add_system_entries();
    ((u32 *)media[24])[2] = 7U;
    ((u32 *)media[24])[7] = EXFAT_CLUSTER_EOC;
    media[41][0] |= 0x28U; /* root tail and nested directory */
    for (u32 offset = 160U; offset < 448U; offset += EXFAT_ENTRY_BYTES)
        media[40][offset] = EXFAT_ENTRY_DELETED;
    u8 crossing[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 crossing_count;
    assert(exfat_build_file_set(&fs, "cross", EXFAT_ATTR_DIRECTORY,
                                5, 512, 512, true, crossing, &crossing_count));
    assert(crossing_count == 3U);
    memcpy(media[40] + 448U, crossing, 64U);
    memcpy(media[45], crossing + 64U, 32U);
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    printf("exFAT linked-root crossing entry: %u reads, no writes\n", reads);
    assert(exfat_finddir(&root, "cross") != NULL);
    cleanup();

    setup();
    add_system_entries();
    ((u32 *)media[24])[2] = 7U;
    ((u32 *)media[24])[7] = EXFAT_CLUSTER_EOC;
    media[41][0] |= 0x20U;
    assert(exfat_ownership_preflight_locked(&root) == VFS_OK);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "meta", 0, 4, 1, true);
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    exfat_put32(media[40] + 128U + 20U, 3U);
    fs.upcase_cluster = 3U;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "range", 0, 64U, 1024, true);
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "free", 0, 5, 1, true);
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    add_entry(160, "loop", 0, 5, 1024, false);
    media[41][0] |= 0x18U;
    ((u32 *)media[24])[5] = 6U;
    ((u32 *)media[24])[6] = 5U;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    media[40][2] ^= 1U; /* Primary entry-set checksum. */
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_BAD_FORMAT);
    assert(!fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    fail_allocation = 1;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_NO_MEM);
    assert(fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    fail_allocation = 2; /* Directory queue after ownership bitmap. */
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_NO_MEM);
    assert(fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    fs.boot.cluster_count = EXFAT_SCAN_MAX_BITMAP_BYTES * 8U + 1U;
    assert(exfat_ownership_preflight_locked(&root) ==
           VFS_ERR_BUFFER_TOO_SMALL);
    assert(fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    fail_read_lba = 40;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_IO);
    assert(fs.version_confident && writes == initial);
    fail_read_lba = ~(u64)0;
    device.id++;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_DEVICE_GONE);
    assert(fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    fail_read_lba = 41; /* Allocation bitmap, before namespace traversal. */
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_IO);
    assert(fs.version_confident && writes == initial);
    cleanup();

    setup();
    add_system_entries();
    replace_after_reads = 4;
    assert(exfat_ownership_preflight_locked(&root) == VFS_ERR_IO);
    assert(fs.version_confident && writes == initial);
    cleanup();
}

int main(void)
{
    setup();
    vfs_node_t *file = exfat_finddir(&root, "old.txt");
    assert(file && file->inode != root.inode);
    assert(exfat_finddir(&root, "old.txt") == file);
    vfs_file_facts_t facts;
    assert(exfat_file_facts(file, &facts) == VFS_OK &&
           facts.generation == 1 && facts.size == 0);
    assert(exfat_node_write(file, 0, 5, "hello") == 5);
    assert(file->size == 5 && exfat_file_facts(file, &facts) == VFS_OK &&
           facts.generation == 2);
    u8 data[8] = {0};
    mg_file_lines_state_t snap;
    assert(exfat_snapshot_stream(file, sizeof(data), sink, data, &snap) == VFS_OK);
    assert(snap.identifier == file->inode && snap.revision == 2 &&
           snap.length == 5 && !memcmp(data, "hello", 5));
    assert(exfat_rename(&root, "old.txt", &root, "new.txt") == VFS_OK);
    assert(exfat_finddir(&root, "new.txt") == file);
    assert(exfat_file_facts(file, &facts) == VFS_OK && facts.generation == 3);
    assert(exfat_truncate(file) == VFS_OK && file->size == 0);
    assert(exfat_file_facts(file, &facts) == VFS_OK && facts.generation == 4);
    assert(exfat_unlink(&root, "new.txt") == VFS_OK);
    assert(exfat_file_facts(file, &facts) == VFS_ERR_NOT_FOUND);
    vfs_node_t *new_file = NULL;
    assert(exfat_create(&root, "again.txt", &new_file) == VFS_OK);
    assert(new_file && new_file->inode != file->inode);
    assert(exfat_finddir(&root, "again.txt") == new_file);
    vfs_node_t *second_empty = NULL;
    assert(exfat_create(&root, "second.txt", &second_empty) == VFS_OK);
    assert(second_empty && second_empty->inode != new_file->inode &&
           exfat_finddir(&root, "second.txt") == second_empty);
    assert(exfat_node_read(file, 0, 1, data) == 0);
    fail_write = true;
    assert(exfat_node_write(new_file, 0, 1, "x") == 0);
    assert(!fs.version_confident);
    assert(exfat_file_facts(new_file, &facts) == VFS_ERR_UNCERTAIN);
    unsigned attempted = writes;
    device.id++;
    assert(!exfat_read(&fs, 40, 1, data));
    assert(writes == attempted);
    cleanup();
    setup();
    exfat_entry_info_t first = {0}, second = {0};
    first.parent_cluster = second.parent_cluster = 2;
    first.entry_count = second.entry_count = 3;
    first.first_cluster = second.first_cluster = 4;
    first.data_length = second.data_length = 1;
    second.entry_offset = 96;
    strcpy(first.name, "one");
    strcpy(second.name, "two");
    assert(exfat_make_node(&root, &first));
    assert(!exfat_make_node(&root, &second));
    assert(!fs.version_confident);
    cleanup();
    setup();
    u8 set[(EXFAT_MAX_SECONDARY + 1U) * EXFAT_ENTRY_BYTES];
    u8 count;
    assert(exfat_build_file_set(&fs, "bad.txt", 0, 4, 1, 1, true,
                                set, &count));
    memcpy(media[40], set, count * EXFAT_ENTRY_BYTES);
    vfs_node_t *bad = exfat_finddir(&root, "bad.txt");
    assert(bad && exfat_file_facts(bad, &facts) == VFS_ERR_BAD_FORMAT &&
           !fs.version_confident); /* cluster 4 is marked free */
    cleanup();
    setup();
    assert(exfat_build_file_set(&fs, "loop.txt", 0, 4, 1024, 1024, false,
                                set, &count));
    memcpy(media[40], set, count * EXFAT_ENTRY_BYTES);
    media[41][0] |= 0x0c; /* clusters 4 and 5 allocated */
    ((u32 *)media[24])[4] = 5;
    ((u32 *)media[24])[5] = 4;
    vfs_node_t *loop = exfat_finddir(&root, "loop.txt");
    assert(loop && exfat_file_facts(loop, &facts) == VFS_ERR_BAD_FORMAT);
    cleanup();
    setup();
    vfs_node_t *retry = exfat_finddir(&root, "old.txt");
    fail_read_lba = 40;
    assert(exfat_file_facts(retry, &facts) == VFS_ERR_IO &&
           fs.version_confident);
    fail_read_lba = ~(u64)0;
    assert(exfat_file_facts(retry, &facts) == VFS_OK);
    cleanup();
    setup();
    unsigned before = writes;
    fail_allocation = 1; /* identity reservation before volume dirtying */
    vfs_node_t *unpublished = NULL;
    assert(exfat_create(&root, "new.txt", &unpublished) == VFS_ERR_NO_MEM);
    assert(!unpublished && writes == before);
    cleanup();
    setup();
    before = writes;
    fail_read_lba = 40;
    assert(exfat_create(&root, "new.txt", &unpublished) == VFS_ERR_IO);
    assert(!unpublished && writes == before);
    cleanup();
    setup();
    before = writes;
    fail_write = true; /* Torn dirty-bit write before namespace mutation. */
    assert(exfat_create(&root, "new.txt", &unpublished) ==
           VFS_ERR_UNCERTAIN);
    assert(!unpublished && writes == before + 1U &&
           !fs.version_confident);
    cleanup();
    ownership_regressions();
    puts("exFAT mount identities, generations, snapshots and ownership passed");
    return 0;
}
