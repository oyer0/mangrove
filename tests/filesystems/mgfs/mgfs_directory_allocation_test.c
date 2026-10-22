/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Complete production VFS/MGFS; controlled transport/heap/locks only. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../mfqs/host.h"
#include "../../../kernel/src/storage/mgfs.c"

static u32 pins(void)
{
    u32 count = 0;
    int status = vfs_recovery_pins_snapshot(NULL, 0, &count);
    assert(status == VFS_OK || status == VFS_ERR_BUFFER_TOO_SMALL);
    return count;
}
static void teardown(vfs_super_t *sb)
{
    vfs_node_t *nodes[256]; unsigned count = 0;
    for (vfs_node_t *n = sb->nodes; n; n = n->next_in_super) {
        assert(count < 256); nodes[count++] = n;
    }
    assert(vfs_unmount_device(mfqs_device()) == VFS_OK);
    mfqs_heap_release_nodes(nodes, count);
}
static u32 source(void *context, u64 offset, u8 *out, u32 count)
{
    if (context) return 0;
    assert(offset + count <= 4); memcpy(out, "DATA" + offset, count); return count;
}
static void content(const char *path, const char *expected)
{
    vfs_node_t *n; char bytes[8] = {0};
    assert(vfs_lookup(path, &n) == VFS_OK && n->size == strlen(expected));
    assert(vfs_read(n, 0, sizeof(bytes), bytes) == strlen(expected));
    assert(!memcmp(bytes, expected, strlen(expected)));
}
static void namespace(vfs_node_t *parent, bool added)
{
    unsigned seen = 0; vfs_dirent_t entry;
    for (u32 i = 0;; i++) {
        int status = vfs_readdir_trusted_checked(parent, i, &entry);
        assert(status == 0 || status == 1);
        if (!status) break;
        assert(!strcmp(entry.name, "keep") || (added && !strcmp(entry.name, "new")));
        seen++;
    }
    assert(seen == (added ? 2U : 1U));
}
int main(int argc, char **argv)
{
    assert(argc == 7 && mfqs_media_open(argv[1]));
    const char *op = argv[2]; char fault = argv[3][0];
    u64 ordinal = strtoull(argv[4], NULL, 10);
    bool torn = atoi(argv[5]), remove = atoi(argv[6]);
    vfs_init(); assert(mgfs_init() == VFS_OK);
    assert(vfs_mount_root("mgfs", mfqs_device()) == VFS_OK);
    vfs_node_t *root = vfs_get_root_node(), *parent, *node = NULL;
    vfs_super_t *sb = root->super; mgfs_fs_t *fs = sb->private_data;
    assert(vfs_lookup("/parent", &parent) == VFS_OK);
    content("/parent/keep", "STAY"); namespace(parent, false);
    void *cursor;
    assert(mutex_lock(&sb->operation_lock));
    assert(parent->ops->readdir_open(parent, &cursor));
    assert(mutex_unlock(&sb->operation_lock));
    u8 old_parent[192]; assert(mgfs_read_record(fs, parent->inode, old_parent));
    u64 allocated = 0;
    mgfs_extent_desc_t original[520]; u64 original_count;
    assert(mgfs_collect_file_extents(fs, old_parent, original, 520, &original_count, MGFS_EXTENT_DIRECTORY_METADATA));
    for (u64 i = 0; i < original_count; i++) allocated += original[i].block_count;
    mfqs_metrics_t before, after; mfqs_metrics(&before);
    mfqs_fail_after(fault, ordinal, torn, remove);
    vfs_file_handle_t *handle = NULL;
    clock_t start = clock();
    int result;
    if (!strcmp(op,"create")) result = vfs_create(parent,"new",&node);
    else if (!strcmp(op,"mkdir")) result = vfs_mkdir(parent,"new",&node);
    else if (!strcmp(op,"owned")) result = vfs_create_owned(parent,"new",0,3,&node);
    else if (!strcmp(op,"owned-dir")) result = vfs_mkdir_owned(parent,"new",0,3,&node);
    else if (!strcmp(op,"move")) result = vfs_rename(root,"source",parent,"new");
    else if (!strcmp(op,"exclusive") || !strcmp(op,"cancel"))
        result = vfs_file_create_exclusive("/parent","new",4,source,
                                          !strcmp(op,"cancel") ? (void *)1 : NULL,&handle);
    else abort();
    double ms = 1000.0 * (clock() - start) / CLOCKS_PER_SEC;
    mfqs_metrics(&after); mfqs_fail_after('-',0,false,false);
    if (handle) assert(vfs_close(handle) == VFS_OK);
    u64 recovery_bytes = 0;
    if (fs->ordinary_transaction) recovery_bytes = sizeof(mgfs_ordinary_transaction_t);
    if (fs->stage_transaction) {
        mgfs_create_transaction_t *tx = (mgfs_create_transaction_t *)fs->stage_transaction;
        recovery_bytes = sizeof(*tx) + tx->parent_inventory_bytes + tx->stage.inventory_bytes;
    }
    if (result == VFS_ERR_UNCERTAIN) {
        assert(pins() == 1 && fs->ownership_confidence == MGFS_CONFIDENCE_INVALID);
        assert(vfs_unmount_device(mfqs_device()) == VFS_ERR_BUSY);
        assert(vfs_format_preflight(mfqs_device()) == (remove ? VFS_ERR_DEVICE_GONE : VFS_ERR_BUSY));
        u64 writes = after.writes;
        assert(vfs_create(parent,"blocked",&node) != VFS_OK);
        mfqs_metrics_t gated; mfqs_metrics(&gated); assert(writes == gated.writes);
        parent->ops->readdir_close(cursor);
        vfs_device_removed(mfqs_device()); assert(pins() == 1 && mfqs_heap_contains(fs));
    } else {
        assert(!pins() && fs->ownership_confidence == MGFS_CONFIDENCE_PROVEN);
        assert(mgfs_complete_ownership_proof(fs,false) == MGFS_OWNERSHIP_VALID);
        content("/parent/keep","STAY"); namespace(parent, result == VFS_OK);
        if (result == VFS_OK) {
            assert(vfs_lookup("/parent/new",&node) == VFS_OK);
            if (!strcmp(op,"move")) {
                content("/parent/new","KEEP");
                assert(vfs_lookup("/source",&node) == VFS_ERR_NOT_FOUND);
            } else if (!strcmp(op,"exclusive")) content("/parent/new","DATA");
            u8 current[192]; assert(mgfs_read_record(fs,parent->inode,current));
            mgfs_extent_desc_t mapping[520]; u64 count, capacity = 0;
            assert(mgfs_collect_file_extents(fs,current,mapping,520,&count,MGFS_EXTENT_DIRECTORY_METADATA));
            for (u64 i = 0; i < count; i++) capacity += mapping[i].block_count;
            assert(capacity == allocated + (mgfs_ceil_div(mgfs_get_le64(current+32),4096) > allocated));
            if (strcmp(op,"exclusive")) {
                assert(count == original_count || capacity > allocated);
                for (u64 i = 0; i < original_count && capacity == allocated; i++)
                    assert(!memcmp(&original[i], &mapping[i], sizeof(original[i])));
            }
        } else {
            assert(vfs_lookup("/parent/new",&node) == VFS_ERR_NOT_FOUND);
            content("/source","KEEP");
        }
        /* Existing cursors keep their EOF but refresh COW mappings. */
        assert(mutex_lock(&sb->operation_lock));
        vfs_dirent_t entry; assert(parent->ops->readdir_next_checked(cursor,&entry) == 1);
        assert(!strcmp(entry.name,"keep"));
        assert(parent->ops->readdir_next_checked(cursor,&entry) == 0);
        parent->ops->readdir_close(cursor);
        assert(mutex_unlock(&sb->operation_lock));
        teardown(sb);
        assert(mfqs_media_reopen());
        assert(vfs_mount_root("mgfs",mfqs_device()) == VFS_OK);
        content("/parent/keep","STAY");
        assert(vfs_lookup("/parent",&parent) == VFS_OK); namespace(parent,result==VFS_OK);
        teardown(parent->super);
    }
    printf("RESULT op=%s status=%d reads=%llu writes=%llu flushes=%llu requests=%llu peak=%llu retained=%llu ordinary=%zu safe=%zu cpu_ms=%.3f uncertain=%u recovery=%llu\n",
        op,result,(unsigned long long)(after.reads-before.reads),
        (unsigned long long)(after.writes-before.writes),(unsigned long long)(after.flushes-before.flushes),
        (unsigned long long)(after.heap_requests-before.heap_requests),(unsigned long long)after.heap_peak,
        (unsigned long long)(after.heap_live-before.heap_live),sizeof(mgfs_ordinary_transaction_t),
        sizeof(mgfs_create_transaction_t),ms,result == VFS_ERR_UNCERTAIN,(unsigned long long)recovery_bytes);
    return 0;
}
