/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production VFS/drivers; only transport, heap and scheduler are controlled. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mfqs/host.h"
#if defined(TEST_FAT32)
#include "storage/fat32.c"
#define BACKEND "fat32"
#define DRIVER_INIT fat32_init
#elif defined(TEST_EXFAT)
#include "storage/exfat.c"
#define BACKEND "exfat"
#define DRIVER_INIT exfat_init
#else
#include "storage/mgfs.c"
#define BACKEND "mgfs"
#define DRIVER_INIT mgfs_init
#endif

static void no_mutation(const mfqs_metrics_t *before)
{
    mfqs_metrics_t after; mfqs_metrics(&after);
    assert(after.writes == before->writes && after.flushes == before->flushes);
    u32 count = 0;
    assert(vfs_recovery_pins_snapshot(NULL, 0, &count) == VFS_OK && !count);
}

int main(int argc, char **argv)
{
    assert(argc == 2 && mfqs_media_open(argv[1]));
    vfs_init(); assert(DRIVER_INIT() == VFS_OK);
    assert(vfs_mount_root(BACKEND, mfqs_device()) == VFS_OK);
    vfs_node_t *root = vfs_get_root_node(), *file = NULL, *busy = NULL;
    assert(vfs_create(root, "existing.bin", &file) == VFS_OK);
    vfs_file_handle_t *handle = NULL; u64 bytes = 0;
    assert(vfs_open_node(file, VFS_OPEN_RDWR, &handle) == VFS_OK);
    assert(vfs_file_write_status(handle, 4, "DATA", &bytes) == VFS_OK && bytes == 4);
    assert(vfs_close(handle) == VFS_OK);
    assert(vfs_mkdir(root, "busy", &busy) == VFS_OK);
    vfs_location_t *cwd = NULL;
    assert(vfs_location_lookup(NULL, "/busy", NULL, NULL, &cwd) == VFS_OK);
    assert(vfs_location_cwd_retain(cwd));
    mfqs_fail_repeated('r', 1, 2);
    vfs_node_t *node = NULL;
    int result = vfs_lookup("/existing.bin", &node);
    mfqs_fail_after('r', 0, false, false);
#ifdef LOOKUP_HISTORICAL
    assert(result == VFS_ERR_NOT_FOUND && !node);
    bool deleted = false;
    for (u64 boundary = 1; boundary <= 96; boundary++) {
        mfqs_metrics_t before, after; mfqs_metrics(&before);
        mfqs_fail_after('r', boundary, false, false);
        result = vfs_rmdir(root, "busy");
        mfqs_fail_after('r', 0, false, false); mfqs_metrics(&after);
        if (result == VFS_OK) {
            char path[256];
            assert(after.writes > before.writes);
            assert(vfs_location_path(cwd, path, sizeof(path), NULL) == VFS_OK);
            assert(!strcmp(path, "/busy"));
            printf("%s historical: false NOT_FOUND and deleted live CWD; boundary=%llu writes=%llu\n",
                   BACKEND, (unsigned long long)boundary,
                   (unsigned long long)(after.writes - before.writes));
            deleted = true; break;
        }
    }
    assert(deleted);
#else
    assert(result == VFS_ERR_IO && !node);
    unsigned io = 0, completed = 0;
    for (unsigned kind = 0; kind < 4; kind++) {
        for (u64 boundary = 1; boundary <= 48; boundary++) {
            mfqs_metrics_t before; mfqs_metrics(&before);
            mfqs_fail_after('r', boundary, false, false);
            if (kind == 0) result = vfs_rmdir(root, "busy");
            if (kind == 1) result = vfs_rmdir_trusted(root, "busy");
            if (kind == 2) result = vfs_rmdir_expected(root, "busy", root->super, busy->inode);
            if (kind == 3) result = vfs_rmdir_expected_authorized(root, "busy", root->super, busy->inode);
            mfqs_fail_after('r', 0, false, false);
            assert(result == VFS_ERR_IO || result == VFS_ERR_BUSY);
            io += result == VFS_ERR_IO; completed += result == VFS_ERR_BUSY;
            no_mutation(&before);
            char path[256];
            assert(vfs_location_path(cwd, path, sizeof(path), NULL) == VFS_OK);
            assert(!strcmp(path, "/busy"));
        }
    }
    assert(io && completed);
    for (unsigned kind = 0; kind < 4; kind++) {
        bool failed = false, found = false;
        for (u64 boundary = 1; boundary <= 48; boundary++) {
            mfqs_metrics_t before; mfqs_metrics(&before);
            mfqs_fail_after('r', boundary, false, false);
            node = NULL; vfs_location_t *location = NULL;
            if (kind == 0) result = vfs_lookup("/existing.bin", &node);
            if (kind == 1) result = vfs_lookup_trusted("/existing.bin", &node);
            if (kind == 2) result = vfs_location_lookup(cwd, "../existing.bin", NULL, NULL, &location);
            if (kind == 3) result = vfs_finddir_checked(root, "existing.bin", &node);
            mfqs_fail_after('r', 0, false, false);
            assert(result == VFS_OK || result == VFS_ERR_IO);
            failed |= result == VFS_ERR_IO; found |= result == VFS_OK;
            if (result != VFS_OK) assert(!node && !location);
            vfs_location_release(location); no_mutation(&before);
        }
        assert(failed && found);
    }
    node = (void *)1;
    assert(vfs_finddir_trusted_checked(root, "absent", &node) == VFS_ERR_NOT_FOUND && !node);
    for (unsigned kind = 0; kind < 8; kind++) {
        mfqs_metrics_t before; mfqs_metrics(&before);
        mfqs_fail_after('r', 1, false, false);
        if (kind == 0) result = vfs_unlink(root, "existing.bin");
        if (kind == 1) result = vfs_unlink_trusted(root, "existing.bin");
        if (kind == 2) result = vfs_unlink_expected(root, "existing.bin", root->super, file->inode);
        if (kind == 3) result = vfs_unlink_expected_authorized(root, "existing.bin", root->super, file->inode);
        if (kind == 4) result = vfs_rename(root, "existing.bin", root, "moved.bin");
        if (kind == 5) result = vfs_rename_trusted(root, "existing.bin", root, "moved.bin");
        if (kind == 6) result = vfs_rename_expected(root, "existing.bin", root->super, file->inode, root, "moved.bin");
        if (kind == 7) result = vfs_rename_expected_authorized(root, "existing.bin", root->super, file->inode, root, "moved.bin");
        mfqs_fail_after('r', 0, false, false);
        assert(result == VFS_ERR_IO); no_mutation(&before);
    }
    vfs_location_cwd_release(cwd); vfs_location_release(cwd); cwd = NULL;
    assert(vfs_rmdir(root, "busy") == VFS_OK);
    assert(vfs_unmount_device(mfqs_device()) == VFS_OK);
    assert(vfs_mount_root(BACKEND, mfqs_device()) == VFS_OK);
    root = vfs_get_root_node(); node = NULL;
    mfqs_fail_after('a', 1, false, false);
    assert(vfs_finddir_checked(root, "existing.bin", &node) == VFS_ERR_NO_MEM && !node);
    mfqs_fail_after('a', 0, false, false);
    assert(vfs_lookup("/existing.bin", &node) == VFS_OK);
    assert(vfs_open_node(node, VFS_OPEN_READ, &handle) == VFS_OK);
    char data[4]; assert(vfs_file_read_status(handle, 4, data, &bytes) == VFS_OK && bytes == 4);
    assert(!memcmp(data, "DATA", 4)); assert(vfs_close(handle) == VFS_OK);
    mfqs_fail_after('r', 1, false, true);
    node = NULL; assert(vfs_lookup("/existing.bin", &node) == VFS_ERR_DEVICE_GONE && !node);
    mfqs_fail_after('r', 0, false, false);
    vfs_device_removed(mfqs_device()); mfqs_media_close();
    printf("%s checked lookup: IO/NOT_FOUND/NO_MEM/DEVICE_GONE, CWD BUSY and pre-mutation refusal PASS\n", BACKEND);
    return 0;
#endif
    vfs_location_cwd_release(cwd); vfs_location_release(cwd);
    assert(vfs_unmount_device(mfqs_device()) == VFS_OK); mfqs_media_close();
    return 0;
}
