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
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vfs.h>
#include <process.h>
#define VFS_FILE_HANDLE_VALID 0x56465348U
static pthread_mutex_t offset_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t operation_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t metadata_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t race_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t race_ready = PTHREAD_COND_INITIALIZER;
static bool race_wait, race_entered;
static vfs_super_t super;
static vfs_node_t node;
static vfs_node_t second_node;
static vfs_file_facts_t disk_facts;
static process_t process;
static process_credentials_t credentials;
static bool device_live;
static bool current_process, owner_regular, current_admin, config_privilege;
static int facts_error;
static unsigned facts_calls, destroy_count;
static unsigned snapshot_calls;
static unsigned replace_calls, finish_calls;
static bool pin_live, fail_finish, remove_during_replace;
static int replace_result;
static u64 replacement_size;
static int snapshot_error;
static bool snapshot_bad_revision, snapshot_remove_during;
static bool snapshot_pause, snapshot_entered, snapshot_resume;
static bool sink_fail, finish_fail;
static usize streamed_bytes;
static pthread_cond_t snapshot_ready = PTHREAD_COND_INITIALIZER;

static u64 fixture_spin_lock(spinlock_t *lock) {
    (void)lock; assert(!pthread_mutex_lock(&metadata_mutex)); return 0;
}
static void fixture_spin_unlock(spinlock_t *lock, u64 flags) {
    (void)lock; (void)flags; assert(!pthread_mutex_unlock(&metadata_mutex));
}
#define spin_lock_irqsave(lock) fixture_spin_lock(lock)
#define spin_unlock_irqrestore(lock, flags) fixture_spin_unlock(lock, flags)
static spinlock_t vfs_metadata_lock;

