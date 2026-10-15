#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production VFS unlink/pin handoff with controlled lifecycle dependencies."""
from host_build import empty_namespace_dependencies
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
#include <string.h>
#include <vfs.h>

static vfs_super_t super;
static vfs_node_t parent, destination, child;
static pthread_mutex_t operation = PTHREAD_MUTEX_INITIALIZER;
static bool live, writable, child_allowed, pinned, attached, unresolved;
static bool hook_uncertain, finish_uncertain, finish_busy_once, detach_on_finish;
static unsigned hook_calls, plain_calls, finish_calls, end_calls, marks;
static vfs_node_t alias, other;
static unsigned destination_identity;
static bool destination_access;

bool vfs_node_is_live(const vfs_node_t *node)
{ return (node == &parent || node == &destination) && live; }
static spinlock_t vfs_metadata_lock;
static u64 metadata_enter(spinlock_t *lock) { assert(lock == &vfs_metadata_lock); return 0; }
static void metadata_leave(spinlock_t *lock, u64 flags) { assert(lock == &vfs_metadata_lock); (void)flags; }
#define spin_lock_irqsave(lock) metadata_enter(lock)
#define spin_unlock_irqrestore(lock, flags) metadata_leave(lock, flags)

bool vfs_check_access(const vfs_node_t *node, u32 permission)
{ assert((node == &parent || node == &destination) && permission == VFS_ACCESS_WRITE);
  return writable && (node != &destination || destination_access); }
static bool vfs_node_operation_begin(vfs_node_t *node, vfs_super_t **out)
{
    assert(node == &parent && out && live);
    assert(!pthread_mutex_lock(&operation));
    super.reference_count++;
    *out = &super;
    return true;
}
bool vfs_super_is_live(const vfs_super_t *sb){return sb == &super && live;}
void vfs_super_release(vfs_super_t *sb){assert(sb == &super && sb->reference_count > 1U);sb->reference_count--;}
static vfs_node_t *vfs_finddir_locked(vfs_node_t *dir, const char *name)
{
    if (!strcmp(name, "OTHER.TXT")) {
        assert(dir == &destination);
        return destination_identity == 1U ? &alias :
               destination_identity == 2U ? &other : NULL;
    }
    assert(dir == &parent && !strcmp(name, "NAME.TXT")); return &child;
}
bool vfs_directory_child_mutation_allowed(const vfs_node_t *dir,
                                          const vfs_node_t *node)
{ assert(dir == &parent && node == &child); return child_allowed; }
bool mutex_lock(mutex_t *lock)
{ assert(lock == &super.operation_lock); return pthread_mutex_lock(&operation) == 0; }
bool mutex_unlock(mutex_t *lock)
{ assert(lock == &super.operation_lock); return pthread_mutex_unlock(&operation) == 0; }
static int fake_plain(vfs_node_t *dir, const char *name)
{
    assert(dir == &parent && !strcmp(name, "NAME.TXT"));
    assert(pthread_mutex_trylock(&operation) == EBUSY);
    plain_calls++;
    return VFS_OK;
}
static int fake_pinned(vfs_node_t *dir, const char *name, u64 *out_pin)
{
    assert(dir == &parent && !strcmp(name, "NAME.TXT") && out_pin);
    assert(pthread_mutex_trylock(&operation) == EBUSY);
    assert(!pinned);
    hook_calls++;
    pinned = attached = true;
    super.reference_count++;
    if (hook_uncertain) {
        unresolved = true;
        return VFS_ERR_UNCERTAIN;
    }
    *out_pin = 47U;
    return VFS_OK;
}
int vfs_recovery_pin_finish_handoff(u64 id)
{
    assert(id == 47U && pinned && attached && !unresolved);
    assert(!pthread_mutex_trylock(&operation));
    assert(!pthread_mutex_unlock(&operation));
    finish_calls++;
    if (finish_busy_once) {
        finish_busy_once = false;
        return VFS_ERR_BUSY;
    }
    if (detach_on_finish) {
        live = false;
        super.state = VFS_SUPER_DEAD;
    }
    if (finish_uncertain) {
        unresolved = true;
        return VFS_ERR_UNCERTAIN;
    }
    pinned = attached = false;
    assert(super.reference_count > 1U);
    super.reference_count--;
    return VFS_OK;
}
int vfs_recovery_pin_mark_unresolved(u64 id)
{ assert(id == 47U && pinned); marks++; unresolved = true; return VFS_OK; }
static int fake_rename(vfs_node_t *src, const char *name,
                       vfs_node_t *dst, const char *new_name)
{
    assert(dst == &destination && !strcmp(new_name, "OTHER.TXT"));
    return fake_plain(src, name);
}
static int fake_rename_pinned(vfs_super_t *sb, vfs_node_t *src, const char *name,
    vfs_node_t *dst, const char *new_name, u64 *pin)
{
    assert(sb == &super && dst == &destination && !strcmp(new_name, "OTHER.TXT"));
    return fake_pinned(src, name, pin);
}
static const vfs_ops_t pinned_ops = {
    .unlink = fake_plain, .unlink_pinned = fake_pinned,
    .rmdir = fake_plain, .rmdir_pinned = fake_pinned,
    .rename_pinned = fake_rename_pinned,
};
static const vfs_ops_t plain_ops = {
    .unlink = fake_plain, .rmdir = fake_plain, .rename = fake_rename};
