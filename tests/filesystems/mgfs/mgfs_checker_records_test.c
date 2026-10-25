/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production VFS/MGFS with the existing controlled host transport and heap. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../mfqs/host.h"
#include "../../../kernel/src/storage/mgfs.c"

static void enumerate(vfs_node_t *root, unsigned expected)
{
    void *cursor;
    assert(mutex_lock(&root->super->operation_lock));
    assert(root->ops->readdir_open(root, &cursor));
    unsigned count = 0;
    vfs_dirent_t entry;
    int status;
    while ((status = root->ops->readdir_next_checked(cursor, &entry)) == 1) {
        u8 record[MGFS_RECORD_BYTES];
        assert(entry.inode && mgfs_read_record(root->super->private_data,
                                              entry.inode, record));
        assert(entry.name[0]);
        count++;
    }
    assert(status == 0 && count == expected);
    root->ops->readdir_close(cursor);
    assert(mutex_unlock(&root->super->operation_lock));
}

int main(int argc, char **argv)
{
    assert(argc == 3 && mfqs_media_open(argv[1]));
    vfs_init();
    assert(mgfs_init() == VFS_OK);
    assert(vfs_mount_root("mgfs", mfqs_device()) == VFS_OK);
    vfs_node_t *root = vfs_get_root_node(), *node = NULL;
    mgfs_fs_t *fs = root->super->private_data;
    unsigned children = (unsigned)fs->layout.record_count - 1U;
    assert(children < 256U); /* Bounded formatter fixture, not a filesystem cap. */
    if (!strcmp(argv[2], "fill")) {
        for (unsigned i = 0; i < children; i++) {
            char name[32];
            snprintf(name, sizeof(name), "item%04u", i);
            assert(vfs_create(root, name, &node) == VFS_OK);
            if (i + 1U == children - 1U) enumerate(root, children - 1U);
        }
        enumerate(root, children);
        assert(fs->record_id_count == fs->layout.record_count);
    } else if (!strcmp(argv[2], "full")) {
        u64 next = fs->next_record_id;
        /* Ordinary create/mkdir currently classify a full table as IO;
         * conditional create returns NO_SPACE. Neither may publish a node. */
        assert(vfs_create(root, "overflow", &node) == VFS_ERR_IO && !node);
        assert(vfs_mkdir(root, "overflow-dir", &node) == VFS_ERR_IO && !node);
        vfs_file_handle_t *handle = NULL;
        assert(vfs_file_create_exclusive("/", "overflow-safe", 0, NULL,
                                         NULL, &handle) == VFS_ERR_NO_SPACE);
        assert(!handle && fs->next_record_id == next);
        assert(!fs->record_lifecycle_uncertain && !fs->ordinary_transaction);
        u32 pins = 0;
        assert(vfs_recovery_pins_snapshot(NULL, 0, &pins) == VFS_OK && !pins);
        enumerate(root, children);
    } else {
        assert(!strcmp(argv[2], "reuse"));
        u64 old_slot, new_slot;
        assert(vfs_lookup("/item0000", &node) == VFS_OK);
        u64 old_id = node->inode;
        assert(mgfs_find_record_slot(fs, old_id, &old_slot));
        assert(vfs_unlink(root, "item0000") == VFS_OK);
        assert(vfs_create(root, "reuse", &node) == VFS_OK);
        assert(node->inode > fs->layout.record_count);
        assert(mgfs_find_record_slot(fs, node->inode, &new_slot));
        assert(new_slot == old_slot && !mgfs_find_record_slot(fs, old_id, &old_slot));
        assert(vfs_unlink(root, "item0001") == VFS_OK);
        vfs_node_t *directory;
        assert(vfs_mkdir(root, "branch", &directory) == VFS_OK);
        assert(directory->inode > fs->layout.record_count);
        assert(vfs_unlink(root, "item0002") == VFS_OK);
        assert(vfs_create(directory, "leaf", &node) == VFS_OK);
        enumerate(root, children - 1U);
        enumerate(directory, 1);
    }
    printf("MGFS %s: slots=%llu allocated=%llu next_id=%llu\n", argv[2],
           (unsigned long long)fs->layout.record_count,
           (unsigned long long)fs->record_id_count,
           (unsigned long long)fs->next_record_id);
    vfs_node_t *nodes[256];
    unsigned count = 0;
    for (node = root->super->nodes; node; node = node->next_in_super) {
        assert(count < 256U);
        nodes[count++] = node;
    }
    assert(vfs_unmount_device(mfqs_device()) == VFS_OK);
    mfqs_heap_release_nodes(nodes, count);
    mfqs_media_close();
    return 0;
}
