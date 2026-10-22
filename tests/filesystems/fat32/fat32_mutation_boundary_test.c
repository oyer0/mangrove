/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production callbacks and read-only inspection with controlled dependencies. */
#define main identity_fixture_main
#include "fat32_mount_identity_test.c"
#undef main

static void shared_allocation(void)
{
    for (u32 operation = 0U; operation < 9U; operation++) {
        setup();
        entry(0U, "ONE     TXT", 3U, 1U);
        entry(1U, "TWO     TXT", 3U, 1U);
        vfs_node_t *file = fat32_finddir(&root, "ONE.TXT");
        assert(file && fs.version_confident && !fs.ownership_proven);
        u8 *before = malloc(sizeof(media)); assert(before);
        memcpy(before, media, sizeof(media));
        vfs_node_t prepared = {0};
        vfs_file_facts_t facts;
        u64 pin = 99U, bytes = 99U;
        int result = VFS_ERR_UNSUPPORTED;
        switch (operation) {
        case 0: result = root.ops->unlink_pinned(&root, "ONE.TXT", &pin); break;
        case 1: result = file->ops->write_pinned(file, 0U, 1U, "x", &bytes, &pin);
            assert(!bytes); break;
        case 2: result = file->ops->truncate_pinned(file, &pin); break;
        case 3: result = file->ops->replace(&super, file->inode, 1U, 0U, NULL, NULL, &pin); break;
        case 4: result = root.ops->create_pinned(&root, &prepared, "NEW.TXT", &pin); break;
        case 5: result = root.ops->mkdir_pinned(&root, &prepared, "NEW", &pin); break;
        case 6: result = root.ops->rename_pinned(&super, &root, "ONE.TXT", &root, "NEW.TXT", &pin); break;
        case 7: result = file->ops->resize_pinned(file, 0U, &pin); break;
        case 8: result = root.ops->create_contents(&root, &prepared, "NEW.TXT", 0U,
            NULL, NULL, &facts, &pin); break;
        }
        assert(result == VFS_ERR_BAD_FORMAT && !pin && !active_pin &&
               !pin_begins && !writes && !flushes && !fs.replace_transaction &&
               !fs.version_confident && !prepared.fs_data &&
               !memcmp(before, media, sizeof(media)));
        free(before);
        cleanup(); assert(!live_requested);
    }
    puts("FAT32 shared allocation: all nine production mutation hooks reject before writes/pins");
}

static void read_only_inspection(void)
{
    setup();
    super.read_only = true;
    usize baseline = live_requested;
    assert(fat32_get_cluster_link(&super, 3U) == FAT32_EOC_MIN);
    assert(fat32_get_cluster_link(&super, 4U) == 0U);
    unsigned calls = reads, allocations_before = allocations;
    assert(fat32_get_cluster_link(NULL, 3U) == FAT32_CLUSTER_ERR);
    assert(fat32_get_cluster_link(&super, 1U) == FAT32_CLUSTER_ERR);
    assert(fat32_get_cluster_link(&super, fs.total_clusters) == FAT32_CLUSTER_ERR);
    assert(reads == calls && allocations == allocations_before);
    fail_allocation = allocations + 1U;
    assert(fat32_get_cluster_link(&super, 3U) == FAT32_CLUSTER_ERR);
    fail_allocation = 0U; fail_read_lba = 1U;
    assert(fat32_get_cluster_link(&super, 3U) == FAT32_CLUSTER_ERR);
    fail_read_lba = 0U; online = false;
    assert(fat32_get_cluster_link(&super, 3U) == FAT32_CLUSTER_ERR);
    online = true; device.id++;
    assert(fat32_get_cluster_link(&super, 3U) == FAT32_CLUSTER_ERR);
    assert(!writes && !flushes && !active_pin && !pin_begins &&
           fs.version_confident && live_requested == baseline);
    printf("FAT32 read-only inspector: one 512-byte scratch allocation/read per valid link; no writes/pins\n");
    cleanup(); assert(!live_requested);
}

int main(void)
{
    shared_allocation();
    read_only_inspection();
    return 0;
}
