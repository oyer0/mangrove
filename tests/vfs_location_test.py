#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Directory leases and ancestry through complete production VFS/MGFS.
Host block transport, allocator, credentials and single-thread scheduler are
controlled dependencies. This is not guest or SMP qualification.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CODE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mfqs/host.h"
#include "storage/mgfs.c"
#include "vfs.c"
#include "initramfs.c"
static void file(vfs_node_t *parent, const char *name, char value)
{
    vfs_node_t *node = NULL;
    vfs_file_handle_t *handle = NULL;
    u64 written = 0;
    assert(vfs_create(parent, name, &node) == VFS_OK);
    assert(vfs_open_node(node, VFS_OPEN_RDWR, &handle) == VFS_OK);
    assert(vfs_file_write_status(handle, 1, &value, &written) == VFS_OK && written == 1);
    assert(vfs_close(handle) == VFS_OK);
}
static vfs_location_t *lookup(vfs_location_t *start, const char *path)
{
    vfs_location_t *out = NULL;
    assert(vfs_location_lookup(start, path, NULL, NULL, &out) == VFS_OK && out);
    return out;
}
int main(int argc, char **argv)
{
    assert((argc == 2 || argc == 3) && mfqs_media_open(argv[1]));
    vfs_init();
    assert(mgfs_init() == VFS_OK);
    assert(vfs_mount_root("mgfs", mfqs_device()) == VFS_OK);
    vfs_node_t *root = vfs_get_root_node(), *left = NULL, *right = NULL, *work = NULL;
    assert(vfs_mkdir(root, "left", &left) == VFS_OK);
    assert(vfs_mkdir(root, "right", &right) == VFS_OK);
    assert(vfs_mkdir(left, "work", &work) == VFS_OK);
    file(left, "sentinel", 'L');
    file(right, "sentinel", 'R');
    file(work, "payload", 'P');
    if (argc == 3) {
        /* Production RAM mount tests the namespace boundary; all persistent
         * mutations still execute production MGFS. Like the guest system RAM
         * mount, this mount lives until the host process exits. */
        assert(initramfs_init() == VFS_OK);
        assert(vfs_mount_path(work,"memory","/left/work/memory","initramfs",
                              NULL,VFS_MOUNT_ROLE_VOLUME,true)==VFS_OK);
        vfs_location_t *mounted=lookup(NULL,"/left/work/memory");
        vfs_location_t *up=lookup(mounted,"..");
        assert(vfs_location_node(up)->inode==work->inode);
        vfs_location_release(up);
        char too_long[256];memset(too_long,'x',255);too_long[255]=0;
        mfqs_metrics_t before,after;mfqs_metrics(&before);
        assert(vfs_rename(root,"left",root,too_long)==VFS_ERR_NAME_TOO_LONG);
        mfqs_metrics(&after);assert(after.writes==before.writes);
        assert(vfs_rename(root,"left",right,"relocated")==VFS_OK);
        char rendered[256];
        assert(vfs_location_path(mounted,rendered,sizeof(rendered),NULL)==VFS_OK);
        assert(!strcmp(rendered,"/right/relocated/work/memory"));
        bool observed=false;
        for(unsigned i=1;i<VFS_MAX_MOUNTS;i++)if(mount_table[i].active) {
            assert(!strcmp(mount_table[i].mount_point,rendered));observed=true;
        }
        assert(observed);
        vfs_location_t *payload=lookup(mounted,"welcome.txt");
        char first=0;assert(vfs_read(vfs_location_node(payload),0,1,&first)==1&&first=='W');
        vfs_location_release(payload);vfs_location_release(mounted);
        mfqs_media_close();
        puts("mounted descendant rename, actual dot-dot, preflight bounds and display publication PASS");
        return 0;
    }
    vfs_location_t *cwd = lookup(NULL, "/left/work");
    assert(vfs_location_cwd_retain(cwd));
    assert(vfs_unmount_device(mfqs_device()) == VFS_ERR_BUSY);
    printf("location=%zu super=%zu pin=%zu ordinary=%zu process=%zu owner-view=%zu move-plan=%zu requested bytes\n",
        sizeof(vfs_location_t),sizeof(vfs_super_t),sizeof(vfs_recovery_pin_t),
        sizeof(mgfs_ordinary_transaction_t),sizeof(process_t),
        sizeof(vfs_recovery_pin_info_t),sizeof(location_move_plan_t));
    unsigned cached=0;
    for(vfs_location_t *p=root->super->locations;p;p=p->next)cached++;
    for(unsigned i=0;i<100;i++) {
        vfs_node_t *temporary=NULL;
        assert(vfs_mkdir(root,"temporary",&temporary)==VFS_OK);
        assert(vfs_rmdir(root,"temporary")==VFS_OK);
    }
    unsigned after=0;
    for(vfs_location_t *p=root->super->locations;p;p=p->next)after++;
    assert(after==cached); /* Deleted directory churn retains no cache entries. */
    assert(vfs_rename(left, "work", right, "moved") == VFS_OK);
    char path[256], data = 0;
    usize required = 0;
    assert(vfs_location_path(cwd, path, sizeof(path), &required) == VFS_OK);
    assert(!strcmp(path, "/right/moved") && required == strlen(path) + 1);
    char unchanged[4]="old";
    assert(vfs_location_path(cwd,unchanged,sizeof(unchanged),&required)==VFS_ERR_BUFFER_TOO_SMALL);
    assert(!strcmp(unchanged,"old") && required==strlen("/right/moved")+1);
    vfs_location_t *entry = lookup(cwd, "../sentinel");
    assert(vfs_read(vfs_location_node(entry), 0, 1, &data) == 1 && data == 'R');
    vfs_location_release(entry);
    entry = lookup(cwd, "payload");
    assert(vfs_read(vfs_location_node(entry), 0, 1, &data) == 1 && data == 'P');
    vfs_location_release(entry);
    assert(vfs_location_lookup(cwd,"missing/../payload",NULL,NULL,&entry)==VFS_ERR_NOT_FOUND);
    assert(vfs_unlink(work, "payload") == VFS_OK);
    assert(vfs_rmdir(right, "moved") == VFS_ERR_BUSY);
    assert(vfs_rename(root, "right", work, "cycle") == VFS_ERR_INVALID_PARAM);
    vfs_location_t *parent = NULL;
    assert(vfs_location_parent(cwd, "../new", NULL, NULL, &parent, path, sizeof(path)) == VFS_OK);
    assert(vfs_location_node(parent)->inode == right->inode && !strcmp(path, "new"));
    vfs_location_release(parent);
    vfs_device_removed(mfqs_device());
    assert(vfs_location_lookup(cwd, "../sentinel", NULL, NULL, &entry) == VFS_ERR_DEVICE_GONE);
    assert(vfs_location_path(cwd, path, sizeof(path), NULL) == VFS_ERR_DEVICE_GONE);
    /* A fresh incarnation of the same filesystem never revives the old CWD. */
    assert(vfs_mount_root("mgfs", mfqs_device()) == VFS_OK);
    assert(vfs_location_lookup(cwd, "payload", NULL, NULL, &entry) == VFS_ERR_DEVICE_GONE);
    entry = lookup(cwd, "/right/sentinel");
    assert(vfs_read(vfs_location_node(entry), 0, 1, &data) == 1 && data == 'R');
    vfs_location_release(entry);
    assert(!vfs_location_cwd_retain(cwd)); /* Explicit binding cannot revive it. */
    assert(vfs_location_cwd_inherit(cwd)); /* Dead binding inheritance. */
    vfs_location_cwd_release(cwd);
    vfs_location_cwd_release(cwd);
    vfs_location_release(cwd);
    assert(vfs_unmount_device(mfqs_device()) == VFS_OK);
    u32 pins = 99;
    assert(vfs_recovery_pins_snapshot(NULL, 0, &pins) == VFS_OK && !pins);
    mfqs_media_close();
    puts("directory identity, moved ancestry, BUSY, removal and fresh incarnation PASS");
    return 0;
}
'''

def main():
    with tempfile.TemporaryDirectory(prefix="mangrove-location-") as work:
        work = Path(work)
        source, binary, image = work / 'test.c', work / 'test', work / 'media.img'
        source.write_text(CODE)
        command = [sys.argv[1] if len(sys.argv) > 1 else 'gcc', '-std=gnu11', '-O1',
                   '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                   '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                   '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'kernel/src'),
                   '-I' + str(ROOT / 'include'), '-idirafter', str(ROOT / 'libc/include'),
                   '-I' + str(ROOT / 'tests'), str(source), str(ROOT / 'tests/mfqs/host.c'), '-o', str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(ROOT / 'build/mkmgfs'), '--blocks', '3200', '--uuid',
                        '11111111-2222-3333-4444-555555555555', '--format-time-ns', '1', str(image)], check=True)
        subprocess.run([str(binary), str(image)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
        subprocess.run([str(ROOT / 'build/mgfsck'), str(image)], check=True)
        subprocess.run([str(ROOT / 'build/mkmgfs'), '--blocks', '3200', '--uuid',
                        '11111111-2222-3333-4444-555555555555', '--format-time-ns', '1', str(image)], check=True)
        subprocess.run([str(binary), str(image), 'mount'], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
        subprocess.run([str(ROOT / 'build/mgfsck'), str(image)], check=True)

if __name__ == '__main__':
    main()