void *kmalloc(usize size) { return malloc(size); }
void kfree(void *ptr) { free(ptr); }
void mutex_init(mutex_t *lock) { (void)lock; }
bool mutex_lock(mutex_t *lock) {
    return pthread_mutex_lock(lock == &super.operation_lock ?
                              &operation_mutex : &offset_mutex) == 0;
}
bool mutex_unlock(mutex_t *lock) {
    return pthread_mutex_unlock(lock == &super.operation_lock ?
                                &operation_mutex : &offset_mutex) == 0;
}
bool vfs_super_is_live(const vfs_super_t *sb) {
    return sb == &super && __atomic_load_n(&device_live, __ATOMIC_ACQUIRE) &&
           sb->state == VFS_SUPER_ACTIVE;
}
bool vfs_node_retain_super(vfs_node_t *n, vfs_super_t **out) {
    if ((n != &node && n != &second_node) ||
        !vfs_super_is_live(n->super)) return false;
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
process_t *process_current(void) { return current_process ? &process : NULL; }
bool process_get_credentials(const process_t *p, process_credentials_t *out) {
    if (p != &process) return false;
    *out = credentials; return true;
}
bool vfs_check_access(const vfs_node_t *n, u32 access) {
    if (n != &node && n != &second_node) return false;
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
    assert(n == &node || n == &second_node); facts_calls++;
    if (facts_error) return facts_error;
    *out = disk_facts; return VFS_OK;
}
static int fake_snapshot(vfs_node_t *n, char *data, usize capacity,
                         mg_file_lines_state_t *state) {
    assert(n == &node || n == &second_node);
    assert(pthread_mutex_trylock(&operation_mutex) == EBUSY);
    assert(pthread_mutex_trylock(&offset_mutex) == EBUSY);
    snapshot_calls++;
    if (snapshot_pause) {
        assert(!pthread_mutex_lock(&race_mutex));
        snapshot_entered = true;
        assert(!pthread_cond_signal(&snapshot_ready));
        while (!snapshot_resume)
            assert(!pthread_cond_wait(&snapshot_ready, &race_mutex));
        assert(!pthread_mutex_unlock(&race_mutex));
    }
    if (snapshot_error) return snapshot_error;
    if (disk_facts.size > capacity) return VFS_ERR_BUFFER_TOO_SMALL;
    memset(data, 's', (usize)disk_facts.size);
    *state = (mg_file_lines_state_t){
        .identifier = disk_facts.record_id,
        .revision = disk_facts.generation + (snapshot_bad_revision ? 1U : 0U),
        .length = (usize)disk_facts.size,
    };
    if (snapshot_remove_during)
        __atomic_store_n(&device_live, false, __ATOMIC_RELEASE);
    return VFS_OK;
}
static int fake_snapshot_stream(vfs_node_t *n, usize capacity,
                                vfs_snapshot_sink_t sink, void *context,
                                mg_file_lines_state_t *state) {
    char chunk[4096];
    assert(n == &node || n == &second_node);
    assert(pthread_mutex_trylock(&operation_mutex) == EBUSY);
    assert(pthread_mutex_trylock(&offset_mutex) == EBUSY);
    if (snapshot_error) return snapshot_error;
    if (disk_facts.size > capacity) return VFS_ERR_BUFFER_TOO_SMALL;
    memset(chunk, 's', sizeof(chunk));
    for (u64 offset = 0; offset < disk_facts.size;) {
        usize count = disk_facts.size - offset > sizeof(chunk)
            ? sizeof(chunk) : (usize)(disk_facts.size - offset);
        int result = sink(context, offset, (const u8 *)chunk, count);
        if (result != VFS_OK) return result;
        offset += count;
    }
    *state = (mg_file_lines_state_t){disk_facts.record_id,
        disk_facts.generation + (snapshot_bad_revision ? 1U : 0U),
        (usize)disk_facts.size, 0};
    if (snapshot_remove_during)
        __atomic_store_n(&device_live, false, __ATOMIC_RELEASE);
    return VFS_OK;
}
static int stream_sink(void *context, u64 offset, const u8 *data, usize size) {
    char *out = context;
    if (sink_fail && offset) return VFS_ERR_INVALID_PARAM;
    memcpy(out + offset, data, size);
    streamed_bytes += size;
    return VFS_OK;
}
static int stream_finish(void *context, const mg_file_lines_state_t *state) {
    (void)context;
    assert(state->revision == disk_facts.generation);
    return finish_fail ? VFS_ERR_INVALID_PARAM : VFS_OK;
}
static u32 source(void *context, u64 offset, u8 *buffer, u32 requested) {
    (void)context; (void)offset; memset(buffer, 'x', requested);
    return requested;
}
static int fake_replace(vfs_super_t *sb, u64 record_id,
                        u64 generation, u64 length,
                        vfs_replace_source_t read_source, void *context,
                        u64 *out_pin) {
    assert(sb == &super && record_id == node.inode);
    assert(pthread_mutex_trylock(&operation_mutex) == EBUSY);
    assert(generation == disk_facts.generation &&
           (length == 0 || read_source == source));
    (void)context; replace_calls++;
    if (remove_during_replace) {
        __atomic_store_n(&device_live, false, __ATOMIC_RELEASE);
        return VFS_ERR_IO;
    }
    if (replace_result != VFS_OK) return replace_result;
    disk_facts.generation++;
    disk_facts.size = replacement_size = length;
    vfs_nodes_update_size(sb, record_id, length);
    pin_live = true; *out_pin = 42;
    return VFS_OK;
}
int vfs_recovery_pin_finish_handoff(u64 pin) {
    assert(pin == 42 && pin_live);
    assert(!pthread_mutex_trylock(&operation_mutex));
    assert(!pthread_mutex_unlock(&operation_mutex));
    finish_calls++;
    if (fail_finish) return VFS_ERR_UNCERTAIN;
    pin_live = false; return VFS_OK;
}
int vfs_recovery_pin_mark_unresolved(u64 pin) {
    assert(pin == 42 && pin_live); return VFS_OK;
}
static const vfs_ops_t ops = {
    .file_facts = fake_facts, .replace = fake_replace,
    .snapshot = fake_snapshot, .snapshot_stream = fake_snapshot_stream,
    .replacement_max_bytes = 512U * 1024U,
};
static const vfs_ops_t unsupported_ops = {0};
static const vfs_ops_t snapshot_only_ops = {.snapshot = fake_snapshot};
int vfs_file_save_check(vfs_file_handle_t *, bool, vfs_file_facts_t *);
static void reset(void) {
    memset(&super, 0, sizeof(super)); memset(&node, 0, sizeof(node));
    memset(&second_node, 0, sizeof(second_node));
    memset(&process, 0, sizeof(process));
    super.state = VFS_SUPER_ACTIVE; super.reference_count = 1;
    node.super = &super; node.inode = 21; node.type = VFS_TYPE_FILE;
    node.size = 4096; node.next_in_super = &second_node;
    node.owner_uid = 41; node.permissions = VFS_PERMISSION_OWNER_WRITE;
    node.ops = &ops;
    second_node.super = &super; second_node.inode = 21;
    second_node.type = VFS_TYPE_FILE; second_node.size = 4096;
    second_node.owner_uid = 41;
    second_node.permissions = VFS_PERMISSION_OWNER_WRITE;
    second_node.ops = &ops;
    super.nodes = &node;
    disk_facts = (vfs_file_facts_t){21, 7, 4096, 41,
                                    VFS_PERMISSION_OWNER_WRITE};
    credentials.uid = 41;
    device_live = current_process = owner_regular = true;
    current_admin = config_privilege = super.read_only = false;
    facts_error = 0; facts_calls = destroy_count = 0;
    snapshot_calls = 0; snapshot_error = 0;
    snapshot_bad_revision = snapshot_remove_during = false;
    sink_fail = finish_fail = false; streamed_bytes = 0;
    snapshot_pause = snapshot_entered = snapshot_resume = false;
    replace_calls = finish_calls = 0;
    pin_live = fail_finish = remove_during_replace = false;
    replace_result = VFS_OK;
    replacement_size = 0;
    race_wait = race_entered = false;
}
static void *queued_check(void *argument) {
    vfs_file_handle_t *handle = argument;
    int result = vfs_file_save_check(handle, true, NULL);
    assert(result == VFS_ERR_DEVICE_GONE);
    return NULL;
}
static void *queued_replace(void *argument) {
    assert(vfs_file_replace(argument, true, 5, source, NULL) ==
           VFS_ERR_DEVICE_GONE);
    return NULL;
}
static void *replace_after_snapshot(void *argument) {
    assert(vfs_file_replace(argument, true, 5, source, NULL) == VFS_OK);
    return NULL;
}
static void *queued_close(void *argument) {
    assert(vfs_close(argument) == VFS_OK);
    return NULL;
}
static void *queued_snapshot(void *argument) {
    char data[16]; mg_file_lines_state_t state = {0};
    assert(vfs_file_snapshot(argument, data, sizeof(data), &state) == VFS_OK);
    assert(state.length == 5 && state.revision == 8);
    return NULL;
}
static void *queued_snapshot_dead(void *argument) {
    char data[16]; mg_file_lines_state_t state = {0};
    assert(vfs_file_snapshot(argument, data, sizeof(data), &state) ==
           VFS_ERR_DEVICE_GONE);
    return NULL;
}
static void *queued_metadata(void *argument) {
    vfs_node_metadata_t metadata;
    assert(vfs_node_metadata_snapshot(argument, &metadata) == VFS_OK);
    assert(metadata.size == replacement_size);
    return NULL;
}
'''

names = [
    "vfs_node_on_read_only_mount",
    "vfs_nodes_update_size",
    "vfs_node_metadata_snapshot",
    "vfs_open_node_internal", "vfs_open_node", "vfs_open_node_authorized",
    "vfs_open_node_user_authorized", "vfs_close",
    "vfs_save_facts_write_allowed", "vfs_file_save_check_locked",
    "vfs_file_save_check",
    "vfs_file_replace", "vfs_handle_valid", "vfs_file_snapshot",
    "vfs_file_snapshot_stream",
    "vfs_seek",
]

suffix = r'''
int main(void) {
    vfs_file_handle_t *handle = NULL;
    vfs_file_facts_t facts = {0};
    char snapshot_data[16];
    mg_file_lines_state_t snapshot_state = {0};
    char streamed[8192];
    reset(); disk_facts.size = 8192;
    disk_facts.permissions = VFS_PERMISSION_OWNER_READ | VFS_PERMISSION_OWNER_WRITE;
    assert(vfs_open_node(&node, VFS_OPEN_READ, &handle) == VFS_OK);
    disk_facts.generation = 8;
    assert(vfs_file_snapshot_stream(handle, sizeof(streamed), stream_sink,
                                    stream_finish, streamed) == VFS_OK);
    assert(streamed_bytes == sizeof(streamed) && streamed[8191] == 's' &&
           handle->save_generation == 8 && handle->offset == 0);
    char *large_stream = malloc(MG_FILE_CONTENT_MAX_BYTES);
    assert(large_stream);
    disk_facts.size = MG_FILE_CONTENT_MAX_BYTES;
    streamed_bytes = 0;
    assert(vfs_file_snapshot_stream(handle, MG_FILE_CONTENT_MAX_BYTES,
                                    stream_sink, stream_finish,
                                    large_stream) == VFS_OK);
    assert(streamed_bytes == MG_FILE_CONTENT_MAX_BYTES &&
           large_stream[MG_FILE_CONTENT_MAX_BYTES - 1U] == 's');
    free(large_stream);
    disk_facts.size = sizeof(streamed);
    disk_facts.generation = 9;
    sink_fail = true;
    assert(vfs_file_snapshot_stream(handle, sizeof(streamed), stream_sink,
                                    stream_finish, streamed) == VFS_ERR_INVALID_PARAM);
    assert(handle->save_generation == 8);
    sink_fail = false; finish_fail = true;
    assert(vfs_file_snapshot_stream(handle, sizeof(streamed), stream_sink,
                                    stream_finish, streamed) == VFS_ERR_INVALID_PARAM);
    assert(handle->save_generation == 8);
    finish_fail = false;
    assert(vfs_file_snapshot_stream(handle, sizeof(streamed) - 1U, stream_sink,
                                    stream_finish, streamed) == VFS_ERR_BUFFER_TOO_SMALL);
    assert(handle->save_generation == 8);
    assert(vfs_close(handle) == VFS_OK);
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
    super.read_only = true;
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_ACCESS_DENIED);
    super.read_only = false;
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

    reset();
    disk_facts.size = 5;
    disk_facts.permissions |= VFS_PERMISSION_OWNER_READ;
    node.permissions |= VFS_PERMISSION_OWNER_READ;
    assert(vfs_open_node(&node, VFS_OPEN_READ, &handle) == VFS_OK);
    disk_facts.generation = 8;
    snapshot_remove_during = true;
    assert(vfs_file_snapshot(handle, snapshot_data, sizeof(snapshot_data),
                             &snapshot_state) == VFS_ERR_DEVICE_GONE);
    assert(handle->save_generation == 7);
    assert(vfs_close(handle) == VFS_OK && super.reference_count == 1);

    reset();
    vfs_file_handle_t *other = NULL;
    assert(vfs_open_node(&node, VFS_OPEN_WRITE, &handle) == VFS_OK);
    assert(vfs_open_node(&second_node, VFS_OPEN_WRITE, &other) == VFS_OK);
    assert(vfs_file_replace(handle, false, 8192, source, NULL) ==
           VFS_ERR_ACCESS_DENIED && !replace_calls);
    super.read_only = true;
    assert(vfs_file_replace(handle, true, 8192, source, NULL) ==
           VFS_ERR_ACCESS_DENIED && !replace_calls);
    super.read_only = false;
    assert(vfs_file_replace(handle, true, 512U * 1024U + 1U,
                            source, NULL) == VFS_ERR_NO_SPACE &&
           !replace_calls);
    replace_result = VFS_ERR_IO;
    assert(vfs_file_replace(handle, true, 8192, source, NULL) == VFS_ERR_IO &&
           handle->save_generation == 7 && !pin_live);
    replace_result = VFS_OK;
    assert(vfs_file_replace(handle, true, 8192, source, NULL) == VFS_OK);
    assert(replace_calls == 2 && finish_calls == 1 && !pin_live);
    assert(handle->save_generation == 8 && other->save_generation == 7);
    assert(node.size == 8192 && second_node.size == 8192);
    u64 end = 0;
    assert(vfs_seek(other, 0, VFS_SEEK_END, &end) == VFS_OK && end == 8192);
    assert(vfs_file_replace(other, true, 12, source, NULL) ==
           VFS_ERR_STALE_GENERATION && replace_calls == 2);
    assert(vfs_file_replace(handle, true, 12, source, NULL) == VFS_OK);
    assert(node.size == 12 && second_node.size == 12 &&
           handle->save_generation == 9);
    assert(vfs_seek(other, 0, VFS_SEEK_END, &end) == VFS_OK && end == 12);
    assert(vfs_file_replace(handle, true, 0, NULL, NULL) == VFS_OK);
    assert(node.size == 0 && second_node.size == 0 &&
           handle->save_generation == 10);
    assert(vfs_seek(other, 0, VFS_SEEK_END, &end) == VFS_OK && end == 0);
    disk_facts.permissions = VFS_PERMISSION_OTHER_READ;
    assert(vfs_file_replace(handle, true, 4, source, NULL) ==
           VFS_ERR_ACCESS_DENIED && replace_calls == 4);
    assert(vfs_close(other) == VFS_OK);
    assert(vfs_close(handle) == VFS_OK && super.reference_count == 1);

    reset();
    assert(vfs_open_node(&node, VFS_OPEN_WRITE, &handle) == VFS_OK);
    fail_finish = true;
    assert(vfs_file_replace(handle, true, 5, source, NULL) ==
           VFS_ERR_UNCERTAIN);
    assert(pin_live && finish_calls == 1 && handle->save_generation == 7);
    assert(node.size == 5 && second_node.size == 5);
    assert(vfs_close(handle) == VFS_OK);
    pin_live = false; /* Fixture retirement of the simulated pin. */

    reset();
    assert(vfs_open_node(&node, VFS_OPEN_WRITE, &handle) == VFS_OK);
    assert(!pthread_mutex_lock(&operation_mutex));
    race_wait = true;
    pthread_t replacing, closing;
    assert(!pthread_create(&replacing, NULL, queued_replace, handle));
    assert(!pthread_mutex_lock(&race_mutex));
    while (!race_entered)
        assert(!pthread_cond_wait(&race_ready, &race_mutex));
    assert(!pthread_mutex_unlock(&race_mutex));
    assert(!pthread_create(&closing, NULL, queued_close, handle));
    __atomic_store_n(&device_live, false, __ATOMIC_RELEASE);
    assert(!pthread_mutex_unlock(&operation_mutex));
    assert(!pthread_join(replacing, NULL));
    assert(!pthread_join(closing, NULL));
    assert(!replace_calls && super.reference_count == 1);

    reset();
    assert(vfs_open_node(&node, VFS_OPEN_WRITE, &handle) == VFS_OK);
    remove_during_replace = true;
    assert(vfs_file_replace(handle, true, 5, source, NULL) ==
           VFS_ERR_DEVICE_GONE && replace_calls == 1 && !pin_live);
    assert(vfs_close(handle) == VFS_OK);

    reset(); node.ops = &unsupported_ops;
    assert(vfs_open_node(&node, VFS_OPEN_WRITE, &handle) == VFS_OK);
    assert(!handle->save_expectation_valid && facts_calls == 0);
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_UNSUPPORTED);
    assert(vfs_file_replace(handle, true, 1, source, NULL) ==
           VFS_ERR_UNSUPPORTED);
    assert(vfs_close(handle) == VFS_OK);
    reset(); node.type = VFS_TYPE_DIRECTORY;
    assert(vfs_open_node(&node, VFS_OPEN_WRITE, &handle) == VFS_OK);
    assert(!handle->save_expectation_valid);
    assert(vfs_file_save_check(handle, true, NULL) == VFS_ERR_UNSUPPORTED);
    assert(vfs_file_replace(handle, true, 1, source, NULL) ==
           VFS_ERR_UNSUPPORTED);
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

    /* A complete snapshot adopts the generation of the bytes it returned. */
    reset();
    disk_facts.size = 5;
    disk_facts.permissions = VFS_PERMISSION_OWNER_READ |
                             VFS_PERMISSION_OWNER_WRITE;
    node.permissions = disk_facts.permissions;
    assert(vfs_open_node(&node, VFS_OPEN_RDWR, &handle) == VFS_OK);
    handle->offset = 23;
    disk_facts.generation = 8; /* Another handle changed the file. */
    assert(vfs_file_replace(handle, true, 5, source, NULL) ==
           VFS_ERR_STALE_GENERATION);
    assert(vfs_file_snapshot(handle, snapshot_data, sizeof(snapshot_data),
                             &snapshot_state) == VFS_OK);
    assert(snapshot_state.identifier == 21 && snapshot_state.revision == 8 &&
           snapshot_state.length == 5 && snapshot_data[0] == 's');
    assert(handle->save_generation == 8 && handle->offset == 23);
    assert(vfs_file_replace(handle, true, 5, source, NULL) == VFS_OK);
    assert(handle->save_generation == 9 && handle->offset == 23);
    assert(vfs_file_snapshot(handle, snapshot_data, sizeof(snapshot_data),
                             &snapshot_state) == VFS_OK);
    assert(snapshot_state.revision == 9);
    assert(vfs_file_replace(handle, true, 5, source, NULL) == VFS_OK);
    assert(handle->save_generation == 10 && handle->offset == 23);
    snapshot_error = VFS_ERR_IO;
    assert(vfs_file_snapshot(handle, snapshot_data, sizeof(snapshot_data),
                             &snapshot_state) == VFS_ERR_IO);
    assert(handle->save_generation == 10);
    snapshot_error = 0;
    assert(vfs_file_snapshot(handle, snapshot_data, 1, &snapshot_state) ==
           VFS_ERR_BUFFER_TOO_SMALL && handle->save_generation == 10);
    snapshot_error = VFS_ERR_UNCERTAIN;
    assert(vfs_file_snapshot(handle, snapshot_data, sizeof(snapshot_data),
                             &snapshot_state) == VFS_ERR_UNCERTAIN);
    snapshot_error = 0;
    snapshot_bad_revision = true;
    assert(vfs_file_snapshot(handle, snapshot_data, sizeof(snapshot_data),
                             &snapshot_state) == VFS_ERR_BAD_FORMAT);
    snapshot_bad_revision = false;
    assert(handle->save_generation == 10);
    facts_error = VFS_ERR_NOT_FOUND; /* Unlinked record. */
    assert(vfs_file_snapshot(handle, snapshot_data, sizeof(snapshot_data),
                             &snapshot_state) == VFS_ERR_NOT_FOUND);
    facts_error = 0;
    disk_facts.record_id = 22; /* Reused or wrong record identity. */
    assert(vfs_file_snapshot(handle, snapshot_data, sizeof(snapshot_data),
                             &snapshot_state) == VFS_ERR_BAD_FORMAT);
    disk_facts.record_id = 21; /* Rename does not change this identity. */
    disk_facts.permissions = VFS_PERMISSION_OWNER_WRITE;
    assert(vfs_file_snapshot(handle, snapshot_data, sizeof(snapshot_data),
                             &snapshot_state) == VFS_ERR_ACCESS_DENIED);
    disk_facts.permissions |= VFS_PERMISSION_OWNER_READ;
    disk_facts.owner_uid = 42;
    assert(vfs_file_snapshot(handle, snapshot_data, sizeof(snapshot_data),
                             &snapshot_state) == VFS_ERR_ACCESS_DENIED);
    disk_facts.owner_uid = 41;
    assert(handle->save_generation == 10 && handle->offset == 23);
    assert(vfs_close(handle) == VFS_OK && super.reference_count == 1);

    /* An open that could not read facts can gain an expectation by snapshot. */
    reset();
    disk_facts.size = 5;
    disk_facts.permissions |= VFS_PERMISSION_OWNER_READ;
    node.permissions |= VFS_PERMISSION_OWNER_READ;
    facts_error = VFS_ERR_IO;
    assert(vfs_open_node(&node, VFS_OPEN_RDWR, &handle) == VFS_OK);
    assert(!handle->save_expectation_valid);
    facts_error = 0;
    assert(vfs_file_snapshot(handle, snapshot_data, sizeof(snapshot_data),
                             &snapshot_state) == VFS_OK);
    assert(handle->save_expectation_valid && handle->save_generation == 7);
    assert(vfs_close(handle) == VFS_OK && super.reference_count == 1);

    /* Snapshot and replacement serialize on the same open handle. */
    reset();
    disk_facts.size = 5; disk_facts.generation = 8;
    disk_facts.permissions |= VFS_PERMISSION_OWNER_READ;
    node.permissions |= VFS_PERMISSION_OWNER_READ;
    assert(vfs_open_node(&node, VFS_OPEN_RDWR, &handle) == VFS_OK);
    handle->save_generation = 7;
    snapshot_pause = true;
    pthread_t snapshot_thread, replacement_thread;
    assert(!pthread_create(&snapshot_thread, NULL, queued_snapshot, handle));
    assert(!pthread_mutex_lock(&race_mutex));
    while (!snapshot_entered)
        assert(!pthread_cond_wait(&snapshot_ready, &race_mutex));
    assert(!pthread_mutex_unlock(&race_mutex));
    assert(!pthread_create(&replacement_thread, NULL, replace_after_snapshot,
                           handle));
    assert(!pthread_mutex_lock(&race_mutex));
    snapshot_resume = true;
    assert(!pthread_cond_broadcast(&snapshot_ready));
    assert(!pthread_mutex_unlock(&race_mutex));
    assert(!pthread_join(snapshot_thread, NULL));
    assert(!pthread_join(replacement_thread, NULL));
    assert(handle->save_generation == 9 && replace_calls == 1 &&
           snapshot_calls == 1);
    assert(vfs_close(handle) == VFS_OK && super.reference_count == 1);

    reset();
    disk_facts.size = 5;
    disk_facts.permissions |= VFS_PERMISSION_OWNER_READ;
    node.permissions |= VFS_PERMISSION_OWNER_READ;
    assert(vfs_open_node(&node, VFS_OPEN_READ, &handle) == VFS_OK);
    assert(!pthread_mutex_lock(&operation_mutex));
    race_wait = true;
    pthread_t disappearing;
    assert(!pthread_create(&disappearing, NULL, queued_snapshot_dead, handle));
    assert(!pthread_mutex_lock(&race_mutex));
    while (!race_entered)
        assert(!pthread_cond_wait(&race_ready, &race_mutex));
    assert(!pthread_mutex_unlock(&race_mutex));
    device_live = false;
    assert(!pthread_mutex_unlock(&operation_mutex));
    assert(!pthread_join(disappearing, NULL));
    assert(handle->save_generation == 7 && !snapshot_calls);
    assert(vfs_close(handle) == VFS_OK && super.reference_count == 1);

    reset(); node.ops = &snapshot_only_ops;
    disk_facts.size = 5;
    assert(vfs_open_node(&node, VFS_OPEN_READ, &handle) == VFS_OK);
    assert(!handle->save_expectation_valid);
    assert(vfs_file_snapshot(handle, snapshot_data, sizeof(snapshot_data),
                             &snapshot_state) == VFS_OK);
    assert(!handle->save_expectation_valid);
    assert(vfs_close(handle) == VFS_OK);
    reset(); node.type = VFS_TYPE_DIRECTORY;
    assert(vfs_open_node(&node, VFS_OPEN_READ, &handle) == VFS_OK);
    assert(vfs_file_snapshot(handle, snapshot_data, sizeof(snapshot_data),
                             &snapshot_state) == VFS_ERR_UNSUPPORTED);
    assert(!handle->save_expectation_valid);
    assert(vfs_close(handle) == VFS_OK);

    /* Path metadata waits for a mutation's size update under operation_lock. */
    reset();
    for (int pass = 0; pass < 2; pass++) {
        assert(!pthread_mutex_lock(&operation_mutex));
        race_wait = true; race_entered = false;
        pthread_t metadata_thread;
        replacement_size = pass ? 2 : 8192;
        assert(!pthread_create(&metadata_thread, NULL, queued_metadata, &node));
        assert(!pthread_mutex_lock(&race_mutex));
        while (!race_entered)
            assert(!pthread_cond_wait(&race_ready, &race_mutex));
        assert(!pthread_mutex_unlock(&race_mutex));
        vfs_nodes_update_size(&super, node.inode, replacement_size);
        assert(!pthread_mutex_unlock(&operation_mutex));
        assert(!pthread_join(metadata_thread, NULL));
        assert(node.size == replacement_size &&
               second_node.size == replacement_size);
    }
    assert(super.reference_count == 1 && !destroy_count);
    device_live = false;
    vfs_node_metadata_t metadata;
    assert(vfs_node_metadata_snapshot(&node, &metadata) ==
           VFS_ERR_DEVICE_GONE && super.reference_count == 1);
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
