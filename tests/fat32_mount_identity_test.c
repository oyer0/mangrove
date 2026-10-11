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
static unsigned allocations, fail_allocation;
static u64 fail_read_lba;
static fat32_fs_t fs;
static vfs_super_t super;
static vfs_node_t root;
static fat32_node_state_t root_state;

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
    for (fat32_node_state_t *state = fs.nodes; state; state = state->next)
        if (state->node->inode == inode) state->node->size = size;
}
bool block_read_for_id(block_device_t *dev, u64 id, u64 lba, u32 count,
                       void *buffer)
{
    if (!online || dev != &device || id != device.id ||
        lba >= 128 || count != 1 || lba == fail_read_lba) return false;
    memcpy(buffer, media[lba], 512);
    return true;
}
bool block_write_for_id(block_device_t *dev, u64 id, u64 lba, u32 count,
                        const void *buffer)
{
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

static void entry(u32 index, const char *short_name, u32 cluster, u32 size)
{
    fat32_on_disk_entry_t *items = (fat32_on_disk_entry_t *)media[10];
    memset(&items[index], 0, sizeof(items[index]));
    memcpy(items[index].name, short_name, 11);
    items[index].attr = 0x20;
    items[index].first_cluster_low = (u16)cluster;
    items[index].file_size = size;
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
    fail_write = false;
    allocations = fail_allocation = 0;
    fail_read_lba = ~(u64)0;
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
        if (state != &root_state) { free(state->node); free(state); }
        state = next;
    }
}

int main(void)
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
    assert(fat32_create(&root, "new.txt", &unpublished) == VFS_ERR_NO_MEM);
    assert(!unpublished && writes == before);
    cleanup();
    puts("FAT32 mount identities, generations, snapshots, slot reuse, shared-first-cluster rejection and ambiguous writes passed");
    return 0;
}
