#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production VFS expectation and save admission with controlled facts.

The file-facts callback, credentials, mount read-only bit, and mutex scheduler
are fixtures. MGFS's actual facts callback is exercised by the stage fixture.
"""
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
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vfs.h>
#include <process.h>
#define VFS_FILE_HANDLE_VALID 0x56465348U
static pthread_mutex_t offset_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t operation_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t race_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t race_ready = PTHREAD_COND_INITIALIZER;
static bool race_wait, race_entered;
static vfs_super_t super;
static vfs_node_t node;
static vfs_file_facts_t disk_facts;
static process_t process;
static process_credentials_t credentials;
static bool device_live, read_only;
static bool current_process, owner_regular, current_admin, config_privilege;
static int facts_error;
static unsigned facts_calls, destroy_count;

void *kmalloc(usize size) { return malloc(size); }
void kfree(void *ptr) { free(ptr); }
void mutex_init(mutex_t *lock) { (void)lock; }
bool mutex_lock(mutex_t *lock) {
    (void)lock; return pthread_mutex_lock(&offset_mutex) == 0;
}
bool mutex_unlock(mutex_t *lock) {
    (void)lock; return pthread_mutex_unlock(&offset_mutex) == 0;
}
bool vfs_super_is_live(const vfs_super_t *sb) {
    return sb == &super && __atomic_load_n(&device_live, __ATOMIC_ACQUIRE) &&
           sb->state == VFS_SUPER_ACTIVE;
}
bool vfs_node_retain_super(vfs_node_t *n, vfs_super_t **out) {
    if (n != &node || !vfs_super_is_live(n->super)) return false;
    super.reference_count++; *out = &super; return true;
}
void vfs_super_release(vfs_super_t *sb) {
    assert(sb == &super && sb->reference_count);
    sb->reference_count--;
    if (!sb->reference_count) destroy_count++;
}
bool vfs_super_operation_lock(vfs_super_t *sb) {
    if (!vfs_super_is_live(sb)) return false;
    if (race_wait) {
        assert(!pthread_mutex_lock(&race_mutex));
        race_entered = true;
        assert(!pthread_cond_signal(&race_ready));
        assert(!pthread_mutex_unlock(&race_mutex));
    }
    if (pthread_mutex_lock(&operation_mutex)) return false;
    if (!vfs_super_is_live(sb)) {
        assert(!pthread_mutex_unlock(&operation_mutex)); return false;
    }
    return true;
}
void vfs_super_operation_unlock(vfs_super_t *sb) {
    assert(sb == &super); assert(!pthread_mutex_unlock(&operation_mutex));
}
static bool vfs_node_on_read_only_mount(const vfs_node_t *n) {
    assert(n == &node); return read_only;
}
process_t *process_current(void) { return current_process ? &process : NULL; }
bool process_get_credentials(const process_t *p, process_credentials_t *out) {
    if (p != &process) return false;
    *out = credentials; return true;
}
bool vfs_check_access(const vfs_node_t *n, u32 access) {
    if (n != &node) return false;
    if (!current_process) return true;
    u32 bit = credentials.uid == n->owner_uid ?
        VFS_PERMISSION_OWNER_WRITE : VFS_PERMISSION_OTHER_WRITE;
    if (access == VFS_ACCESS_WRITE) return (n->permissions & bit) != 0;
    return true;
}
bool identity_credentials_has_privilege(
    const process_credentials_t *c, identity_privilege_t privilege) {
    assert(c && c->uid == credentials.uid);
    return privilege == IDENTITY_PRIVILEGE_MANAGE_CONFIGURATION ?
        config_privilege : current_admin;
}
bool identity_credentials_is_admin(const process_credentials_t *c) {
    assert(c && c->uid == credentials.uid); return current_admin;
}
bool identity_registry_lookup_uid(mg_uid_t uid, user_identity_t *out) {
    if (uid != disk_facts.owner_uid || !owner_regular) return false;
    memset(out, 0, sizeof(*out)); out->uid = uid;
    out->role = MG_IDENTITY_ROLE_REGULAR; return true;
}
bool vfs_administrator_override_allowed(
    const vfs_node_t *n, const process_credentials_t *c) {
    (void)n; (void)c; return current_admin && owner_regular;
}
static int fake_facts(vfs_node_t *n, vfs_file_facts_t *out) {
    assert(n == &node); facts_calls++;
    if (facts_error) return facts_error;
    *out = disk_facts; return VFS_OK;
}
static const vfs_ops_t ops = {.file_facts = fake_facts};
static const vfs_ops_t unsupported_ops = {0};
int vfs_file_save_check(vfs_file_handle_t *, bool, vfs_file_facts_t *);
static void reset(void) {
    memset(&super, 0, sizeof(super)); memset(&node, 0, sizeof(node));
    memset(&process, 0, sizeof(process));
    super.state = VFS_SUPER_ACTIVE; super.reference_count = 1;
    node.super = &super; node.inode = 21; node.type = VFS_TYPE_FILE;
    node.owner_uid = 41; node.permissions = VFS_PERMISSION_OWNER_WRITE;
    node.ops = &ops;
    disk_facts = (vfs_file_facts_t){21, 7, 4096, 41,
                                    VFS_PERMISSION_OWNER_WRITE};
    credentials.uid = 41;
    device_live = current_process = owner_regular = true;
    current_admin = config_privilege = read_only = false;
    facts_error = 0; facts_calls = destroy_count = 0;
    race_wait = race_entered = false;
}
static void *queued_check(void *argument) {
    vfs_file_handle_t *handle = argument;
    int result = vfs_file_save_check(handle, true, NULL);
    assert(result == VFS_ERR_DEVICE_GONE);
    return NULL;
}
'''

names = [
    "vfs_open_node_internal", "vfs_open_node", "vfs_open_node_authorized",
    "vfs_open_node_user_authorized", "vfs_close",
    "vfs_save_facts_write_allowed", "vfs_file_save_check_locked",
    "vfs_file_save_check",
]

suffix = r'''
int main(void) {
    vfs_file_handle_t *handle = NULL;
    vfs_file_facts_t facts = {0};
    reset();
    assert(vfs_open_node(&node, VFS_OPEN_WRITE, &handle) == VFS_OK);
    assert(handle && handle->save_expectation_valid &&
           handle->save_record_id == 21 && handle->save_generation == 7);
    assert(super.reference_count == 2 && facts_calls == 1);
    assert(vfs_file_save_check(handle, true, &facts) == VFS_OK);
    assert(facts.record_id == 21 && facts.size == 4096);
    assert(vfs_file_save_check(handle, false, NULL) == VFS_ERR_ACCESS_DENIED);
    disk_facts.generation = 8; /* Ordinary write or truncate via another open. */
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_STALE_GENERATION);
    assert(handle->save_generation == 7);
    disk_facts.generation = 7;
    /* Rename changes a directory entry, not the opened record identity. */
    assert(vfs_file_save_check(handle, true, NULL) == VFS_OK);
    facts_error = VFS_ERR_UNCERTAIN;
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_UNCERTAIN);
    facts_error = 0;
    facts_error = VFS_ERR_NOT_FOUND; /* Unlinked record. */
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_NOT_FOUND);
    facts_error = 0; disk_facts.record_id = 22;
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_BAD_FORMAT);
    disk_facts.record_id = 21;
    disk_facts.permissions = VFS_PERMISSION_OTHER_READ;
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_ACCESS_DENIED);
    disk_facts.permissions = VFS_PERMISSION_OWNER_WRITE;
    disk_facts.owner_uid = 42;
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_ACCESS_DENIED);
    disk_facts.owner_uid = 41;
    read_only = true;
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_ACCESS_DENIED);
    read_only = false;
    assert(!pthread_mutex_lock(&operation_mutex));
    race_wait = true;
    pthread_t queued;
    unsigned prior_facts = facts_calls;
    assert(!pthread_create(&queued, NULL, queued_check, handle));
    assert(!pthread_mutex_lock(&race_mutex));
    while (!race_entered)
        assert(!pthread_cond_wait(&race_ready, &race_mutex));
    assert(!pthread_mutex_unlock(&race_mutex));
    __atomic_store_n(&device_live, false, __ATOMIC_RELEASE);
    assert(!pthread_mutex_unlock(&operation_mutex));
    assert(!pthread_join(queued, NULL));
    assert(facts_calls == prior_facts);
    device_live = false;
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_DEVICE_GONE);
    assert(vfs_close(handle) == VFS_OK && super.reference_count == 1);

    reset(); node.ops = &unsupported_ops;
    assert(vfs_open_node(&node, VFS_OPEN_WRITE, &handle) == VFS_OK);
    assert(!handle->save_expectation_valid && facts_calls == 0);
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_UNSUPPORTED);
    assert(vfs_close(handle) == VFS_OK);
    reset(); node.type = VFS_TYPE_DIRECTORY;
    assert(vfs_open_node(&node, VFS_OPEN_WRITE, &handle) == VFS_OK);
    assert(!handle->save_expectation_valid);
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_UNSUPPORTED);
    assert(vfs_close(handle) == VFS_OK);

    reset();
    facts_error = VFS_ERR_IO;
    assert(vfs_open_node(&node, VFS_OPEN_WRITE, &handle) == VFS_OK);
    assert(handle && !handle->save_expectation_valid);
    facts_error = 0;
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_IO);
    assert(vfs_close(handle) == VFS_OK && super.reference_count == 1);
    reset();
    facts_error = VFS_ERR_UNCERTAIN;
    assert(vfs_open_node(&node, VFS_OPEN_WRITE, &handle) == VFS_OK);
    assert(!handle->save_expectation_valid);
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_UNCERTAIN);
    assert(vfs_close(handle) == VFS_OK);
    reset();
    assert(vfs_open_node(&node, VFS_OPEN_READ, &handle) == VFS_OK);
    assert(handle->save_expectation_valid);
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_ACCESS_DENIED);
    assert(vfs_close(handle) == VFS_OK);

    reset(); credentials.uid = 7; current_admin = true;
    node.permissions = 0; disk_facts.permissions = VFS_PERMISSION_OWNER_WRITE;
    assert(vfs_open_node_user_authorized(&node, VFS_OPEN_WRITE, &handle) == VFS_OK);
    assert(vfs_file_save_check(handle, true, NULL) == VFS_OK);
    disk_facts.owner_uid = 42;
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_ACCESS_DENIED);
    disk_facts.owner_uid = 41; current_admin = false;
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_ACCESS_DENIED);
    assert(vfs_close(handle) == VFS_OK);

    reset(); credentials.uid = 7; config_privilege = true;
    node.permissions = 0; disk_facts.permissions = VFS_PERMISSION_OWNER_WRITE;
    assert(vfs_open_node_authorized(&node, VFS_OPEN_WRITE, &handle) == VFS_OK);
    assert(vfs_file_save_check(handle, true, NULL) == VFS_OK);
    disk_facts.owner_uid = 42;
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_ACCESS_DENIED);
    disk_facts.owner_uid = 41;
    config_privilege = false;
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_ACCESS_DENIED);
    assert(vfs_close(handle) == VFS_OK);
    puts("VFS file facts and save admission: production paths passed");
    return 0;
}
'''

code = prefix + "\n".join(function(name) for name in names) + suffix
with tempfile.TemporaryDirectory(prefix="mangrove-vfs-facts-") as directory:
    path = Path(directory)
    fixture = path / "fixture.c"
    fixture.write_text(code)
    binary = path / "facts-test"
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
