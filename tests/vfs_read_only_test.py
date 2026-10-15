#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production VFS dispatch and FAT32/exFAT mounted read-only defenses.

The existing driver fixtures provide controlled writable media, heap and pins.
This fixture adds production VFS mutation dispatch; no driver logic is copied.
"""
from host_build import empty_namespace_dependencies
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from host_build import dead_strip_flags

root = Path(__file__).resolve().parents[1]
source = (root / "kernel/src/vfs.c").read_text()


def function(name):
    for match in re.finditer(r"^(?:static )?[^\n;{}]*\b" + name + r"\s*\(", source, re.M):
        brace = source.find("{", match.end())
        if ";" in source[match.end():brace]:
            continue
        depth, end = 1, brace + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end] + "\n"
    raise AssertionError(name)


dependencies = r'''
static spinlock_t vfs_metadata_lock;
static u64 metadata_enter(spinlock_t *lock) { assert(lock == &vfs_metadata_lock); return 0; }
static void metadata_leave(spinlock_t *lock, u64 flags) { assert(lock == &vfs_metadata_lock); (void)flags; }
#define spin_lock_irqsave(lock) metadata_enter(lock)
#define spin_unlock_irqrestore(lock, flags) metadata_leave(lock, flags)
#define VFS_FILE_HANDLE_VALID 0x56465348U
bool mutex_lock(mutex_t *lock) { (void)lock; return true; }
bool mutex_unlock(mutex_t *lock) { (void)lock; return true; }
bool vfs_super_is_live(const vfs_super_t *sb) { return sb == &super && online; }
bool vfs_node_is_live(const vfs_node_t *node) { return node && vfs_super_is_live(node->super); }
bool vfs_super_operation_lock(vfs_super_t *sb) { return vfs_super_is_live(sb); }
void vfs_super_operation_unlock(vfs_super_t *sb) { assert(sb == &super); }
static bool vfs_node_operation_begin(vfs_node_t *node, vfs_super_t **out) {
    if (!vfs_node_is_live(node)) return false;
    *out = node->super; (*out)->reference_count++; return true;
}
static void vfs_node_operation_end(vfs_super_t *sb) { assert(sb->reference_count); sb->reference_count--; }
void vfs_super_release(vfs_super_t *sb) { vfs_node_operation_end(sb); }
static bool vfs_handle_valid(const vfs_file_handle_t *h) { return h && h->valid == VFS_FILE_HANDLE_VALID && vfs_super_is_live(h->super); }
static bool vfs_handle_authorized(const vfs_file_handle_t *h, u32 access) { (void)h; (void)access; return false; }
static vfs_node_t *vfs_finddir_locked(vfs_node_t *node, const char *name) { return node->ops->finddir(node, name); }
bool vfs_directory_child_mutation_allowed(const vfs_node_t *parent, const vfs_node_t *child) { (void)parent; (void)child; return true; }
'''

dependencies += r'''
/* Controlled non-opt-in admission; actual guards are covered by mgfs_handoff_test. */
int vfs_super_operation_error(const vfs_super_t *sb) { return vfs_super_is_live(sb) ? VFS_OK : VFS_ERR_DEVICE_GONE; }
void vfs_super_operation_pause(vfs_super_t *sb) { (void)mutex_unlock(&sb->operation_lock); }
bool vfs_super_operation_guard_release(vfs_super_t *sb) { (void)sb; return true; }
'''

dependencies += empty_namespace_dependencies(source)

names = (
    "vfs_node_on_read_only_mount", "vfs_operation_finish_pin", "vfs_super_operation_complete", "vfs_node_operation_finish", "vfs_write_dispatch",
    "vfs_file_write_status", "vfs_file_write_trusted", "vfs_write",
    "vfs_truncate_dispatch", "vfs_truncate", "vfs_truncate_trusted",
    "vfs_unlink_locked", "vfs_unlink", "vfs_unlink_trusted",
    "vfs_rmdir_locked", "vfs_rmdir", "vfs_rmdir_trusted",
)

tail = r'''
int main(void) {
    setup(); SETUP_FILE;
    vfs_node_t *file = root.ops->finddir(&root, FILE_NAME); assert(file);
    vfs_file_handle_t handle = {.node=file, .super=&super, .flags=VFS_OPEN_RDWR,
        .valid=VFS_FILE_HANDLE_VALID, .save_record_id=file->inode,
        .save_generation=1, .save_expectation_valid=true};
    super.read_only = true; DRIVER_POLICY;
    super.reference_count = 1;
    __typeof__(fs) original = fs;
    vfs_node_t before = *file;
    usize heap = live_requested;
    unsigned allocations_before = allocations;
    u8 *media_before = malloc(sizeof(media)); assert(media_before);
    memcpy(media_before, media, sizeof(media));
    u64 bytes = 123, pin = 123;
    assert(vfs_file_write_status(&handle, 1, "x", &bytes) == VFS_ERR_ACCESS_DENIED && !bytes);
    assert(vfs_file_write_trusted(&handle, 1, "x") == 0);
    assert(vfs_write(file, 0, 1, "x") == 0);
    assert(vfs_truncate(file) == VFS_ERR_ACCESS_DENIED);
    assert(vfs_truncate_trusted(file) == VFS_ERR_ACCESS_DENIED);
    assert(vfs_unlink(&root, FILE_NAME) == VFS_ERR_ACCESS_DENIED);
    assert(vfs_unlink_trusted(&root, FILE_NAME) == VFS_ERR_ACCESS_DENIED);
    assert(vfs_rmdir(&root, "directory") == VFS_ERR_ACCESS_DENIED);
    assert(vfs_rmdir_trusted(&root, "directory") == VFS_ERR_ACCESS_DENIED);
    /* Direct driver callbacks cannot bypass VFS. All return resources stay private. */
    assert(file->ops->write_pinned(file, 0, 1, "x", &bytes, &pin) == VFS_ERR_ACCESS_DENIED && !bytes && !pin);
    assert(file->ops->truncate_pinned(file, &pin) == VFS_ERR_ACCESS_DENIED && !pin);
    vfs_node_t prepared = {0}; vfs_file_facts_t facts;
    assert(root.ops->create_pinned(&root, &prepared, "new.txt", &pin) == VFS_ERR_ACCESS_DENIED && !pin && !prepared.fs_data);
    assert(root.ops->mkdir_pinned(&root, &prepared, "directory", &pin) == VFS_ERR_ACCESS_DENIED && !pin && !prepared.fs_data);
    assert(root.ops->create_contents(&root, &prepared, "new.txt", 0, NULL, NULL, &facts, &pin) == VFS_ERR_ACCESS_DENIED && !pin && !prepared.fs_data);
    assert(root.ops->replace(&super, file->inode, 1, 0, NULL, NULL, &pin) == VFS_ERR_ACCESS_DENIED && !pin);
    assert(root.ops->rename_pinned(&super, &root, FILE_NAME, &root, "new.txt", &pin) == VFS_ERR_ACCESS_DENIED && !pin);
    assert(root.ops->unlink_pinned(&root, FILE_NAME, &pin) == VFS_ERR_ACCESS_DENIED && !pin);
    assert(root.ops->rmdir_pinned(&root, "directory", &pin) == VFS_ERR_ACCESS_DENIED && !pin);
    LOW_LEVEL;
    assert(!writes && !flushes && allocations == allocations_before && live_requested == heap);
    assert(!memcmp(&original, &fs, sizeof(fs)) && !memcmp(&before, file, sizeof(before)));
    assert(!memcmp(media_before, media, sizeof(media)) && !handle.offset && handle.save_generation == 1);
    /* Mount-slot removal cannot erase policy; stale handles still report device loss. */
    online = false;
    assert(super.read_only && vfs_file_write_status(&handle, 1, "x", &bytes) == VFS_ERR_DEVICE_GONE && !bytes);
    assert(vfs_file_write_trusted(&handle, 1, "x") == 0 && !writes && super.reference_count == 1);
    online = true; free(media_before); cleanup();
    puts("DRIVER production VFS/driver read-only mutation matrix: unchanged media/state, no allocations or pins");
    return 0;
}
'''

compiler = sys.argv[1] if len(sys.argv) > 1 else "gcc"
with tempfile.TemporaryDirectory(prefix="mangrove-read-only-") as directory:
    for driver, setup, policy, low_level, filename in (
        ("fat32", 'entry(0, "TEST    TXT", 3, 4); ((u32 *)media[1])[3]=0x0fffffffU; memcpy(media[11], "data", 4)',
         "", 'assert(!fat32_mounted_write(&super, 10, 1, media[10]));', "TEST.TXT"),
        ("exfat", "add_system_entries()", "fs.read_only = true",
         'assert(!exfat_write(&fs, 40, 1, media[40]))', "old.txt"),
    ):
        fixture = Path(directory) / (driver + ".c")
        binary = Path(directory) / driver
        include = '#define main driver_fixture_main\n#include "' + str(root / "tests" / (driver + "_mount_identity_test.c")) + '"\n#undef main\n'
        code = tail.replace("SETUP_FILE", setup).replace("DRIVER_POLICY", policy)
        code = code.replace("LOW_LEVEL", low_level).replace("FILE_NAME", '"' + filename + '"').replace("DRIVER", driver)
        extra = "" if driver == "fat32" else 'int vfs_recovery_pin_finish_handoff(u64 pin) { (void)pin; assert(false); return VFS_ERR_UNCERTAIN; }\n'
        fixture.write_text(include + dependencies + extra + "\n".join(function(n) for n in names) + code)
        subprocess.run([compiler, "-O1", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-Wno-type-limits", "-ffunction-sections",
                        "-fdata-sections", "-fsanitize=address,undefined", *dead_strip_flags(),
                        "-I" + str(root / "kernel/include"), "-I" + str(root / "include"),
                        "-idirafter", str(root / "libc/include"), str(fixture), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
