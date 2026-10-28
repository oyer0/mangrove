#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host regressions for production process handles and VFS mount admission."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def function(path, name):
    source = (ROOT / path).read_text()
    matches = re.finditer(r'^(?:static )?[^\n;{}]*\b' + name + r'\s*\(', source, re.M)
    match = None
    for candidate in matches:
        line_start = source.rfind('\n', 0, candidate.start()) + 1
        name_start = source.index(name, candidate.start(), candidate.end())
        prefix = source[line_start:name_start].strip()
        if (prefix.startswith(('if ', 'if(', 'return ', 'while ', 'for ', '!'))
                or '=' in prefix):
            continue
        brace = source.find('{', candidate.end())
        if brace >= 0 and ';' not in source[candidate.end():brace]:
            match = candidate
            break
    if not match:
        raise AssertionError(f"missing production function {name}")
    brace = source.index('{', match.end())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


# Compile the production process handle table algorithms and production object
# reference operations. Only kernel allocation and spinlock primitives are
# controlled by the host fixture.
process_parts = [
    function('kernel/src/process.c', name)
    for name in ('handle_slots', 'encode_handle', 'decode_handle',
                 'process_handle_install', 'process_handle_lookup_reference',
                 'process_handle_lookup', 'process_handle_close',
                 'process_handle_close_all')
]
object_parts = [
    function('kernel/src/object.c', name)
    for name in ('object_init', 'object_reference', 'object_release')
]

