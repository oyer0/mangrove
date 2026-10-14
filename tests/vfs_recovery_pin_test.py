#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute production VFS recovery pins, unmount preflight and teardown.

The host supplies allocation, locks, device liveness and filesystem callbacks.
It does not emulate MGFS transactions, real block retirement or kernel threads.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VFS = (ROOT / 'kernel/src/vfs.c').read_text()
CAP = re.search(r'^#define VFS_MAX_RECOVERY_PINS [0-9]+U$',
                (ROOT / 'kernel/include/vfs.h').read_text(), re.M)
assert CAP, 'missing production recovery-pin capacity'


def function(name):
    for match in re.finditer(r'^(?:static )?[^\n;{}]*\b' + name + r'\s*\(',
                             VFS, re.M):
        brace = VFS.find('{', match.end())
        if brace < 0 or ';' in VFS[match.end():brace]:
            continue
        depth, end = 1, brace + 1
        while depth:
            depth += (VFS[end] == '{') - (VFS[end] == '}')
            end += 1
        return VFS[match.start():end]
    raise AssertionError(f'missing production function: {name}')


names = (
    'vfs_retarget_nodes_locked', 'vfs_super_is_live_locked',
    'vfs_super_mark_dispose_locked', 'vfs_super_destroy',
    'vfs_super_dispose', 'vfs_super_retain', 'vfs_super_release',
    'vfs_device_range', 'vfs_device_already_mounted',
    'vfs_recovery_private_begin', 'vfs_recovery_private_release',
    'vfs_format_preflight',
    'vfs_recovery_pin_begin_internal', 'vfs_recovery_pin_begin',
    'vfs_recovery_pin_begin_handoff', 'vfs_recovery_pin_mark_unresolved',
    'vfs_recovery_pin_finish', 'vfs_recovery_pin_finish_handoff',
    'vfs_recovery_pins_snapshot',
    'vfs_mount_detach', 'vfs_unmount_preflight_super_locked',
    'vfs_mounted_super_for_device_locked', 'vfs_unmount_device_preflight',
    'vfs_unmount_devices_preflight', 'vfs_unmount_devices',
    'vfs_unmount_device', 'vfs_device_removed',
)