static void reset(void)
{
    memset(&super, 0, sizeof(super));
    memset(&parent, 0, sizeof(parent));
    memset(&destination, 0, sizeof(destination));
    memset(&child, 0, sizeof(child));
    super.reference_count = 1U;
    super.state = VFS_SUPER_ACTIVE;
    parent.super = destination.super = child.super = &super;
    parent.type = destination.type = VFS_TYPE_DIRECTORY;
    child.type = VFS_TYPE_FILE;
    child.inode = 9U;
    alias = child;
    other = child; other.inode = 10U;
    destination_identity = 0U;
    destination_access = true;
    parent.ops = destination.ops = &pinned_ops;
    live = writable = child_allowed = true;
    pinned = attached = unresolved = false;
    hook_uncertain = finish_uncertain = finish_busy_once = detach_on_finish = false;
    hook_calls = plain_calls = finish_calls = end_calls = marks = 0U;
}
'''

tail = r'''
int main(void)
{
    reset();
    assert(vfs_unlink(&parent, "NAME.TXT") == VFS_OK);
    assert(hook_calls == 1U && !plain_calls && finish_calls == 1U &&
           end_calls == 1U && !pinned && super.reference_count == 1U);
    reset();
    finish_busy_once = true;
    assert(vfs_unlink_expected(&parent, "NAME.TXT", &super, 9U) == VFS_OK);
    assert(finish_calls == 2U && !pinned && !marks);
    reset();
    finish_uncertain = true;
    assert(vfs_unlink_expected_authorized(&parent, "NAME.TXT", &super, 9U)
           == VFS_ERR_UNCERTAIN);
    assert(pinned && attached && unresolved && finish_calls == 1U &&
           super.reference_count == 2U);
    reset();
    hook_uncertain = true;
    assert(vfs_unlink_trusted(&parent, "NAME.TXT") == VFS_ERR_UNCERTAIN);
    assert(pinned && attached && unresolved && !finish_calls &&
           super.reference_count == 2U);
    reset();
    detach_on_finish = true;
    assert(vfs_unlink(&parent, "NAME.TXT") == VFS_OK &&
           !live && !pinned && super.reference_count == 1U);
    reset();
    child_allowed = false;
    assert(vfs_unlink(&parent, "NAME.TXT") == VFS_ERR_ACCESS_DENIED &&
           !hook_calls && !pinned);
    reset();
    parent.ops = &plain_ops;
    assert(vfs_unlink(&parent, "NAME.TXT") == VFS_OK &&
           plain_calls == 1U && !hook_calls && !finish_calls);
    reset(); child.type = VFS_TYPE_DIRECTORY;
    assert(vfs_rmdir(&parent, "NAME.TXT") == VFS_OK &&
           hook_calls == 1U && finish_calls == 1U && !pinned);
    reset(); child.type = VFS_TYPE_DIRECTORY;
    finish_uncertain = true;
    assert(vfs_rmdir_expected(&parent, "NAME.TXT", &super, 9U) ==
           VFS_ERR_UNCERTAIN && pinned && attached && unresolved);
    reset(); child.type = VFS_TYPE_DIRECTORY;
    hook_uncertain = true;
    assert(vfs_rmdir_trusted(&parent, "NAME.TXT") == VFS_ERR_UNCERTAIN &&
           pinned && attached && unresolved && !finish_calls);
    reset(); child.type = VFS_TYPE_DIRECTORY;
    detach_on_finish = true;
    assert(vfs_rmdir_expected_authorized(&parent, "NAME.TXT", &super, 9U)
           == VFS_OK && !live && !pinned);
    reset(); child.type = VFS_TYPE_DIRECTORY;
    parent.ops = &plain_ops;
    assert(vfs_rmdir(&parent, "NAME.TXT") == VFS_OK &&
           plain_calls == 1U && !hook_calls && !finish_calls);
    reset();
    assert(vfs_rename(&parent, "NAME.TXT", &destination, "OTHER.TXT") == VFS_OK &&
           hook_calls == 1U && finish_calls == 1U && !pinned && !plain_calls);
    reset(); finish_busy_once = true;
    assert(vfs_rename_expected(&parent, "NAME.TXT", &super, 9U,
        &destination, "OTHER.TXT") == VFS_OK && finish_calls == 2U);
    reset(); destination_identity = 1U;
    assert(vfs_rename_expected(&parent, "NAME.TXT", &super, 9U,
        &destination, "OTHER.TXT") == VFS_OK && hook_calls == 1U && finish_calls == 1U);
    reset(); destination_identity = 2U;
    assert(vfs_rename_expected(&parent, "NAME.TXT", &super, 9U,
        &destination, "OTHER.TXT") == VFS_ERR_ALREADY_EXISTS && !hook_calls && !finish_calls);
    reset(); destination_access = false;
    assert(vfs_rename_expected(&parent, "NAME.TXT", &super, 9U,
        &destination, "OTHER.TXT") == VFS_ERR_ACCESS_DENIED && !hook_calls && !finish_calls);
    reset(); destination_access = false;
    assert(vfs_rename_expected_authorized(&parent, "NAME.TXT", &super, 9U,
        &destination, "OTHER.TXT") == VFS_OK && hook_calls == 1U);
    reset(); super.read_only = true;
    assert(vfs_rename_trusted(&parent, "NAME.TXT", &destination, "OTHER.TXT") ==
        VFS_ERR_ACCESS_DENIED && !hook_calls && !finish_calls);
    reset(); finish_uncertain = true;
    assert(vfs_rename_expected_authorized(&parent, "NAME.TXT", &super, 9U,
        &destination, "OTHER.TXT") == VFS_ERR_UNCERTAIN && pinned && attached && unresolved);
    reset(); hook_uncertain = true;
    assert(vfs_rename_trusted(&parent, "NAME.TXT", &destination, "OTHER.TXT") ==
           VFS_ERR_UNCERTAIN && pinned && attached && unresolved && !finish_calls);
    reset(); detach_on_finish = true;
    assert(vfs_rename(&parent, "NAME.TXT", &destination, "OTHER.TXT") == VFS_OK &&
           !live && !pinned && super.reference_count == 1U);
    reset(); child_allowed = false;
    assert(vfs_rename_expected(&parent, "NAME.TXT", &super, 9U,
        &destination, "OTHER.TXT") == VFS_ERR_ACCESS_DENIED && !hook_calls);
    reset();
    assert(vfs_rename_expected(&parent, "NAME.TXT", &super, 99U,
        &destination, "OTHER.TXT") == VFS_ERR_ACCESS_DENIED && !hook_calls);
    reset(); parent.ops = &plain_ops;
    assert(vfs_rename_trusted(&parent, "NAME.TXT", &destination, "OTHER.TXT") ==
           VFS_OK && plain_calls == 1U && !hook_calls && !finish_calls);
    for (unsigned mode = 0U; mode < 2U; mode++) {
        reset(); super.read_only = true; if (mode) parent.ops = &plain_ops;
        assert(vfs_unlink(&parent, "NAME.TXT") == VFS_ERR_ACCESS_DENIED);
        assert(vfs_unlink_trusted(&parent, "NAME.TXT") == VFS_ERR_ACCESS_DENIED);
        assert(vfs_unlink_expected(&parent, "NAME.TXT", &super, 9U) == VFS_ERR_ACCESS_DENIED);
        assert(vfs_unlink_expected_authorized(&parent, "NAME.TXT", &super, 9U) == VFS_ERR_ACCESS_DENIED);
        child.type = VFS_TYPE_DIRECTORY;
        assert(vfs_rmdir(&parent, "NAME.TXT") == VFS_ERR_ACCESS_DENIED);
        assert(vfs_rmdir_trusted(&parent, "NAME.TXT") == VFS_ERR_ACCESS_DENIED);
        assert(vfs_rmdir_expected(&parent, "NAME.TXT", &super, 9U) == VFS_ERR_ACCESS_DENIED);
        assert(vfs_rmdir_expected_authorized(&parent, "NAME.TXT", &super, 9U) == VFS_ERR_ACCESS_DENIED);
        assert(vfs_rename(&parent, "NAME.TXT", &destination, "OTHER.TXT") == VFS_ERR_ACCESS_DENIED);
        assert(vfs_rename_trusted(&parent, "NAME.TXT", &destination, "OTHER.TXT") == VFS_ERR_ACCESS_DENIED);
        assert(vfs_rename_expected_authorized(&parent, "NAME.TXT", &super, 9U, &destination, "OTHER.TXT") == VFS_ERR_ACCESS_DENIED);
        assert(!hook_calls && !plain_calls && !pinned && !finish_calls && super.reference_count == 1U);
    }
    reset(); writable = false;
    assert(vfs_unlink_trusted(&parent, "NAME.TXT") == VFS_OK && hook_calls == 1U);
    puts("VFS production unlink/rmdir/rename pin handoff and lifetime passed");
    return 0;
}
'''

prefix += r'''
/* Controlled non-opt-in admission; actual guards are covered by mgfs_handoff_test. */
int vfs_super_operation_error(const vfs_super_t *sb) { return vfs_super_is_live(sb) ? VFS_OK : VFS_ERR_DEVICE_GONE; }
void vfs_super_operation_pause(vfs_super_t *sb) { end_calls++; (void)mutex_unlock(&sb->operation_lock); }
bool vfs_super_operation_guard_release(vfs_super_t *sb) { (void)sb; return true; }
'''

prefix += empty_namespace_dependencies(source)

names = (
    "vfs_node_on_read_only_mount", "vfs_unlink_locked", "vfs_operation_finish_pin", "vfs_super_operation_complete", "vfs_node_operation_finish", "vfs_unlink",
    "vfs_unlink_trusted", "vfs_node_identity_matches",
    "vfs_unlink_expected_internal", "vfs_unlink_expected",
    "vfs_unlink_expected_authorized",
    "vfs_rmdir_locked", "vfs_rmdir", "vfs_rmdir_trusted",
    "vfs_rmdir_expected_internal", "vfs_rmdir_expected",
    "vfs_rmdir_expected_authorized",
    "vfs_rename_locked", "vfs_rename_finish", "vfs_rename", "vfs_rename_expected_internal",
    "vfs_rename_expected", "vfs_rename_expected_authorized", "vfs_rename_trusted",
)
code = prefix + "\n".join(function(name) for name in names) + tail
compiler = sys.argv[1] if len(sys.argv) > 1 else "gcc"
with tempfile.TemporaryDirectory(prefix="mangrove-vfs-unlink-pin-") as temp:
    fixture = Path(temp) / "fixture.c"
    binary = Path(temp) / "fixture"
    fixture.write_text(code)
    subprocess.run([compiler, "-O1", "-std=gnu11", "-Wall", "-Wextra",
                    "-Werror", "-fsanitize=address,undefined", "-pthread",
                    "-I" + str(root / "kernel/include"),
                    "-I" + str(root / "include"), "-idirafter",
                    str(root / "libc/include"), str(fixture), "-o",
                    str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