process_fixture = r'''#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
typedef uint64_t u64; typedef uint32_t u32; typedef uint16_t u16;
typedef unsigned long usize;
typedef u32 process_handle_t;
typedef enum { PROCESS_STATE_ACTIVE, PROCESS_STATE_TERMINATED } process_state_t;
typedef enum { OBJECT_TYPE_INVALID = 0, OBJECT_TYPE_FILE = 3,
               OBJECT_TYPE_DIRECTORY = 4 } kernel_object_type_t;
typedef struct kernel_object kernel_object_t;
typedef struct process process_t;
typedef struct { pthread_mutex_t native; } spinlock_t;
typedef struct { pthread_mutex_t native; } mutex_t;
typedef struct {
    kernel_object_t *object; u32 rights; u16 generation; bool active, reserved;
} process_handle_slot_t;
struct kernel_object {
    kernel_object_type_t type; u32 ref_count;
    void (*destroy)(kernel_object_t *);
    void *read; void *write;
};
struct process {
    process_state_t state; void *handle_table; usize handle_capacity;
    u64 handle_revision; usize next_handle_slot;
    spinlock_t handle_lock; mutex_t handle_growth_mutex;
};
#define OBJECT_RIGHT_READ (1U << 0)
#define OBJECT_RIGHT_WRITE (1U << 1)
#define PROCESS_HANDLE_SLOTS 16U
#define HANDLE_INDEX_MASK 0xffffU
bool object_reference(kernel_object_t *object);
void object_release(kernel_object_t *object);
static void process_handle_revision_advance(process_t *process) {
    process->handle_revision++;
    if (!process->handle_revision) process->handle_revision = 1U;
}
/* This lifetime fixture leaves free slots available; dynamic growth and its
 * allocation-failure path are exercised by process_handles_test.py. */
static bool process_handle_grow(process_t *process) {
    (void)process; return false;
}
static void kfree(void *pointer) { free(pointer); }
static u64 spin_lock_irqsave(spinlock_t *lock) {
    assert(pthread_mutex_lock(&lock->native) == 0); return 0;
}
static void spin_unlock_irqrestore(spinlock_t *lock, u64 flags) {
    (void)flags; assert(pthread_mutex_unlock(&lock->native) == 0);
}
static bool mutex_lock(mutex_t *mutex) {
    return pthread_mutex_lock(&mutex->native) == 0;
}
static void mutex_unlock(mutex_t *mutex) {
    assert(pthread_mutex_unlock(&mutex->native) == 0);
}
static unsigned destroyed;
static void tracked_destroy(kernel_object_t *object) {
    destroyed++; free(object);
}
'''+ '\n'.join(process_parts + object_parts) + r'''
static void *close_thread(void *argument) {
    process_handle_t handle = *(process_handle_t *)argument;
    extern process_t test_process;
    assert(process_handle_close(&test_process, handle));
    return NULL;
}
process_t test_process;
static kernel_object_t *new_object(void) {
    kernel_object_t *object = calloc(1, sizeof(*object));
    assert(object); object_init(object, OBJECT_TYPE_FILE, tracked_destroy);
    return object;
}
int main(void) {
    process_handle_slot_t *slots = calloc(PROCESS_HANDLE_SLOTS,
                                          sizeof(*slots));
    assert(slots);
    pthread_t closer;
    process_handle_t first, second;
    kernel_object_t *file = new_object(), *retained;
    test_process.state = PROCESS_STATE_ACTIVE;
    test_process.handle_table = slots;
    test_process.handle_capacity = PROCESS_HANDLE_SLOTS;
    assert(pthread_mutex_init(&test_process.handle_lock.native, NULL) == 0);
    assert(pthread_mutex_init(&test_process.handle_growth_mutex.native, NULL) == 0);
    assert(process_handle_install(&test_process, file,
                                  OBJECT_RIGHT_READ | OBJECT_RIGHT_WRITE,
                                  &first));
    object_release(file); /* Drop the creator's reference, as open does. */
    assert((first & 0xffffU) == 1U && (first >> 16) == 1U);
    retained = process_handle_lookup(&test_process, first,
                                            OBJECT_TYPE_FILE,
                                            OBJECT_RIGHT_READ);
    assert(retained && retained->ref_count == 2U);
    assert(!process_handle_lookup(&test_process, first,
                                        OBJECT_TYPE_DIRECTORY, 0));
    assert(!process_handle_lookup(&test_process, first,
                                        OBJECT_TYPE_FILE,
                                        OBJECT_RIGHT_READ | (1U << 8)));
    assert(!process_handle_lookup(&test_process, 0,
                                        OBJECT_TYPE_INVALID, 0));
    assert(!process_handle_close(&test_process, first ^ 0x10000U));

    /* The operation's retained reference survives close on another thread. */
    assert(pthread_create(&closer, NULL, close_thread, &first) == 0);
    assert(pthread_join(closer, NULL) == 0);
    assert(destroyed == 0U && retained->type == OBJECT_TYPE_FILE &&
           retained->ref_count == 1U);
    assert(!process_handle_lookup(&test_process, first,
                                        OBJECT_TYPE_FILE, 0));
    assert(!process_handle_close(&test_process, first));
    object_release(retained);
    assert(destroyed == 1U);

    /* Reuse the freed slot; the old generation must not name the new object. */
    file = new_object();
    assert(process_handle_install(&test_process, file, OBJECT_RIGHT_READ,
                                  &second));
    object_release(file);
    assert((second & 0xffffU) == (first & 0xffffU));
    assert((second >> 16) != (first >> 16));
    assert(!process_handle_lookup(&test_process, first,
                                        OBJECT_TYPE_FILE, 0));
    kernel_object_t *second_pin = process_handle_lookup(
        &test_process, second, OBJECT_TYPE_FILE, OBJECT_RIGHT_READ);
    assert(second_pin);
    process_handle_close_all(&test_process);
    assert(second_pin->ref_count == 1U && destroyed == 1U);
    assert(!process_handle_close(&test_process, second));
    test_process.handle_table = NULL;
    free(slots); /* Model process teardown while the operation still owns a pin. */
    assert(!process_handle_lookup(&test_process, second,
                                        OBJECT_TYPE_FILE, 0));
    object_release(second_pin);
    assert(destroyed == 2U);
    assert(!process_handle_lookup(&test_process, second,
                                        OBJECT_TYPE_FILE, 0));
    test_process.state = PROCESS_STATE_TERMINATED;
    assert(!process_handle_lookup(&test_process, second,
                                        OBJECT_TYPE_FILE, 0));
    file = new_object();
    assert(!process_handle_install(&test_process, file,
                                   OBJECT_RIGHT_READ, &second));
    object_release(file);
    pthread_mutex_destroy(&test_process.handle_lock.native);
    pthread_mutex_destroy(&test_process.handle_growth_mutex.native);
    puts("production process handle lifetime: passed");
}
'''

with tempfile.TemporaryDirectory(prefix='mangrove-process-handles-') as directory:
    source = Path(directory) / 'process_handles.c'
    executable = source.with_suffix('')
    source.write_text(process_fixture)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', str(source), '-o',
                    str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=5)


# The production mount functions depend on much of the kernel VFS. Execute
# their smallest common admission boundary (the exact production identity
# helper) with concurrent attempts; inspect each real route to ensure the
# mount mutex encloses that check through the driver mount/publication call.
vfs_helper = function('kernel/src/vfs.c', 'vfs_device_range') + function('kernel/src/vfs.c', 'vfs_device_already_mounted')
identity_helper = function('kernel/src/vfs.c',
                           'vfs_mount_device_identity_live')
