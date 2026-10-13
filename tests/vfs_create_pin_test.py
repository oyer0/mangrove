#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute production VFS ordinary create/mkdir with controlled pin/lock dependencies."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "kernel/src/vfs.c").read_text()


def function(name):
    for match in re.finditer(r"^(?:static )?[^\n;{}]*\b" + name + r"\s*\(",
                             source, re.M):
        brace = source.find("{", match.end())
        if brace < 0 or ";" in source[match.end():brace]:
            continue
        depth, end = 1, brace + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end] + "\n"
    raise AssertionError(f"missing production function {name}")


prefix = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vfs.h>

static vfs_super_t super;
static vfs_node_t parent;
static vfs_mount_t mounted_child;
static spinlock_t vfs_metadata_lock;
static pthread_mutex_t operation_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t metadata_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t race_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t race_condition = PTHREAD_COND_INITIALIZER;
static bool signal_waiter, waiter_ready;
static bool device_live, read_only, mount_child, name_exists, permission;
static bool pin_live, pin_unresolved, attached, fail_finish, finish_busy;
static bool remove_during_hook, remove_during_finish, fail_lock;
static unsigned private_discards;
static unsigned hook_calls, finish_calls, allocations, frees, fail_allocation;
static int hook_result;

static u64 fixture_spin_lock(spinlock_t *lock) {
    assert(lock == &vfs_metadata_lock);
    assert(!pthread_mutex_lock(&metadata_mutex));
    return 0;
}
static void fixture_spin_unlock(spinlock_t *lock, u64 flags) {
    (void)flags;
    assert(lock == &vfs_metadata_lock);
    assert(!pthread_mutex_unlock(&metadata_mutex));
}
#define spin_lock_irqsave(lock) fixture_spin_lock(lock)
#define spin_unlock_irqrestore(lock, flags) fixture_spin_unlock(lock, flags)

static bool vfs_super_is_live_locked(const vfs_super_t *sb) {
    return sb == &super && device_live && sb->state == VFS_SUPER_ACTIVE;
}
bool vfs_super_is_live(const vfs_super_t *sb) {
    return vfs_super_is_live_locked(sb);
}
static vfs_mount_t *vfs_find_mount_child_locked(vfs_node_t *dir,
                                                const char *name) {
    assert(dir == &parent && name);
    return mount_child ? &mounted_child : NULL;
}
static bool vfs_node_on_read_only_mount(const vfs_node_t *dir) {
    assert(dir == &parent); return read_only;
}
void *kmalloc(usize size) {
    allocations++;
    if (allocations == fail_allocation) return NULL;
    void *p = malloc(size);
    assert(p);
    return p;
}
void kfree(void *p) {
    if (p) { frees++; free(p); }
}
bool vfs_node_retain_super(vfs_node_t *dir, vfs_super_t **out) {
    assert(dir == &parent && out);
    if (!vfs_super_is_live(&super)) return false;
    super.reference_count++; *out = &super; return true;
}
void vfs_super_release(vfs_super_t *sb) {
    assert(sb == &super && sb->reference_count > 1);
    sb->reference_count--;
}
bool vfs_super_operation_lock(vfs_super_t *sb) {
    assert(sb == &super);
    if (fail_lock || !vfs_super_is_live(sb)) return false;
    if (signal_waiter) {
        assert(!pthread_mutex_lock(&race_mutex));
        waiter_ready = true;
        assert(!pthread_cond_signal(&race_condition));
        assert(!pthread_mutex_unlock(&race_mutex));
    }
    assert(!pthread_mutex_lock(&operation_mutex));
    if (!vfs_super_is_live(sb)) {
        assert(!pthread_mutex_unlock(&operation_mutex));
        return false;
    }
    return true;
}
void vfs_super_operation_unlock(vfs_super_t *sb) {
    assert(sb == &super);
    assert(!pthread_mutex_unlock(&operation_mutex));
}
bool mutex_lock(mutex_t *lock) {
    assert(lock == &super.operation_lock);
    return pthread_mutex_lock(&operation_mutex) == 0;
}
bool mutex_unlock(mutex_t *lock) {
    assert(lock == &super.operation_lock);
    return pthread_mutex_unlock(&operation_mutex) == 0;
}