prefix = r'''
#define _XOPEN_SOURCE 700
#include <assert.h>
#include <errno.h>
#include "host_barrier.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64; typedef uint32_t u32;
#define VFS_MAX_MOUNTS 4U
__RECOVERY_CAP__
#define VFS_OK 0
#define VFS_ERR_INVALID_PARAM (-1)
#define VFS_ERR_NOT_FOUND (-2)
#define VFS_ERR_IO (-3)
#define VFS_ERR_NO_MEM (-4)
#define VFS_ERR_BUSY (-10)
#define VFS_ERR_ACCESS_DENIED (-8)
#define VFS_ERR_DEVICE_GONE (-11)
#define VFS_ERR_NO_SPACE (-12)
#define VFS_ERR_UNCERTAIN (-14)
#define VFS_ERR_BUFFER_TOO_SMALL (-15)
typedef struct { pthread_mutex_t native; } spinlock_t;
typedef struct { pthread_mutex_t native; } mutex_t;
#define BLOCK_DEVICE_PARTITION 4
 typedef struct { u64 id, sector_count; unsigned type; bool read_only; } block_device_t;
 static block_device_t backing = { .id=41, .sector_count=1000 };
 typedef struct { block_device_t *parent; u64 parent_id, first_lba, last_lba; } gpt_partition_info_t;
 static bool gpt_get_partition_info(block_device_t *dev, gpt_partition_info_t *out) { if (dev->id != 43) return false; *out=(gpt_partition_info_t){ &backing, 41, 100, 199 }; return true; }
 static bool block_device_snapshot(block_device_t *dev, block_device_t *out) { if (!dev) return false; *out=*dev; return true; }
 static bool block_device_is_live(block_device_t *dev);
typedef struct vfs_super vfs_super_t;
typedef struct vfs_node { vfs_super_t *super; struct vfs_node *next_in_super; } vfs_node_t;
typedef enum { VFS_SUPER_ACTIVE, VFS_SUPER_DETACHING, VFS_SUPER_DEAD } vfs_super_state_t;
typedef enum { VFS_RECOVERY_PIN_ACTIVE, VFS_RECOVERY_PIN_UNRESOLVED } vfs_recovery_pin_state_t;
typedef struct { u64 id, device_id; vfs_recovery_pin_state_t state; } vfs_recovery_pin_info_t;
typedef bool (*vfs_recovery_handoff_t)(vfs_super_t *, void *);
typedef struct { int (*unmount)(vfs_super_t *); int (*sync)(vfs_super_t *); } vfs_super_ops_t;
struct vfs_super {
    block_device_t *dev; vfs_node_t *root_node; void *private_data;
    const vfs_super_ops_t *ops; u64 device_id; u32 reference_count;
    vfs_super_state_t state; vfs_node_t *nodes; mutex_t operation_lock;
    bool dispose_pending, read_only, recovery_private;
};
typedef struct {
    vfs_super_t *sb; vfs_node_t *covered_node; vfs_super_t *covered_super;
    vfs_super_t *parent_super; u64 parent_inode; block_device_t *dev;
    u64 dev_id; char mount_point[32], name[32]; bool active;
} vfs_mount_t;
typedef struct vfs_recovery_pin {
    struct vfs_recovery_pin *next; vfs_super_t *sb; u64 id, device_id;
    u64 storage_id, first_lba, last_lba;
    vfs_recovery_pin_state_t state; bool finishing;
    vfs_recovery_handoff_t handoff; void *handoff_context;
} vfs_recovery_pin_t;
static vfs_mount_t mount_table[VFS_MAX_MOUNTS];
static vfs_super_t dead_super;
static spinlock_t vfs_metadata_lock = { PTHREAD_MUTEX_INITIALIZER };
static mutex_t vfs_mount_lock = { PTHREAD_MUTEX_INITIALIZER };
static vfs_recovery_pin_t *recovery_pins;
static u32 recovery_pin_count;
static u64 next_recovery_pin_id = 1;
static bool device_online = true, fail_allocation;
static bool pause_allocation, pause_finish_try, finish_try_entered;
static host_barrier_t admission_barrier;
static pthread_mutex_t finish_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t finish_ready = PTHREAD_COND_INITIALIZER;
static unsigned destroyed, allocated, released, sync_calls;
static void *kmalloc(size_t size) {
    if (fail_allocation) { fail_allocation = false; return NULL; }
    if (pause_allocation) {
        int result = host_barrier_wait(&admission_barrier);
        assert(result == 0 || result == HOST_BARRIER_SERIAL_THREAD);
    }
    allocated++; return malloc(size);
}
static void kfree(void *p) { if (p) { released++; free(p); } }
static u64 spin_lock_irqsave(spinlock_t *lock) {
    assert(!pthread_mutex_lock(&lock->native)); return 0;
}
static void spin_unlock_irqrestore(spinlock_t *lock, u64 flags) {
    (void)flags; assert(!pthread_mutex_unlock(&lock->native));
}
static void mutex_init(mutex_t *lock) { assert(!pthread_mutex_init(&lock->native, NULL)); }
static bool mutex_lock(mutex_t *lock) { return !pthread_mutex_lock(&lock->native); }
static bool mutex_try_lock(mutex_t *lock) {
    if (pause_finish_try) {
        assert(!pthread_mutex_lock(&finish_gate));
        finish_try_entered = true;
        assert(!pthread_cond_broadcast(&finish_ready));
        while (pause_finish_try)
            assert(!pthread_cond_wait(&finish_ready, &finish_gate));
        assert(!pthread_mutex_unlock(&finish_gate));
    }
    return !pthread_mutex_trylock(&lock->native);
}
static bool mutex_unlock(mutex_t *lock) { return !pthread_mutex_unlock(&lock->native); }
static bool block_device_id_is_live(u64 id) {
    return __atomic_load_n(&device_online, __ATOMIC_ACQUIRE) &&
           (id == 41 || id == 42 || id == 43);
}
static bool block_device_is_live(block_device_t *dev) { return dev && block_device_id_is_live(dev->id); }
static bool process_any_cwd_under_path(const char *path) { (void)path; return false; }
static int fake_unmount(vfs_super_t *sb) { (void)sb; destroyed++; return VFS_OK; }
static int sync_result;
static int fake_sync(vfs_super_t *sb) { assert(pthread_mutex_trylock(&sb->operation_lock.native) == EBUSY); sync_calls++; return sync_result; }
static const vfs_super_ops_t ops = { fake_unmount, fake_sync };
'''