for route in ('vfs_mount_root', 'vfs_mount_node', 'vfs_mount_path'):
    body = function('kernel/src/vfs.c', route)
    lock = body.index('mutex_lock(&vfs_mount_lock)')
    check = body.index('vfs_device_already_mounted')
    mount_call = body.index('fs_type->mount' if route == 'vfs_mount_root'
                            else 'vfs_mount_instance')
    assert lock < check < mount_call
    assert body.rindex('mutex_unlock(&vfs_mount_lock)') > mount_call
for route in ('vfs_mount_root', 'vfs_mount_instance'):
    body = function('kernel/src/vfs.c', route)
    assert body.index('device_id = dev ? dev->id') < body.index('fs_type->mount')
    assert body.index('vfs_mount_device_identity_live') > body.index('fs_type->mount')
    assert 'sb->device_id = device_id' in body
    assert 'dev_id = device_id' in body

mount_fixture = r'''#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t u64; typedef unsigned u32;
#define BLOCK_DEVICE_PARTITION 4
 typedef struct { u64 id, sector_count; unsigned type; } block_device_t;
 typedef struct { block_device_t *parent; u64 parent_id, first_lba, last_lba; } gpt_partition_info_t;
 static bool gpt_get_partition_info(block_device_t *dev, gpt_partition_info_t *out) { (void)dev; (void)out; return false; }
 static bool block_device_snapshot(block_device_t *dev, block_device_t *out) { if (!dev) return false; *out=*dev; return true; }
typedef struct { bool active; u64 dev_id; block_device_t *dev; } vfs_mount_t;
 typedef struct vfs_recovery_pin { struct vfs_recovery_pin *next; u64 storage_id, first_lba, last_lba; } vfs_recovery_pin_t;
 static vfs_recovery_pin_t *recovery_pins;
 static struct {bool active;u64 device_id,storage_id,first_lba,last_lba;} pending_mount;
typedef struct { pthread_mutex_t native; } spinlock_t;
#define VFS_MAX_MOUNTS 4
static vfs_mount_t mount_table[VFS_MAX_MOUNTS];
static spinlock_t vfs_metadata_lock = { PTHREAD_MUTEX_INITIALIZER };
static pthread_mutex_t vfs_mount_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 live_registry_id = 77;
static bool block_device_id_is_live(u64 id) {
    return id == live_registry_id;
}
static u64 spin_lock_irqsave(spinlock_t *lock) {
    assert(pthread_mutex_lock(&lock->native) == 0); return 0;
}
static void spin_unlock_irqrestore(spinlock_t *lock, u64 flags) {
    (void)flags; assert(pthread_mutex_unlock(&lock->native) == 0);
}
'''+vfs_helper+identity_helper+r'''
static unsigned admitted;
static void *try_admit(void *argument) {
    block_device_t *device = argument;
    pthread_mutex_lock(&vfs_mount_lock);
    if (!vfs_device_already_mounted(device)) {
        mount_table[admitted].active = true;
        mount_table[admitted++].dev_id = device->id;
    }
    pthread_mutex_unlock(&vfs_mount_lock);
    return NULL;
}
int main(void) {
    pthread_t first, second;
    block_device_t a = { .id = 77, .sector_count = 1000 }, b = { .id = 77, .sector_count = 1000 }, c = { .id = 78, .sector_count = 1000 };
    assert(!vfs_device_already_mounted(&a));
    pending_mount.active=true;pending_mount.storage_id=77;pending_mount.first_lba=0;pending_mount.last_lba=999;
    assert(vfs_device_already_mounted(&a));pending_mount.active=false;
    assert(vfs_mount_device_identity_live(&a, 77));
    assert(vfs_mount_device_identity_live(NULL, 0));
    assert(!vfs_mount_device_identity_live(NULL, 77));
    a.id = 78; /* Slot reused after the filesystem observed ID 77. */
    live_registry_id = 78;
    assert(!vfs_mount_device_identity_live(&a, 77));
    a.id = 77;
    assert(!vfs_mount_device_identity_live(&a, 77));
    live_registry_id = 77;
    assert(pthread_create(&first, NULL, try_admit, &a) == 0);
    assert(pthread_create(&second, NULL, try_admit, &b) == 0);
    pthread_join(first, NULL); pthread_join(second, NULL);
    assert(admitted == 1U && vfs_device_already_mounted(&b));
    assert(vfs_device_already_mounted(&c)); /* Dead device cannot be admitted. */
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-vfs-admission-') as directory:
    source = Path(directory) / 'mount_admission.c'
    executable = source.with_suffix('')
    source.write_text(mount_fixture)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                    str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=5)
print('production VFS admission boundary: passed')
