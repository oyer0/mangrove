#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production VFS write/truncate pin dispatch with controlled locks."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "kernel/src/vfs.c").read_text()
object_source = (root / "kernel/src/object.c").read_text()
syscall_source = (root / "kernel/src/syscall.c").read_text()


def function(name, body=source):
    for match in re.finditer(r"^(?:static )?[^\n;{}]*\b" + name + r"\s*\(",
                             body, re.M):
        brace = body.find("{", match.end())
        if brace < 0 or ";" in body[match.end():brace]:
            continue
        depth, end = 1, brace + 1
        while depth:
            depth += (body[end] == "{") - (body[end] == "}")
            end += 1
        return body[match.start():end] + "\n"
    raise AssertionError(name)


prefix = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <vfs.h>
#include <object.h>
#include <syscall.h>
#include <mangrove_errors.h>
#define VFS_FILE_HANDLE_VALID 0x56465348U
typedef struct { kernel_object_t base; vfs_file_handle_t *handle; } file_object_t;

static vfs_super_t super;
static vfs_node_t node;
static vfs_file_handle_t handle;
static pthread_mutex_t operation = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t offset = PTHREAD_MUTEX_INITIALIZER;
static bool live, writable, readonly, pinned, attached, unresolved;
static bool finish_fails, finish_busy_once, reacquire_fails, detach_on_finish;
static bool pin_exhausted, hook_uncertain;
static bool uncertain_with_active_pin;
static bool expect_offset_lock;
static int hook_status;
static u64 hook_bytes, resize_length;
static unsigned hooks, plain, finishes, marks, mutations, begins, ends;

bool mutex_lock(mutex_t *lock)
{
    if (lock == &handle.offset_lock) return pthread_mutex_lock(&offset) == 0;
    assert(lock == &super.operation_lock);
    return !reacquire_fails && pthread_mutex_lock(&operation) == 0;
}
bool mutex_unlock(mutex_t *lock)
{
    if (lock == &handle.offset_lock) return pthread_mutex_unlock(&offset) == 0;
    assert(lock == &super.operation_lock);
    return pthread_mutex_unlock(&operation) == 0;
}
bool vfs_super_is_live(const vfs_super_t *sb)
{ return sb == &super && live; }
bool vfs_super_operation_lock(vfs_super_t *sb)
{ assert(sb == &super); return live && pthread_mutex_lock(&operation) == 0; }
void vfs_super_operation_unlock(vfs_super_t *sb)
{ assert(sb == &super); assert(!pthread_mutex_unlock(&operation)); }
static bool vfs_node_operation_begin(vfs_node_t *target, vfs_super_t **out)
{
    if (target != &node || !live) return false;
    assert(!pthread_mutex_lock(&operation));
    super.reference_count++; begins++; *out = &super;
    return true;
}
static void vfs_node_operation_end(vfs_super_t *sb)
{
    assert(sb == &super && super.reference_count > 1U);
    ends++; assert(!pthread_mutex_unlock(&operation));
    super.reference_count--;
}
static bool vfs_handle_valid(const vfs_file_handle_t *target)
{ return target == &handle && target->valid == VFS_FILE_HANDLE_VALID && live; }
static bool vfs_handle_authorized(const vfs_file_handle_t *target, u32 permission)
{ assert(target == &handle && permission == VFS_ACCESS_WRITE); return writable; }
bool vfs_node_is_live(const vfs_node_t *target)
{ return target == &node && live; }
static bool vfs_node_on_read_only_mount(const vfs_node_t *target)
{ assert(target == &node); return readonly; }
bool vfs_check_access(const vfs_node_t *target, u32 permission)
{ assert(target == &node && permission == VFS_ACCESS_WRITE); return writable; }

