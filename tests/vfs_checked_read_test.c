/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Complete production drivers/VFS/object read, controlled host transport. */
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
#include "object.c"
static u8 expected[8193], actual[8193];
int main(int argc, char **argv)
{
    assert((argc == 2 || argc == 3) && mfqs_media_open(argv[1]));
    vfs_init();
    assert(DRIVER_INIT() == VFS_OK);
    assert(vfs_mount_root(BACKEND, mfqs_device()) == VFS_OK);
    vfs_node_t *node = NULL;
    vfs_file_handle_t *handle = NULL;
    assert(vfs_create(vfs_get_root_node(), "read.bin", &node) == VFS_OK);
    assert(vfs_open_node(node, VFS_OPEN_RDWR, &handle) == VFS_OK);
    for (usize i = 0; i < sizeof(expected); i++) expected[i] = (u8)(1 + i % 251);
    u64 bytes;
    assert(vfs_file_write_status(handle, sizeof(expected), expected, &bytes) == VFS_OK);
    assert(bytes == sizeof(expected));
    if (argc == 3) {
        assert(!strcmp(argv[2], "uncertain"));
        assert(vfs_seek(handle, 0, VFS_SEEK_SET, NULL) == VFS_OK);
        mfqs_fail_after('w', 1, true, false);
        assert(vfs_file_write_status(handle, 16, expected, &bytes) == VFS_ERR_UNCERTAIN && !bytes);
        assert(vfs_file_read_status(handle, sizeof(actual), actual, &bytes) == VFS_ERR_UNCERTAIN && !bytes);
        vfs_node_t *unavailable=NULL;
        assert(vfs_lookup("/read.bin",&unavailable)==VFS_ERR_UNCERTAIN);
        assert(vfs_lookup("/checked.bin",&unavailable)==VFS_ERR_UNCERTAIN);
        VFS_LOCATION_AUTO vfs_location_t *location=NULL;
        assert(vfs_location_lookup(NULL,"/read.bin",NULL,NULL,&location)==VFS_ERR_UNCERTAIN);
        vfs_dirent_t entry;
        assert(vfs_readdir_checked(vfs_get_root_node(),0,&entry)==VFS_ERR_UNCERTAIN);
        file_object_t file = {0};file.base.type=OBJECT_TYPE_FILE;file.handle=handle;
        assert(file_read(&file.base, actual, sizeof(actual)) == MG_ERR_UNCERTAIN);
        vfs_recovery_pin_info_t owner;u32 pins=0;
        assert(vfs_recovery_pins_snapshot(&owner,1,&pins)==VFS_OK && pins==1 &&
               owner.state==VFS_RECOVERY_PIN_UNRESOLVED);
        assert(vfs_close(handle)==VFS_OK);
        assert(vfs_unmount_device(mfqs_device())==VFS_ERR_BUSY);
        mfqs_device()->online=false;vfs_device_removed(mfqs_device());
        assert(vfs_recovery_pins_snapshot(&owner,1,&pins)==VFS_OK && pins==1);
        mfqs_media_close();
        printf("%s checked reads preserve actual write uncertainty and retained ownership PASS\n",BACKEND);
        return 0; /* Retained pin is intentional; no recovery or clean-media claim. */
    }
    bool failed = false, partial = false, success = false;
    for (u64 boundary = 1; boundary < 100; boundary++) {
        mfqs_fail_after('r', 0, false, false);
        assert(vfs_seek(handle, 0, VFS_SEEK_SET, NULL) == VFS_OK);
        memset(actual, 0xcc, sizeof(actual));
        mfqs_fail_repeated('r', boundary, 2);
        int result = vfs_file_read_status(handle, sizeof(actual), actual, &bytes);
        assert(bytes <= sizeof(actual) && handle->offset == bytes);
        assert(!memcmp(actual, expected, (usize)bytes));
        if (result == VFS_OK) {
            assert(bytes == sizeof(actual));
            success = true;
            continue;
        }
        assert(result == VFS_ERR_IO);
        assert(bytes < sizeof(actual));
        failed = true;
        partial |= bytes != 0;
        mfqs_fail_after('r', 0, false, false);
        u64 tail;
        assert(vfs_file_read_status(handle, sizeof(actual) - bytes,
                                    actual + bytes, &tail) == VFS_OK);
        assert(tail == sizeof(actual) - bytes && !memcmp(actual, expected, sizeof(actual)));
    }
    assert(failed && partial && success);
    mfqs_fail_after('r', 0, false, false);
    assert(vfs_seek(handle, 0, VFS_SEEK_SET, NULL) == VFS_OK);
    file_object_t file = {0};
    file.base.type = OBJECT_TYPE_FILE;
    file.handle = handle;
    mfqs_fail_after('r', 1, false, false);
    assert(file_read(&file.base, actual, sizeof(actual)) == MG_ERR_IO);
    assert(handle->offset == 0); /* Original false-EOF regression. */
    mfqs_fail_after('r', 0, false, false);
    assert(file_read(&file.base, actual, sizeof(actual)) == sizeof(actual));
    assert(!memcmp(actual, expected, sizeof(actual)));
    assert(file_read(&file.base, actual, sizeof(actual)) == 0); /* Actual EOF. */
    assert(vfs_file_read_status(handle, 0, NULL, &bytes) == VFS_OK && bytes == 0);
    assert(vfs_seek(handle, 0, VFS_SEEK_SET, NULL) == VFS_OK);
    mfqs_fail_after('r', 1, false, true);
    assert(file_read(&file.base, actual, sizeof(actual)) == MG_ERR_DEVICE_GONE);
    assert(handle->offset == 0);
    assert(vfs_close(handle) == VFS_OK);
    vfs_device_removed(mfqs_device());
    mfqs_media_close();
    printf("%s checked reads: every read boundary, confirmed prefix, native error/EOF, removal PASS\n", BACKEND);
    return 0;
}