suffix = r'''
static block_device_t devices[2] = { { .id=41, .sector_count=1000 }, { .id=42, .sector_count=1000 } };
static vfs_super_t *mounted(unsigned slot) {
    vfs_super_t *sb = calloc(1, sizeof(*sb)); assert(sb);
    assert(!pthread_mutex_init(&sb->operation_lock.native, NULL));
    sb->dev = &devices[slot]; sb->device_id = devices[slot].id;
    sb->ops = &ops; sb->state = VFS_SUPER_ACTIVE;
    mount_table[slot].active = true; mount_table[slot].sb = sb;
    mount_table[slot].dev = sb->dev; mount_table[slot].dev_id = sb->device_id;
    strcpy(mount_table[slot].mount_point, slot ? "/b" : "/a");
    device_online = true;
    return sb;
}
static void reset_mounts(void) { memset(mount_table, 0, sizeof(mount_table)); }
static void assert_snapshot(u64 id, u64 device_id, vfs_recovery_pin_state_t state) {
    vfs_recovery_pin_info_t info[2] = {0}; u32 count = 0;
    assert(vfs_recovery_pins_snapshot(NULL, 0, &count) == VFS_ERR_BUFFER_TOO_SMALL);
    assert(count == 1);
    assert(vfs_recovery_pins_snapshot(info, 2, &count) == VFS_OK);
    assert(count == 1 && info[0].id == id && info[0].device_id == device_id &&
           info[0].state == state);
}
static void no_pins(void) {
    u32 count = 99;
    assert(vfs_recovery_pins_snapshot(NULL, 0, &count) == VFS_OK && count == 0);
}
typedef struct { vfs_super_t *sb; u64 id; int result; } race_context_t;
static void *race_admit(void *argument) {
    race_context_t *ctx = argument;
    assert(mutex_lock(&ctx->sb->operation_lock));
    ctx->result = vfs_recovery_pin_begin(ctx->sb, &ctx->id);
    assert(mutex_unlock(&ctx->sb->operation_lock));
    return NULL;
}
static void *race_remove(void *argument) {
    (void)argument;
    __atomic_store_n(&device_online, false, __ATOMIC_RELEASE);
    vfs_device_removed(&devices[0]);
    return NULL;
}
typedef struct { u64 id; int result; } finish_context_t;
static void *race_finish(void *argument) {
    finish_context_t *ctx = argument;
    ctx->result = vfs_recovery_pin_finish(ctx->id);
    return NULL;
}
typedef struct { bool attached, fail, remove_device; unsigned calls; u64 id; } handoff_state_t;
static bool handoff_callback(vfs_super_t *sb, void *context) {
    handoff_state_t *state = context;
    vfs_recovery_pin_info_t entry = {0}; u32 count = 0;
    assert(pthread_mutex_trylock(&sb->operation_lock.native) == EBUSY);
    assert(vfs_recovery_pins_snapshot(&entry, 1, &count) == VFS_OK);
    assert(count == 1 && entry.id == state->id &&
           entry.state == VFS_RECOVERY_PIN_ACTIVE);
    assert(vfs_recovery_pin_finish_handoff(state->id) == VFS_ERR_BUSY);
    assert(vfs_recovery_pin_mark_unresolved(state->id) == VFS_ERR_BUSY);
    state->calls++;
    if (state->remove_device) {
        __atomic_store_n(&device_online, false, __ATOMIC_RELEASE);
        vfs_device_removed(&devices[0]);
        assert(sb->state == VFS_SUPER_DEAD && !mount_table[0].active);
    }
    if (state->fail) return false;
    assert(state->attached);
    state->attached = false;
    return true;
}
/* Exercise cyclic reuse and exactly one serial return per generation on
 * the Darwin implementation as well as Linux's native pthread barrier. */
typedef struct { unsigned serial; } barrier_worker_t;
static host_barrier_t reusable_barrier;
static void *barrier_worker(void *context) {
    barrier_worker_t *worker = context;
    for (unsigned i = 0; i < 1000; i++) {
        int result = host_barrier_wait(&reusable_barrier);
        assert(result == 0 || result == HOST_BARRIER_SERIAL_THREAD);
        worker->serial += result == HOST_BARRIER_SERIAL_THREAD;
    }
    return NULL;
}
static void barrier_tests(void) {
    assert(host_barrier_init(&reusable_barrier, NULL, 0) == EINVAL);
    assert(!host_barrier_init(&reusable_barrier, NULL, 1));
    assert(host_barrier_wait(&reusable_barrier) == HOST_BARRIER_SERIAL_THREAD);
    assert(!host_barrier_destroy(&reusable_barrier));
    assert(!host_barrier_init(&reusable_barrier, NULL, 2));
    barrier_worker_t first = {0}, second = {0};
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, barrier_worker, &first));
    barrier_worker(&second);
    assert(!pthread_join(thread, NULL));
    assert(first.serial + second.serial == 1000);
    assert(!host_barrier_destroy(&reusable_barrier));
}
int main(void) {
    barrier_tests();
    vfs_super_t *sb; u64 id = 0;
    reset_mounts(); sb = mounted(0);
    assert(vfs_recovery_pin_begin(NULL, &id) == VFS_ERR_INVALID_PARAM);
    assert(vfs_recovery_pin_finish(0) == VFS_ERR_INVALID_PARAM);
    assert(vfs_recovery_pin_mark_unresolved(0) == VFS_ERR_INVALID_PARAM);
    assert(vfs_recovery_pin_finish(999) == VFS_ERR_NOT_FOUND);
    sb->read_only = true; id = 123U;
    unsigned before_allocated = allocated;
    assert(vfs_recovery_pin_begin(sb, &id) == VFS_ERR_ACCESS_DENIED && !id);
    assert(allocated == before_allocated && !sb->reference_count); no_pins();
    sb->read_only = false;
    fail_allocation = true;
    assert(vfs_recovery_pin_begin(sb, &id) == VFS_ERR_NO_MEM && id == 0);
    assert(sb->reference_count == 0); no_pins();
    assert(vfs_recovery_pin_begin(sb, &id) == VFS_OK && id != 0);
    assert(sb->reference_count == 1);
    assert_snapshot(id, 41, VFS_RECOVERY_PIN_ACTIVE);
    assert(vfs_unmount_device_preflight(&devices[0]) == VFS_ERR_BUSY);
    vfs_super_t *other = mounted(1);
    block_device_t *batch[] = { &devices[0], &devices[1] };
    assert(vfs_unmount_devices_preflight(batch, 2) == VFS_ERR_BUSY);
    assert(vfs_unmount_device(&devices[0]) == VFS_ERR_BUSY);
    assert(vfs_unmount_devices(batch, 2) == VFS_ERR_BUSY);
    assert(sync_calls == 0 && destroyed == 0 && other->state == VFS_SUPER_ACTIVE);
    assert(mutex_lock(&sb->operation_lock));
    assert(vfs_recovery_pin_finish(id) == VFS_ERR_BUSY);
    assert(mutex_unlock(&sb->operation_lock));
    assert_snapshot(id, 41, VFS_RECOVERY_PIN_ACTIVE);
    pthread_t first_finish, second_finish;
    finish_context_t a = { id, 0 }, b = { id, 0 };
    assert(!pthread_create(&first_finish, NULL, race_finish, &a));
    assert(!pthread_create(&second_finish, NULL, race_finish, &b));
    assert(!pthread_join(first_finish, NULL));
    assert(!pthread_join(second_finish, NULL));
    assert((a.result == VFS_OK) != (b.result == VFS_OK));
    assert(a.result == VFS_OK || a.result == VFS_ERR_BUSY ||
           a.result == VFS_ERR_NOT_FOUND);
    assert(b.result == VFS_OK || b.result == VFS_ERR_BUSY ||
           b.result == VFS_ERR_NOT_FOUND);
    assert(vfs_recovery_pin_finish(id) == VFS_ERR_NOT_FOUND);
    no_pins(); assert(sb->reference_count == 0);
    for (unsigned failure = 0; failure < 2U; failure++) {
        sync_result = failure ? VFS_ERR_UNCERTAIN : VFS_ERR_IO;
        unsigned calls = sync_calls;
        assert(vfs_unmount_devices(batch, 2) == sync_result);
        assert(sync_calls == calls + 1U && !destroyed &&
            sb->state == VFS_SUPER_ACTIVE && other->state == VFS_SUPER_ACTIVE &&
            !sb->reference_count && !other->reference_count &&
            mount_table[0].active && mount_table[1].active);
        assert(mutex_try_lock(&sb->operation_lock) && mutex_unlock(&sb->operation_lock));
    }
    sync_result = VFS_OK;
    assert(vfs_unmount_devices(batch, 2) == VFS_OK);
    assert(destroyed == 2 && sync_calls == 4);
    reset_mounts();

    /* An open file reference and ACTIVE pin survive forced namespace loss. */
    sb = mounted(0); sb->reference_count = 1;
    assert(vfs_recovery_pin_begin(sb, &id) == VFS_OK);
    assert(sb->reference_count == 2);
    device_online = false;
    vfs_device_removed(&devices[0]);
    assert(!mount_table[0].active && sb->state == VFS_SUPER_DEAD);
    vfs_super_release(sb); /* Final file-handle close. */
    assert(destroyed == 2 && sb->reference_count == 1);
    assert_snapshot(id, 41, VFS_RECOVERY_PIN_ACTIVE);
    assert(vfs_recovery_pin_finish(id) == VFS_OK);
    assert(destroyed == 3 && vfs_recovery_pin_finish(id) == VFS_ERR_NOT_FOUND);
    no_pins(); reset_mounts();

    sb = mounted(0); sb->reference_count = 1;
    assert(vfs_recovery_pin_begin(sb, &id) == VFS_OK);
    assert(mutex_lock(&sb->operation_lock));
    assert(!pthread_mutex_lock(&finish_gate)); pause_finish_try = true;
    assert(!pthread_mutex_unlock(&finish_gate));
    finish_context_t overlapping = { id, 0 };
    pthread_t overlapping_finish;
    assert(!pthread_create(&overlapping_finish, NULL, race_finish, &overlapping));
    assert(!pthread_mutex_lock(&finish_gate));
    while (!finish_try_entered)
        assert(!pthread_cond_wait(&finish_ready, &finish_gate));
    assert(!pthread_mutex_unlock(&finish_gate));
    assert(vfs_recovery_pin_mark_unresolved(id) == VFS_ERR_BUSY);
    assert(!pthread_mutex_lock(&finish_gate)); pause_finish_try = false;
    assert(!pthread_cond_broadcast(&finish_ready));
    assert(!pthread_mutex_unlock(&finish_gate));
    assert(!pthread_join(overlapping_finish, NULL));
    assert(overlapping.result == VFS_ERR_BUSY);
    assert(vfs_recovery_pin_mark_unresolved(id) == VFS_OK);
    assert(mutex_unlock(&sb->operation_lock));
    assert(vfs_recovery_pin_mark_unresolved(id) == VFS_ERR_BUSY);
    assert(vfs_recovery_pin_finish(id) == VFS_ERR_UNCERTAIN);
    device_online = false; vfs_device_removed(&devices[0]);
    vfs_super_release(sb);
    assert(destroyed == 3);
    assert_snapshot(id, 41, VFS_RECOVERY_PIN_UNRESOLVED);
    /* Fixture-only cleanup; production intentionally has no retirement API. */
    assert(recovery_pins && recovery_pins->id == id);
    vfs_recovery_pin_t *pin = recovery_pins;
    recovery_pins = NULL;
    recovery_pin_count--;
    vfs_super_release(pin->sb); kfree(pin);
    assert(destroyed == 4); no_pins(); reset_mounts();

    /* Admission and forced detachment race under the production metadata lock.
     * A caller reference keeps the candidate superblock alive in both orders. */
    for (unsigned iteration = 0; iteration < 100; iteration++) {
        pthread_t admit_thread, remove_thread;
        race_context_t ctx = {0};
        sb = mounted(0); sb->reference_count = 1; ctx.sb = sb;
        assert(!pthread_create(&admit_thread, NULL, race_admit, &ctx));
        assert(!pthread_create(&remove_thread, NULL, race_remove, NULL));
        assert(!pthread_join(admit_thread, NULL));
        assert(!pthread_join(remove_thread, NULL));
        assert(sb->state == VFS_SUPER_DEAD && !mount_table[0].active);
        if (ctx.result == VFS_OK) {
            assert(ctx.id != 0 && sb->reference_count == 2);
            assert_snapshot(ctx.id, 41, VFS_RECOVERY_PIN_ACTIVE);
            vfs_super_release(sb);
            assert(vfs_recovery_pin_finish(ctx.id) == VFS_OK);
        } else {
            assert(ctx.result == VFS_ERR_DEVICE_GONE && ctx.id == 0);
            no_pins(); vfs_super_release(sb);
        }
        reset_mounts(); no_pins();
    }

    /* The cap counts ACTIVE and UNRESOLVED pins. Full admission must not
     * allocate, retain, or consume an ID; a resolved finish reopens one slot. */
    sb = mounted(0); sb->reference_count = 1;
    u64 ids[VFS_MAX_RECOVERY_PINS];
    for (u32 i = 0; i < VFS_MAX_RECOVERY_PINS; i++)
        assert(vfs_recovery_pin_begin(sb, &ids[i]) == VFS_OK);
    assert(recovery_pin_count == VFS_MAX_RECOVERY_PINS);
    assert(sb->reference_count == VFS_MAX_RECOVERY_PINS + 1);
    assert(vfs_recovery_pin_mark_unresolved(ids[0]) == VFS_OK);
    vfs_recovery_pin_info_t entries[VFS_MAX_RECOVERY_PINS] = {0};
    u32 count = 0;
    assert(vfs_recovery_pins_snapshot(entries, VFS_MAX_RECOVERY_PINS - 1,
                                      &count) == VFS_ERR_BUFFER_TOO_SMALL);
    assert(count == VFS_MAX_RECOVERY_PINS);
    assert(vfs_recovery_pins_snapshot(entries, VFS_MAX_RECOVERY_PINS,
                                      &count) == VFS_OK && count == VFS_MAX_RECOVERY_PINS);
    unsigned unresolved_seen = 0;
    for (u32 i = 0; i < count; i++) {
        assert(entries[i].id && entries[i].device_id == 41);
        unresolved_seen += entries[i].state == VFS_RECOVERY_PIN_UNRESOLVED;
        for (u32 j = 0; j < i; j++) assert(entries[i].id != entries[j].id);
    }
    assert(unresolved_seen == 1);
    unsigned before_allocation = allocated;
    u64 rejected = 99;
    assert(vfs_recovery_pin_begin(sb, &rejected) == VFS_ERR_NO_SPACE);
    assert(rejected == 0 && allocated == before_allocation);
    assert(recovery_pin_count == VFS_MAX_RECOVERY_PINS &&
           sb->reference_count == VFS_MAX_RECOVERY_PINS + 1);
    assert(vfs_recovery_pin_finish(ids[0]) == VFS_ERR_UNCERTAIN);
    assert(vfs_recovery_pin_finish(ids[1]) == VFS_OK);
    assert(recovery_pin_count == VFS_MAX_RECOVERY_PINS - 1);
    assert(vfs_recovery_pin_begin(sb, &rejected) == VFS_OK);
    assert(rejected > ids[VFS_MAX_RECOVERY_PINS - 1]);
    assert(recovery_pin_count == VFS_MAX_RECOVERY_PINS);
    for (u32 i = 2; i < VFS_MAX_RECOVERY_PINS; i++)
        assert(vfs_recovery_pin_finish(ids[i]) == VFS_OK);
    assert(vfs_recovery_pin_finish(rejected) == VFS_OK);
    assert(recovery_pin_count == 1 && sb->reference_count == 2);
    __atomic_store_n(&device_online, false, __ATOMIC_RELEASE);
    vfs_device_removed(&devices[0]); vfs_super_release(sb);
    assert_snapshot(ids[0], 41, VFS_RECOVERY_PIN_UNRESOLVED);
    pin = recovery_pins; recovery_pins = NULL; recovery_pin_count--;
    vfs_super_release(pin->sb); kfree(pin);
    no_pins(); reset_mounts();

    /* Both contenders pass the pre-allocation check at count 31. The second
     * metadata-locked check admits exactly one and frees the rejected node. */
    sb = mounted(0); vfs_super_t *other_boundary = mounted(1);
    for (u32 i = 0; i < VFS_MAX_RECOVERY_PINS - 1; i++)
        assert(vfs_recovery_pin_begin(sb, &ids[i]) == VFS_OK);
    assert(!host_barrier_init(&admission_barrier, NULL, 2));
    pause_allocation = true;
    race_context_t left = { .sb = sb }, right = { .sb = other_boundary };
    pthread_t left_thread, right_thread;
    assert(!pthread_create(&left_thread, NULL, race_admit, &left));
    assert(!pthread_create(&right_thread, NULL, race_admit, &right));
    assert(!pthread_join(left_thread, NULL));
    assert(!pthread_join(right_thread, NULL));
    pause_allocation = false;
    assert(!host_barrier_destroy(&admission_barrier));
    assert((left.result == VFS_OK) != (right.result == VFS_OK));
    assert(left.result == VFS_OK || left.result == VFS_ERR_NO_SPACE);
    assert(right.result == VFS_OK || right.result == VFS_ERR_NO_SPACE);
    assert(recovery_pin_count == VFS_MAX_RECOVERY_PINS);
    assert(sb->reference_count + other_boundary->reference_count ==
           VFS_MAX_RECOVERY_PINS);
    for (u32 i = 0; i < VFS_MAX_RECOVERY_PINS - 1; i++)
        assert(vfs_recovery_pin_finish(ids[i]) == VFS_OK);
    assert(vfs_recovery_pin_finish(left.result == VFS_OK ? left.id : right.id) == VFS_OK);
    no_pins();
    block_device_t *boundary_batch[] = { &devices[0], &devices[1] };
    assert(vfs_unmount_devices(boundary_batch, 2) == VFS_OK);
    reset_mounts();

    sb = mounted(0); sb->state = VFS_SUPER_DETACHING;
    assert(vfs_recovery_pin_begin(sb, &id) == VFS_ERR_DEVICE_GONE);
    sb->state = VFS_SUPER_DEAD;
    assert(vfs_recovery_pin_begin(sb, &id) == VFS_ERR_DEVICE_GONE);
    vfs_super_dispose(sb); assert(destroyed == 108); reset_mounts();

    /* The production handoff finish keeps its pin discoverable through the
     * callback, and ordinary finish cannot bypass that callback. */
    sb = mounted(0); handoff_state_t handoff = { .attached = true };
    assert(vfs_recovery_pin_begin_handoff(sb, handoff_callback,
                                          &handoff, &handoff.id) == VFS_OK);
    assert(vfs_recovery_pin_finish(handoff.id) == VFS_ERR_BUSY);
    assert(vfs_recovery_pin_finish_handoff(handoff.id) == VFS_OK);
    assert(handoff.calls == 1 && !handoff.attached && sb->reference_count == 0);
    no_pins(); assert(vfs_unmount_device(&devices[0]) == VFS_OK);
    reset_mounts();

    /* A callback failure leaves the transaction attached and the pin
     * UNRESOLVED. Forced removal and the last ordinary reference cannot
     * destroy or hide that state. */
    sb = mounted(0); sb->reference_count = 1;
    handoff = (handoff_state_t){ .attached = true, .fail = true };
    assert(vfs_recovery_pin_begin_handoff(sb, handoff_callback,
                                          &handoff, &handoff.id) == VFS_OK);
    assert(vfs_recovery_pin_finish_handoff(handoff.id) == VFS_ERR_UNCERTAIN);
    assert(handoff.calls == 1 && handoff.attached && sb->reference_count == 2);
    assert_snapshot(handoff.id, 41, VFS_RECOVERY_PIN_UNRESOLVED);
    device_online = false; vfs_device_removed(&devices[0]);
    vfs_super_release(sb); assert(sb->reference_count == 1);
    assert_snapshot(handoff.id, 41, VFS_RECOVERY_PIN_UNRESOLVED);
    assert(vfs_recovery_pin_finish_handoff(handoff.id) == VFS_ERR_UNCERTAIN);
    pin = recovery_pins; recovery_pins = NULL; recovery_pin_count--;
    vfs_super_release(pin->sb); kfree(pin); reset_mounts(); no_pins();

    /* Failed lock admission invokes no callback and retains an ACTIVE pin.
     * The callback can later complete even if removal occurs inside it. */
    sb = mounted(0);
    handoff = (handoff_state_t){ .attached = true, .remove_device = true };
    assert(vfs_recovery_pin_begin_handoff(sb, handoff_callback,
                                          &handoff, &handoff.id) == VFS_OK);
    assert(mutex_lock(&sb->operation_lock));
    assert(vfs_recovery_pin_finish_handoff(handoff.id) == VFS_ERR_BUSY);
    assert(mutex_unlock(&sb->operation_lock));
    assert(handoff.calls == 0 && handoff.attached);
    assert_snapshot(handoff.id, 41, VFS_RECOVERY_PIN_ACTIVE);
    unsigned before_destroy = destroyed;
    assert(vfs_recovery_pin_finish_handoff(handoff.id) == VFS_OK);
    assert(handoff.calls == 1 && !handoff.attached && destroyed == before_destroy + 1);
    no_pins(); reset_mounts();

    reset_mounts(); sb = mounted(0); sb->read_only = true;
    before_destroy = destroyed;
    /* Forced detach clears the mount slot, but retained policy and lifetime
     * remain authoritative until the last reference is released. */
    assert(vfs_super_retain(sb));
    device_online = false;
    vfs_device_removed(&devices[0]);
    assert(!mount_table[0].active && sb->state == VFS_SUPER_DEAD && sb->read_only);
    id = 123U;
    assert(vfs_recovery_pin_begin(sb, &id) == VFS_ERR_ACCESS_DENIED && !id);
    vfs_super_release(sb);
    assert(destroyed == before_destroy + 1U); reset_mounts();
    unsigned ordinary_destroyed = destroyed;
    assert(allocated + destroyed == released); /* sb objects use calloc. */
    device_online = true;
    handoff = (handoff_state_t){ .attached = true };
    assert(vfs_recovery_private_begin(&devices[0], devices[0].id, &ops, handoff_callback,
        &handoff, &sb, &handoff.id) == VFS_OK && sb->reference_count == 2);
    assert(vfs_format_preflight(&devices[0]) == VFS_ERR_BUSY);
    assert(vfs_device_already_mounted(&devices[0]));
    block_device_t child = { .id=43, .sector_count=100, .type=BLOCK_DEVICE_PARTITION };
    assert(vfs_format_preflight(&child) == VFS_ERR_BUSY);
    assert(!vfs_device_already_mounted(&devices[1]));
    vfs_super_t *rejected_sb = NULL; u64 rejected_pin = 0;
    assert(vfs_recovery_private_begin(&devices[0], devices[0].id, &ops, handoff_callback,
        &handoff, &rejected_sb, &rejected_pin) == VFS_ERR_BUSY && !rejected_sb && !rejected_pin);
    assert(vfs_recovery_pin_finish_handoff(handoff.id) == VFS_OK);
    assert(sb->reference_count == 1 && !vfs_device_already_mounted(&devices[0]));
    vfs_recovery_private_release(sb);
    assert(destroyed == ordinary_destroyed + 1);
    no_pins();
    handoff = (handoff_state_t){ .attached = true };
    assert(vfs_recovery_private_begin(&devices[0], devices[0].id, &ops, handoff_callback,
        &handoff, &sb, &handoff.id) == VFS_OK);
    assert(vfs_recovery_pin_mark_unresolved(handoff.id) == VFS_OK);
    vfs_recovery_private_release(sb);
    assert(sb->state == VFS_SUPER_DETACHING && sb->reference_count == 1);
    assert_snapshot(handoff.id, 41, VFS_RECOVERY_PIN_UNRESOLVED);
    assert(vfs_format_preflight(&devices[0]) == VFS_ERR_BUSY);
    device_online = false;
    assert(vfs_format_preflight(&devices[0]) == VFS_ERR_DEVICE_GONE);
    device_online = true;
    assert(vfs_recovery_pin_finish_handoff(handoff.id) == VFS_ERR_UNCERTAIN);
    /* Test-only teardown of intentionally unresolved registry state. */
    pin = recovery_pins; recovery_pins = NULL; recovery_pin_count = 0;
    kfree(pin); vfs_super_release(sb);
    assert(allocated + ordinary_destroyed == released);
    puts("production VFS recovery pins: admission, bounded capacity, concurrent transitions, unmount and teardown passed");
}
'''

source_text = prefix.replace('__RECOVERY_CAP__', CAP.group(0)) + \
    '\n\n'.join(function(name) for name in names) + suffix
with tempfile.TemporaryDirectory(prefix='mangrove-recovery-pin-') as directory:
    source = Path(directory) / 'recovery_pin.c'
    executable = source.with_suffix('')
    source.write_text(source_text)
    subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'cc',
                    '-I' + str(ROOT / 'tests'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', str(source), '-o',
                    str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