static u64 plain_write(vfs_node_t *target, u64 at, u64 size, const void *buffer)
{
    assert(target == &node && buffer && at == handle.offset);
    assert(pthread_mutex_trylock(&operation) == EBUSY);
    plain++;
    return size;
}
static int plain_truncate(vfs_node_t *target)
{ assert(target == &node); plain++; return VFS_OK; }
static int pinned_write(vfs_node_t *target, u64 at, u64 size,
                        const void *buffer, u64 *out_bytes, u64 *out_pin)
{
    assert(target == &node && buffer && at == handle.offset && size &&
           out_bytes && out_pin && pthread_mutex_trylock(&operation) == EBUSY);
    if (expect_offset_lock) assert(pthread_mutex_trylock(&offset) == EBUSY);
    hooks++;
    if (pin_exhausted) return VFS_ERR_NO_SPACE;
    assert(!pinned);
    pinned = attached = true;
    super.reference_count++;
    mutations++;
    if (hook_uncertain) {
        if (uncertain_with_active_pin) *out_pin = 47U;
        else unresolved = true;
        return VFS_ERR_UNCERTAIN;
    }
    *out_bytes = hook_bytes;
    *out_pin = 47U;
    return hook_status;
}
static int pinned_truncate(vfs_node_t *target, u64 *out_pin)
{
    assert(target == &node && out_pin &&
           pthread_mutex_trylock(&operation) == EBUSY);
    hooks++;
    if (pin_exhausted) return VFS_ERR_NO_SPACE;
    assert(!pinned);
    pinned = attached = true;
    super.reference_count++;
    mutations++;
    if (hook_uncertain) {
        if (uncertain_with_active_pin) *out_pin = 47U;
        else unresolved = true;
        return VFS_ERR_UNCERTAIN;
    }
    *out_pin = 47U;
    return hook_status;
}
static int pinned_resize(vfs_node_t *target, u64 length, u64 *out_pin)
{
    assert(pthread_mutex_trylock(&offset) == EBUSY);
    resize_length = length;
    return pinned_truncate(target, out_pin);
}
static const vfs_ops_t resize_ops = { .resize_pinned = pinned_resize };
int vfs_recovery_pin_finish_handoff(u64 id)
{
    assert(id == 47U && pinned && attached && !unresolved);
    assert(!pthread_mutex_trylock(&operation));
    assert(!pthread_mutex_unlock(&operation));
    finishes++;
    if (finish_busy_once) { finish_busy_once = false; return VFS_ERR_BUSY; }
    if (detach_on_finish) live = false;
    if (finish_fails) { unresolved = true; return VFS_ERR_UNCERTAIN; }
    pinned = attached = false;
    assert(super.reference_count > 1U);
    super.reference_count--;
    return VFS_OK;
}
int vfs_recovery_pin_mark_unresolved(u64 id)
{ assert(id == 47U && pinned); marks++; unresolved = true; return VFS_OK; }
static const vfs_ops_t pinned_ops = {
    .write = plain_write, .write_pinned = pinned_write,
    .truncate = plain_truncate, .truncate_pinned = pinned_truncate,
};
static const vfs_ops_t plain_ops = {
    .write = plain_write, .truncate = plain_truncate,
};
static void reset(void)
{
    memset(&super, 0, sizeof(super));
    memset(&node, 0, sizeof(node));
    memset(&handle, 0, sizeof(handle));
    super.reference_count = 1U;
    node.super = &super;
    node.type = VFS_TYPE_FILE;
    node.ops = &pinned_ops;
    handle.node = &node;
    handle.super = &super;
    handle.flags = VFS_OPEN_RDWR;
    handle.save_record_id = 41U; handle.save_generation = 17U;
    handle.save_expectation_valid = true;
    handle.valid = VFS_FILE_HANDLE_VALID;
    live = writable = true;
    readonly = pinned = attached = unresolved = false;
    finish_fails = finish_busy_once = reacquire_fails = detach_on_finish = false;
    pin_exhausted = hook_uncertain = uncertain_with_active_pin = false;
    hook_status = VFS_OK;
    hook_bytes = 3U;
    expect_offset_lock = true;
    hooks = plain = finishes = marks = mutations = begins = ends = 0U;
}
'''

tail = r'''
int main(void)
{
    u64 written = 99U;
    char input[4] = "abc";
    reset();
    assert(vfs_file_write_status(&handle, 3U, input, &written) == VFS_OK);
    assert(written == 3U && handle.offset == 3U && hooks == 1U &&
           finishes == 1U && !pinned && super.reference_count == 1U &&
           handle.save_record_id == 41U && handle.save_generation == 17U &&
           handle.save_expectation_valid);
    reset(); finish_busy_once = true;
    assert(vfs_file_write_status(&handle, 3U, input, &written) == VFS_OK &&
           finishes == 2U && !marks);
    reset(); finish_busy_once = reacquire_fails = true;
    assert(vfs_file_write_status(&handle, 3U, input, &written) ==
           VFS_ERR_UNCERTAIN && written == 0U && handle.offset == 0U &&
           pinned && attached && unresolved && marks == 1U);
    reset(); finish_fails = true;
    assert(vfs_file_write_status(&handle, 3U, input, &written) ==
           VFS_ERR_UNCERTAIN && written == 0U && pinned && unresolved);
    reset(); hook_uncertain = true;
    assert(vfs_file_write_status(&handle, 3U, input, &written) ==
           VFS_ERR_UNCERTAIN && written == 0U && !finishes && pinned &&
           unresolved && handle.offset == 0U);
    reset(); hook_uncertain = uncertain_with_active_pin = true;
    assert(vfs_file_write_status(&handle, 3U, input, &written) ==
           VFS_ERR_UNCERTAIN && written == 0U && !finishes && marks == 1U &&
           pinned && unresolved);
    reset(); pin_exhausted = true;
    assert(vfs_file_write_status(&handle, 3U, input, &written) ==
           VFS_ERR_NO_SPACE && !mutations && !pinned && !finishes);
    reset(); hook_status = VFS_ERR_IO; hook_bytes = 2U;
    assert(vfs_file_write_status(&handle, 3U, input, &written) == VFS_OK &&
           written == 2U && handle.offset == 2U && !pinned);
    reset(); hook_status = VFS_ERR_IO; hook_bytes = 0U;
    assert(vfs_file_write_status(&handle, 3U, input, &written) == VFS_ERR_IO &&
           !written && !handle.offset && !pinned);
    reset(); detach_on_finish = true;
    assert(vfs_file_write_status(&handle, 3U, input, &written) == VFS_OK &&
           written == 3U && !pinned && super.reference_count == 1U);
    reset(); live = false;
    assert(vfs_file_write_status(&handle, 3U, input, &written) ==
           VFS_ERR_DEVICE_GONE && !hooks && !mutations);
    reset(); writable = false;
    assert(vfs_file_write_status(&handle, 3U, input, &written) ==
           VFS_ERR_ACCESS_DENIED && !hooks);
    reset(); readonly = true;
    assert(vfs_file_write_status(&handle, 3U, input, &written) ==
           VFS_ERR_ACCESS_DENIED && !hooks);
    reset();
    assert(vfs_file_write_status(&handle, 0U, NULL, &written) == VFS_OK &&
           !written && !hooks && !mutations);
    reset();
    assert(vfs_file_write_status(&handle, 3U, NULL, &written) ==
           VFS_ERR_INVALID_PARAM && !hooks);
    reset(); node.ops = &plain_ops;
    assert(vfs_file_write_status(&handle, 3U, input, &written) == VFS_OK &&
           written == 3U && plain == 1U && !hooks && !finishes);
    reset();
    assert(vfs_truncate_handle(&handle) == VFS_OK && hooks == 1U &&
           finishes == 1U && !pinned);
    reset(); node.ops = &resize_ops; handle.offset = 91U;
    assert(vfs_resize_handle(&handle, 513U) == VFS_OK && resize_length == 513U &&
           hooks == 1U && finishes == 1U && !pinned && handle.offset == 91U &&
           handle.save_generation == 17U && handle.save_expectation_valid);
    reset(); node.ops = &resize_ops;
    assert(vfs_truncate_handle(&handle) == VFS_OK && resize_length == 0U);
    reset(); node.ops = &resize_ops; hook_uncertain = true;
    assert(vfs_resize_handle(&handle, 1201U) == VFS_ERR_UNCERTAIN &&
           pinned && unresolved && !finishes);
    reset(); node.ops = &resize_ops; finish_fails = true;
    assert(vfs_resize_handle(&handle, 1201U) == VFS_ERR_UNCERTAIN && pinned);
    reset(); node.ops = &resize_ops; readonly = true;
    assert(vfs_resize_handle(&handle, 1201U) == VFS_ERR_ACCESS_DENIED && !hooks);
    reset(); node.ops = &resize_ops; live = false;
    assert(vfs_resize_handle(&handle, 1201U) == VFS_ERR_DEVICE_GONE && !hooks);
    reset(); node.ops = &plain_ops;
    assert(vfs_resize_handle(&handle, 1201U) == VFS_ERR_UNSUPPORTED && !plain);
    reset(); finish_fails = true;
    assert(vfs_truncate_handle(&handle) == VFS_ERR_UNCERTAIN &&
           pinned && unresolved);
    reset(); hook_uncertain = true;
    assert(vfs_truncate(&node) == VFS_ERR_UNCERTAIN && begins == 1U &&
           ends == 1U && pinned && unresolved);
    reset(); hook_uncertain = uncertain_with_active_pin = true;
    assert(vfs_truncate_handle(&handle) == VFS_ERR_UNCERTAIN &&
           pinned && unresolved && marks == 1U);
    reset();
    assert(vfs_truncate_trusted(&node) == VFS_OK && begins == 1U &&
           ends == 1U && !pinned);
    reset(); pin_exhausted = true;
    assert(vfs_truncate_handle(&handle) == VFS_ERR_NO_SPACE &&
           !mutations && !pinned);
    reset(); node.ops = &plain_ops;
    assert(vfs_truncate_handle(&handle) == VFS_OK && plain == 1U && !hooks);
    reset();
    assert(vfs_file_write_trusted(&handle, 3U, input) == 3U &&
           finishes == 1U && !pinned);
    reset();
    file_object_t object = {0};
    object.base.type = OBJECT_TYPE_FILE;
    object.handle = &handle;
    assert(file_write(&object.base, input, 3U) == 3 && !pinned);
    reset(); hook_uncertain = true;
    assert(file_write(&object.base, input, 3U) == MG_ERR_UNCERTAIN &&
           syscall_object_io_result(true, MG_ERR_UNCERTAIN) ==
           MG_ERR_UNCERTAIN);
    assert(syscall_object_io_result(false, MG_ERR_UNCERTAIN) ==
           MG_ERR_UNSUPPORTED);
    reset(); pin_exhausted = true;
    assert(file_write(&object.base, input, 3U) == MG_ERR_NO_SPACE &&
           syscall_object_io_result(true, MG_ERR_NO_SPACE) ==
           MG_ERR_NO_SPACE);
    reset(); hook_uncertain = true; expect_offset_lock = false;
    assert(vfs_write(&node, 0U, 3U, input) == 0U && pinned && unresolved &&
           begins == 1U && ends == 1U);
    printf("VFS production write/truncate pin handoff passed; "
           "ops table=%zu bytes, per-call heap=0\n", sizeof(vfs_ops_t));
    return 0;
}
'''

names = (
    "vfs_operation_finish_pin", "vfs_write_dispatch", "vfs_file_write_status",
    "vfs_file_write", "vfs_file_write_trusted", "vfs_truncate_dispatch",
    "vfs_resize_handle", "vfs_truncate_handle", "vfs_truncate", "vfs_truncate_trusted", "vfs_write",
)
code = (prefix + "\n".join(function(name) for name in names) +
        function("file_write", object_source) +
        function("syscall_object_io_result", syscall_source) + tail)
compiler = sys.argv[1] if len(sys.argv) > 1 else "gcc"
with tempfile.TemporaryDirectory(prefix="mangrove-vfs-write-pin-") as temp:
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
