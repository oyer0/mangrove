#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production VFS unlink/pin handoff with controlled lifecycle dependencies."""
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
static vfs_node_t parent, child;
static pthread_mutex_t operation = PTHREAD_MUTEX_INITIALIZER;
static bool live, writable, child_allowed, pinned, attached, unresolved;
static bool hook_uncertain, finish_uncertain, finish_busy_once, detach_on_finish;
static unsigned hook_calls, plain_calls, finish_calls, end_calls, marks;

bool vfs_node_is_live(const vfs_node_t *node)
{ return node == &parent && live; }
static bool vfs_node_on_read_only_mount(const vfs_node_t *node)
{ assert(node == &parent); return !writable; }
bool vfs_check_access(const vfs_node_t *node, u32 permission)
{ assert(node == &parent && permission == VFS_ACCESS_WRITE); return writable; }
static bool vfs_node_operation_begin(vfs_node_t *node, vfs_super_t **out)
{
    assert(node == &parent && out && live);
    assert(!pthread_mutex_lock(&operation));
    super.reference_count++;
    *out = &super;
    return true;
}
static void vfs_node_operation_end(vfs_super_t *sb)
{
    assert(sb == &super && super.reference_count > 1U);
    end_calls++;
    assert(!pthread_mutex_unlock(&operation));
    super.reference_count--;
}
static vfs_node_t *vfs_finddir_locked(vfs_node_t *dir, const char *name)
{ assert(dir == &parent && !strcmp(name, "NAME.TXT")); return &child; }
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
static const vfs_ops_t pinned_ops = {
    .unlink = fake_plain, .unlink_pinned = fake_pinned,
    .rmdir = fake_plain, .rmdir_pinned = fake_pinned,
};
static const vfs_ops_t plain_ops = {.unlink = fake_plain, .rmdir = fake_plain};
static void reset(void)
{
    memset(&super, 0, sizeof(super));
    memset(&parent, 0, sizeof(parent));
    memset(&child, 0, sizeof(child));
    super.reference_count = 1U;
    super.state = VFS_SUPER_ACTIVE;
    parent.super = child.super = &super;
    parent.type = VFS_TYPE_DIRECTORY;
    child.type = VFS_TYPE_FILE;
    child.inode = 9U;
    parent.ops = &pinned_ops;
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
    puts("VFS production unlink pin handoff and lifetime passed");
    return 0;
}
'''

names = (
    "vfs_unlink_locked", "vfs_unlink_finish_pin", "vfs_unlink",
    "vfs_unlink_trusted", "vfs_node_identity_matches",
    "vfs_unlink_expected_internal", "vfs_unlink_expected",
    "vfs_unlink_expected_authorized",
    "vfs_rmdir_locked", "vfs_rmdir", "vfs_rmdir_trusted",
    "vfs_rmdir_expected_internal", "vfs_rmdir_expected",
    "vfs_rmdir_expected_authorized",
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
