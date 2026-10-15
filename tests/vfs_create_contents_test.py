#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute production VFS exclusive-create ownership with controlled MGFS I/O."""
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
#define VFS_FILE_HANDLE_VALID 0x56465348U

static vfs_super_t super;
static vfs_node_t parent;
static vfs_mount_t mounted_child;
static spinlock_t vfs_metadata_lock;
static pthread_mutex_t operation_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t metadata_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t race_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t race_condition = PTHREAD_COND_INITIALIZER;
static bool signal_waiter, waiter_ready;
static bool device_live, mount_child, name_exists, permission;
static bool pin_live, pin_unresolved, attached, fail_finish;
static bool remove_during_hook, remove_during_finish, fail_lock;
static bool attach_private;
static vfs_node_t *late_observer;
static unsigned private_discards;
static unsigned hook_calls, finish_calls, allocations, frees, fail_allocation;
static int hook_result;
static u64 expected_length;

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
int vfs_lookup(const char *path, vfs_node_t **out) {
    assert(path && out);
    if (strcmp(path, "/parent")) return VFS_ERR_NOT_FOUND;
    *out = &parent; return VFS_OK;
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
void mutex_init(mutex_t *lock) { (void)lock; }
bool mutex_lock(mutex_t *lock) {
    assert(lock == &super.operation_lock || lock);
    if (lock != &super.operation_lock) return true;
    return pthread_mutex_lock(&operation_mutex) == 0;
}
bool mutex_unlock(mutex_t *lock) {
    if (lock != &super.operation_lock) return true;
    return pthread_mutex_unlock(&operation_mutex) == 0;
}
static u32 source(void *context, u64 offset, u8 *buffer, u32 requested) {
    (void)context;
    assert(offset + requested <= expected_length);
    memset(buffer, 'c', requested);
    return requested;
}
static int fake_create(vfs_node_t *dir, vfs_node_t *prepared,
                       const char *name, u64 length,
                       vfs_replace_source_t read_source, void *context,
                       vfs_file_facts_t *facts, u64 *pin) {
    assert(dir == &parent && strcmp(name, "new") == 0);
    assert(prepared && prepared->fs_data == NULL);
    assert(pthread_mutex_trylock(&operation_mutex) == EBUSY);
    assert(length == expected_length && (length == 0 || read_source == source));
    (void)context;
    hook_calls++;
    if (!permission) return VFS_ERR_ACCESS_DENIED;
    if (name_exists) return VFS_ERR_ALREADY_EXISTS;
    if (remove_during_hook) device_live = false;
    if (hook_result == VFS_ERR_UNCERTAIN) {
        pin_live = pin_unresolved = attached = true;
        super.reference_count++;
        return hook_result;
    }
    if (hook_result == VFS_ERR_IO && !length) return hook_result;
    pin_live = attached = true;
    super.reference_count++;
    *pin = 42;
    if (hook_result != VFS_OK) return hook_result; /* resolved cancellation */
    *facts = (vfs_file_facts_t){.record_id = 89, .generation = 1,
        .size = length, .owner_uid = 7,
        .permissions = VFS_DEFAULT_FILE_PERMISSIONS};
    if (attach_private) prepared->fs_data = (void *)0x1000;
    return VFS_OK;
}
static void fake_discard(vfs_node_t *node, vfs_super_t *sb) {
    assert(node && sb == &super && node->fs_data == (void *)0x1000);
    private_discards++;
    node->fs_data = NULL;
}
int vfs_recovery_pin_finish_handoff(u64 id) {
    assert(id == 42 && pin_live && !pin_unresolved);
    assert(!pthread_mutex_trylock(&operation_mutex));
    assert(!pthread_mutex_unlock(&operation_mutex));
    finish_calls++;
    if (remove_during_finish) {
        late_observer = super.nodes; /* lookup admitted just after handoff */
        device_live = false;
        super.state = VFS_SUPER_DEAD;
        assert(!pthread_mutex_lock(&metadata_mutex));
        for (vfs_node_t *n = super.nodes; n; ) {
            vfs_node_t *next = n->next_in_super;
            n->super = NULL;
            n->next_in_super = NULL;
            n = next;
        }
        super.nodes = NULL;
        assert(!pthread_mutex_unlock(&metadata_mutex));
    }
    if (fail_finish) { pin_unresolved = true; return VFS_ERR_UNCERTAIN; }
    attached = pin_live = false;
    super.reference_count--;
    return VFS_OK;
}
int vfs_recovery_pin_mark_unresolved(u64 id) {
    assert(id == 42 && pin_live && attached);
    pin_unresolved = true; return VFS_OK;
}
static const vfs_ops_t create_ops = {
    .create_contents = fake_create,
    .create_contents_max_bytes = 512U * 1024U,
    .discard_created_node = fake_discard,
};
static const vfs_ops_t no_create_ops = {0};
static void reset(void) {
    memset(&super, 0, sizeof(super));
    memset(&parent, 0, sizeof(parent));
    super.reference_count = 1; super.state = VFS_SUPER_ACTIVE;
    parent.super = &super; parent.inode = 11;
    parent.type = VFS_TYPE_DIRECTORY; parent.ops = &create_ops;
    device_live = permission = true;
    super.read_only = mount_child = name_exists = false;
    pin_live = pin_unresolved = attached = fail_finish = false;
    remove_during_hook = remove_during_finish = fail_lock = false;
    hook_calls = finish_calls = allocations = frees = fail_allocation = 0;
    hook_result = VFS_OK; expected_length = 0;
    attach_private = false; private_discards = 0; late_observer = NULL;
    signal_waiter = waiter_ready = false;
}
'''

suffix = r'''
static void *queued_create(void *unused) {
    (void)unused;
    vfs_file_handle_t *result = (void *)1;
    assert(vfs_file_create_exclusive("/parent", "new", 0, NULL, NULL,
                                     &result) == VFS_ERR_DEVICE_GONE);
    assert(!result);
    return NULL;
}
int main(void) {
    vfs_file_handle_t *handle = (void *)1;
    reset();
    expected_length = 57;
    assert(vfs_file_create_exclusive("/parent", "new", 57, source, NULL,
                                     &handle) == VFS_OK);
    assert(handle && handle != (void *)1 && handle->node);
    assert(handle->super == &super && handle->node->super == &super);
    assert(handle->node->inode == 89 && handle->node->size == 57);
    assert(handle->node->owner_uid == 7 && handle->offset == 0);
    assert(handle->flags == VFS_OPEN_RDWR && handle->save_expectation_valid);
    assert(handle->save_record_id == 89 && handle->save_generation == 1);
    assert(super.nodes == handle->node && !pin_live && !attached);
    assert(super.reference_count == 2 && finish_calls == 1);
    vfs_node_t *created = handle->node;
    vfs_node_unregister_created(created, &super);
    vfs_super_release(handle->super);
    kfree(handle); kfree(created);
    assert(super.reference_count == 1 && frees == 2);

    reset(); attach_private = true;
    assert(vfs_file_create_exclusive("/parent", "new", 0, NULL, NULL,
                                     &handle) == VFS_OK);
    assert(handle && handle->node->fs_data == (void *)0x1000);
    created = handle->node;
    vfs_node_unregister_created(created, &super);
    vfs_super_release(handle->super);
    kfree(handle); kfree(created);
    assert(private_discards == 0);

    reset(); name_exists = true;
    assert(vfs_file_create_exclusive("/parent", "new", 0, NULL, NULL,
                                     &handle) == VFS_ERR_ALREADY_EXISTS);
    assert(!handle && !pin_live && !super.nodes && allocations == frees);
    reset(); mount_child = true;
    assert(vfs_file_create_exclusive("/parent", "new", 0, NULL, NULL,
                                     &handle) == VFS_ERR_ALREADY_EXISTS);
    assert(!hook_calls && allocations == frees);
    reset(); permission = false;
    assert(vfs_file_create_exclusive("/parent", "new", 0, NULL, NULL,
                                     &handle) == VFS_ERR_ACCESS_DENIED);
    assert(!handle && !pin_live && allocations == frees);
    reset(); super.read_only = true;
    assert(vfs_file_create_exclusive("/parent", "new", 0, NULL, NULL,
                                     &handle) == VFS_ERR_ACCESS_DENIED);
    assert(!hook_calls && allocations == frees);
    reset(); parent.ops = &no_create_ops;
    assert(vfs_file_create_exclusive("/parent", "new", 0, NULL, NULL,
                                     &handle) == VFS_ERR_UNSUPPORTED);
    reset(); parent.type = VFS_TYPE_FILE;
    assert(vfs_file_create_exclusive("/parent", "new", 0, NULL, NULL,
                                     &handle) == VFS_ERR_NOT_DIRECTORY);
    reset();
    assert(vfs_file_create_exclusive("/parent", ".", 0, NULL, NULL,
                                     &handle) == VFS_ERR_INVALID_PARAM);
    assert(vfs_file_create_exclusive("/parent", "new", 1, NULL, NULL,
                                     &handle) == VFS_ERR_INVALID_PARAM);
    assert(vfs_file_create_exclusive("/parent", "new", 512U * 1024U + 1,
                                     source, NULL, &handle) == VFS_ERR_NO_SPACE);
    assert(!allocations && !hook_calls);
    reset(); device_live = false;
    assert(vfs_file_create_exclusive("/parent", "new", 0, NULL, NULL,
                                     &handle) == VFS_ERR_DEVICE_GONE);
    assert(!allocations && !hook_calls);

    for (unsigned failure = 1; failure <= 2; failure++) {
        reset(); fail_allocation = failure;
        assert(vfs_file_create_exclusive("/parent", "new", 0, NULL, NULL,
                                         &handle) == VFS_ERR_NO_MEM);
        assert(!handle && !hook_calls && super.reference_count == 1);
        assert(frees + 1 == allocations);
    }
    reset(); fail_lock = true;
    assert(vfs_file_create_exclusive("/parent", "new", 0, NULL, NULL,
                                     &handle) == VFS_ERR_DEVICE_GONE);
    assert(!hook_calls && allocations == frees && super.reference_count == 1);
    reset(); hook_result = VFS_ERR_IO; expected_length = 1;
    assert(vfs_file_create_exclusive("/parent", "new", 1, source, NULL,
                                     &handle) == VFS_ERR_IO);
    assert(!handle && !pin_live && finish_calls == 1 && allocations == frees);
    reset(); hook_result = VFS_ERR_UNCERTAIN;
    assert(vfs_file_create_exclusive("/parent", "new", 0, NULL, NULL,
                                     &handle) == VFS_ERR_UNCERTAIN);
    assert(!handle && attached && pin_live && pin_unresolved);
    assert(super.reference_count == 2 && allocations == frees && !super.nodes);
    reset(); fail_finish = true; attach_private = true;
    assert(vfs_file_create_exclusive("/parent", "new", 0, NULL, NULL,
                                     &handle) == VFS_ERR_UNCERTAIN);
    assert(!handle && attached && pin_live && pin_unresolved);
    assert(super.reference_count == 2 && !super.nodes && allocations == frees &&
           private_discards == 1);
    reset(); remove_during_hook = true;
    assert(vfs_file_create_exclusive("/parent", "new", 0, NULL, NULL,
                                     &handle) == VFS_ERR_DEVICE_GONE);
    assert(!handle && !pin_live && !super.nodes && allocations == frees);
    reset(); remove_during_finish = true; attach_private = true;
    assert(vfs_file_create_exclusive("/parent", "new", 0, NULL, NULL,
                                     &handle) == VFS_ERR_DEVICE_GONE);
    assert(!handle && !pin_live && !super.nodes && allocations == frees + 1U &&
           private_discards == 0 && late_observer &&
           late_observer->fs_data == (void *)0x1000);
    /* End the fixture's public cached-node lifetime; production leaves it to
     * normal detached-node rules, since other handles may still retain it. */
    fake_discard(late_observer, &super); kfree(late_observer);
    reset();
    assert(!pthread_mutex_lock(&operation_mutex));
    signal_waiter = true;
    pthread_t waiting;
    assert(!pthread_create(&waiting, NULL, queued_create, NULL));
    assert(!pthread_mutex_lock(&race_mutex));
    while (!waiter_ready)
        assert(!pthread_cond_wait(&race_condition, &race_mutex));
    assert(!pthread_mutex_unlock(&race_mutex));
    device_live = false;
    assert(!pthread_mutex_unlock(&operation_mutex));
    assert(!pthread_join(waiting, NULL));
    assert(!hook_calls && super.reference_count == 1 &&
           allocations == frees && !super.nodes);
    puts("VFS exclusive create: production admission and return-object lifetime passed");
    return 0;
}
'''

prefix += r"""
int vfs_super_operation_error(const vfs_super_t *sb) { return vfs_super_is_live(sb) ? VFS_OK : VFS_ERR_DEVICE_GONE; }
void vfs_super_operation_pause(vfs_super_t *sb) { vfs_super_operation_unlock(sb); }
bool vfs_super_operation_guard_release(vfs_super_t *sb) { (void)sb; return true; }
"""
code = prefix + "\n".join(function(name) for name in [
    "vfs_node_on_read_only_mount", "vfs_node_register_created", "vfs_node_unregister_created",
    "vfs_file_create_exclusive",
]) + suffix
with tempfile.TemporaryDirectory(prefix="mangrove-vfs-create-") as directory:
    path = Path(directory)
    fixture = path / "fixture.c"
    fixture.write_text(code)
    binary = path / "create-test"
    compiler = sys.argv[1] if len(sys.argv) > 1 else "gcc"
    subprocess.run([
        compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-pthread",
        "-I" + str(root / "kernel/include"),
        "-I" + str(root / "include"),
        "-idirafter", str(root / "libc/include"),
        str(fixture), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