bool vfs_node_is_live(const vfs_node_t *node) { return node == &parent && device_live; }
bool vfs_check_access(const vfs_node_t *node, u32 access) {
    assert(node == &parent && access == VFS_ACCESS_WRITE);
    assert(pthread_mutex_trylock(&operation_mutex) == EBUSY);
    return permission;
}
static int plain_calls;
static vfs_node_t legacy_node;
static int fake_plain(vfs_node_t *dir, const char *name, vfs_node_t **out) {
    assert(dir == &parent && name);
    assert(pthread_mutex_trylock(&operation_mutex) == EBUSY);
    plain_calls++; *out = &legacy_node; return VFS_OK;
}
static int ordinary_create(vfs_node_t *dir, vfs_node_t *node,
                            const char *name, u64 *pin, bool directory) {
    assert(dir == &parent && !strcmp(name, "new") && node && !node->fs_data);
    assert(pthread_mutex_trylock(&operation_mutex) == EBUSY);
    hook_calls++;
    if (name_exists) return VFS_ERR_ALREADY_EXISTS;
    if (remove_during_hook) device_live = false;
    if (hook_result == VFS_ERR_NO_SPACE) return hook_result; /* pin exhaustion */
    pin_live = attached = true; super.reference_count++;
    if (hook_result == VFS_ERR_UNCERTAIN) {
        pin_unresolved = true; return hook_result;
    }
    *pin = 42U;
    if (hook_result != VFS_OK) return hook_result; /* resolved cancellation */
    node->super = &super; node->inode = 89U; node->fs_data = (void *)0x1000;
    node->type = directory ? VFS_TYPE_DIRECTORY : VFS_TYPE_FILE;
    node->ops = parent.ops;
    return VFS_OK;
}
static int create_hook(vfs_node_t *dir, vfs_node_t *node, const char *name, u64 *pin)
{ return ordinary_create(dir, node, name, pin, false); }
static int mkdir_hook(vfs_node_t *dir, vfs_node_t *node, const char *name, u64 *pin)
{ return ordinary_create(dir, node, name, pin, true); }
static void fake_discard(vfs_node_t *node, vfs_super_t *sb) {
    assert(node && sb == &super && node->fs_data == (void *)0x1000);
    assert(pthread_mutex_trylock(&operation_mutex) == EBUSY);
    private_discards++; node->fs_data = NULL;
}
int vfs_recovery_pin_finish_handoff(u64 id) {
    assert(id == 42U && pin_live && !pin_unresolved);
    assert(!pthread_mutex_trylock(&operation_mutex));
    assert(!pthread_mutex_unlock(&operation_mutex));
    assert(super.reference_count >= 3U); /* operation and pin still hold mount */
    finish_calls++;
    if (finish_busy) { finish_busy = false; return VFS_ERR_BUSY; }
    if (remove_during_finish) { device_live = false; super.state = VFS_SUPER_DEAD; }
    if (fail_finish) { pin_unresolved = true; return VFS_ERR_UNCERTAIN; }
    attached = pin_live = false; super.reference_count--; return VFS_OK;
}
int vfs_recovery_pin_mark_unresolved(u64 id) {
    assert(id == 42U && pin_live && attached); pin_unresolved = true; return VFS_OK;
}
static const vfs_ops_t pinned_ops = {
    .create = fake_plain, .mkdir = fake_plain,
    .create_pinned = create_hook, .mkdir_pinned = mkdir_hook,
    .discard_created_node = fake_discard,
};
static const vfs_ops_t plain_ops = { .create = fake_plain, .mkdir = fake_plain };
static void reset(void) {
    memset(&super, 0, sizeof(super)); memset(&parent, 0, sizeof(parent));
    super.reference_count = 1U; super.state = VFS_SUPER_ACTIVE;
    parent.super = &super; parent.type = VFS_TYPE_DIRECTORY; parent.ops = &pinned_ops;
    device_live = permission = true;
    read_only = mount_child = name_exists = false;
    pin_live = pin_unresolved = attached = fail_finish = finish_busy = false;
    remove_during_hook = remove_during_finish = fail_lock = false;
    hook_calls = finish_calls = allocations = frees = fail_allocation = 0U;
    hook_result = VFS_OK; private_discards = 0U; plain_calls = 0;
    signal_waiter = waiter_ready = false;
}
static int invoke(bool directory, vfs_node_t **node)
{ return directory ? vfs_mkdir(&parent, "new", node) : vfs_create(&parent, "new", node); }
'''

tail = r'''
static void *queued_create(void *unused) {
    (void)unused; vfs_node_t *node = (void *)1;
    assert(vfs_create(&parent, "new", &node) == VFS_ERR_ACCESS_DENIED && !node);
    return NULL;
}
int main(void) {
    for (unsigned directory = 0U; directory < 2U; directory++) {
        vfs_node_t *node = NULL;
        reset();
        assert(invoke(directory, &node) == VFS_OK && node && !pin_live && !attached &&
               node->type == (directory ? VFS_TYPE_DIRECTORY : VFS_TYPE_FILE) &&
               super.nodes == node && super.reference_count == 1U &&
               hook_calls == 1U && finish_calls == 1U && !plain_calls);
        assert(mutex_lock(&super.operation_lock));
        vfs_node_unregister_created(node, &super); fake_discard(node, &super);
        assert(mutex_unlock(&super.operation_lock)); kfree(node); assert(frees == 1U);
        reset(); parent.ops = &plain_ops;
        assert(invoke(directory, &node) == VFS_OK && node == &legacy_node &&
               plain_calls == 1 && !hook_calls && !allocations && super.reference_count == 1U);
        for (unsigned kind = 0U; kind < 5U; kind++) {
            reset();
            if (!kind) permission = false;
            if (kind == 1U) read_only = true;
            if (kind == 2U) mount_child = true;
            if (kind == 3U) name_exists = true;
            if (kind == 4U) fail_allocation = 1U;
            int expected = kind <= 1U ? VFS_ERR_ACCESS_DENIED :
                           kind <= 3U ? VFS_ERR_ALREADY_EXISTS : VFS_ERR_NO_MEM;
            assert(invoke(directory, &node) == expected && !node && !pin_live &&
                   !super.nodes && super.reference_count == 1U && !plain_calls);
        }
        reset(); finish_busy = true;
        assert(invoke(directory, &node) == VFS_OK && node && finish_calls == 2U);
        assert(mutex_lock(&super.operation_lock));
        vfs_node_unregister_created(node, &super); fake_discard(node, &super);
        assert(mutex_unlock(&super.operation_lock)); kfree(node);
        reset(); hook_result = VFS_ERR_NO_SPACE;
        assert(invoke(directory, &node) == VFS_ERR_NO_SPACE && !node && !pin_live && frees == 1U);
        reset(); hook_result = VFS_ERR_IO;
        assert(invoke(directory, &node) == VFS_ERR_IO && !node && !pin_live &&
               finish_calls == 1U && frees == 1U && super.reference_count == 1U);
        reset(); hook_result = VFS_ERR_UNCERTAIN;
        assert(invoke(directory, &node) == VFS_ERR_UNCERTAIN && !node && pin_live &&
               pin_unresolved && !finish_calls && frees == 1U && super.reference_count == 2U);
        reset(); fail_finish = true;
        assert(invoke(directory, &node) == VFS_ERR_UNCERTAIN && !node && pin_live &&
               pin_unresolved && private_discards == 1U && !super.nodes &&
               frees == 1U && super.reference_count == 2U);
        for (unsigned when = 0U; when < 2U; when++) {
            reset();
            remove_during_hook = !when; remove_during_finish = when;
            assert(invoke(directory, &node) == VFS_ERR_DEVICE_GONE && !node &&
                   !pin_live && super.reference_count == 1U);
            if (!when) assert(private_discards == 1U && !super.nodes && frees == 1U);
            else {
                /* A lookup may acquire this node immediately after handoff;
                 * late removal cannot discard its state or free the pointer. */
                assert(!private_discards && !frees && super.nodes &&
                       super.nodes->fs_data == (void *)0x1000);
                node = super.nodes;
                assert(mutex_lock(&super.operation_lock));
                vfs_node_unregister_created(node, &super); fake_discard(node, &super);
                assert(mutex_unlock(&super.operation_lock)); kfree(node);
            }
        }
        reset(); device_live = false;
        assert(invoke(directory, &node) == VFS_ERR_INVALID_PARAM && !node && !allocations);
        reset(); fail_lock = true;
        assert(invoke(directory, &node) == VFS_ERR_DEVICE_GONE && !node &&
               !allocations && !hook_calls && super.reference_count == 1U);
    }
    /* Access changes while waiting for operation_lock must affect admission. */
    reset(); signal_waiter = true;
    assert(!pthread_mutex_lock(&operation_mutex));
    pthread_t thread; assert(!pthread_create(&thread, NULL, queued_create, NULL));
    assert(!pthread_mutex_lock(&race_mutex));
    while (!waiter_ready) assert(!pthread_cond_wait(&race_condition, &race_mutex));
    assert(!pthread_mutex_unlock(&race_mutex));
    permission = false; assert(!pthread_mutex_unlock(&operation_mutex));
    assert(!pthread_join(thread, NULL));
    assert(!allocations && !hook_calls && super.reference_count == 1U);
    puts("VFS ordinary create/mkdir: production dispatch, references, authorization and post-unlock handoff passed");
    return 0;
}
'''

functions = "".join(function(name) for name in (
    "vfs_node_register_created", "vfs_node_unregister_created", "vfs_node_operation_begin",
    "vfs_operation_finish_pin", "vfs_create_node", "vfs_create", "vfs_mkdir"))
with tempfile.TemporaryDirectory(prefix="mangrove-vfs-create-pin-") as directory:
    path = Path(directory) / "fixture.c"
    path.write_text(prefix + functions + tail)
    binary = Path(directory) / "fixture"
    compiler = sys.argv[1] if len(sys.argv) > 1 else "gcc"
    subprocess.run([compiler, "-O1", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-pthread",
                    "-fsanitize=address,undefined", "-I" + str(root / "kernel/include"),
                    "-I" + str(root / "include"), "-idirafter", str(root / "libc/include"),
                    str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
