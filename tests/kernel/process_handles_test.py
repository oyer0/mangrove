#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production process handle-table implementation."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "kernel/src/process.c").read_text()


def function(name):
    for match in re.finditer(
            r"^(?:static )?[A-Za-z_][^\n;{}]*\b" + name + r"\s*\(",
            SOURCE, re.M):
        brace = SOURCE.index("{", match.end())
        if ";" in SOURCE[match.end():brace]:
            continue
        depth = 1
        end = brace + 1
        while depth:
            depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
            end += 1
        return SOURCE[match.start():end] + "\n"
    raise ValueError(name)


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef size_t usize;
typedef uint32_t process_handle_t;
#define PROCESS_HANDLE_SLOTS_INITIAL 16U
#define PROCESS_HANDLE_SLOTS_MAX 65535U
#define HANDLE_INDEX_MASK 0xffffU
typedef enum { OBJECT_TYPE_INVALID, OBJECT_TYPE_TEST } kernel_object_type_t;
enum { PROCESS_STATE_ACTIVE = 1, OBJECT_RIGHT_READ = 1, OBJECT_RIGHT_WRITE = 2 };
typedef struct { unsigned value; } spinlock_t;
typedef struct { unsigned value; } mutex_t;
typedef struct kernel_object {
    kernel_object_type_t type;
    u32 ref_count;
} kernel_object_t;
typedef struct process {
    int state;
    void *handle_table;
    usize handle_capacity;
    spinlock_t handle_lock;
    mutex_t handle_growth_mutex;
    u64 handle_revision;
    usize next_handle_slot;
} process_t;
typedef struct {
    kernel_object_t *object;
    u32 rights;
    uint16_t generation;
    bool active;
    bool reserved;
} process_handle_slot_t;
static long allocation_countdown = -1;
static unsigned allocations, frees, spin_depth;
static u64 spin_lock_irqsave(spinlock_t *lock)
{ assert(!lock->value); lock->value = 1; spin_depth++; return 0; }
static void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{ (void)flags; assert(lock->value && spin_depth); lock->value = 0; spin_depth--; }
static bool mutex_lock(mutex_t *mutex)
{ assert(!mutex->value); mutex->value = 1; return true; }
static bool mutex_unlock(mutex_t *mutex)
{ assert(mutex->value); mutex->value = 0; return true; }
static void *kmalloc(usize size)
{
    assert(spin_depth == 0);
    if (allocation_countdown == 0) { allocation_countdown = -1; return NULL; }
    if (allocation_countdown > 0) allocation_countdown--;
    void *memory = malloc(size);
    if (memory) allocations++;
    return memory;
}
static void kfree(void *memory) { if (memory) { frees++; free(memory); } }
static bool object_reference(kernel_object_t *object)
{ if (!object || !object->ref_count || object->ref_count == UINT32_MAX) return false;
  object->ref_count++; return true; }
static void object_release(kernel_object_t *object)
{ assert(object && object->ref_count); object->ref_count--; }
'''

NAMES = (
    "handle_slots", "process_handle_revision_advance", "process_handle_grow",
    "encode_handle", "decode_handle", "process_handle_install",
    "process_handle_reserve", "process_handle_activate",
    "process_handle_cancel", "process_handle_lookup_any",
    "process_handle_close", "process_handle_close_all",
)


def main():
    cc = sys.argv[1] if len(sys.argv) > 1 else "gcc"
    with tempfile.TemporaryDirectory(prefix="mangrove-process-handles-") as temp:
        folder = Path(temp)
        code = PREFIX + "\n".join(function(name) for name in NAMES)
        code += r'''
static void process_init_fixture(process_t *process)
{
    memset(process, 0, sizeof(*process));
    process->state = PROCESS_STATE_ACTIVE;
    process->handle_capacity = PROCESS_HANDLE_SLOTS_INITIAL;
    process->handle_table = kmalloc(process->handle_capacity *
                                    sizeof(process_handle_slot_t));
    assert(process->handle_table);
    memset(process->handle_table, 0, process->handle_capacity *
           sizeof(process_handle_slot_t));
}
int main(void)
{
    process_t process;
    kernel_object_t object = {OBJECT_TYPE_TEST, 1};
    process_handle_t handles[PROCESS_HANDLE_SLOTS_MAX];
    process_init_fixture(&process);

    /* Growth across several capacities preserves all active handles. */
    for (usize i = 0; i < 4097; i++) {
        assert(process_handle_install(&process, &object, OBJECT_RIGHT_READ,
                                      &handles[i]));
        assert(process_handle_lookup_any(&process, handles[i],
               OBJECT_TYPE_TEST, OBJECT_RIGHT_READ) == &object);
    }
    assert(process.handle_capacity >= 4097 && process.handle_capacity <
           PROCESS_HANDLE_SLOTS_MAX);
    for (usize i = 0; i < 4097; i++)
        assert(process_handle_lookup_any(&process, handles[i],
               OBJECT_TYPE_TEST, 0) == &object);

    /* A failed growth leaves all existing handles usable and does not retain
       the attempted object's reference. */
    process_handle_close_all(&process);
    assert(object.ref_count == 1);
    kfree(process.handle_table);
    memset(&process, 0, sizeof(process));
    process_init_fixture(&process);
    for (usize i = 0; i < PROCESS_HANDLE_SLOTS_INITIAL; i++)
        assert(process_handle_install(&process, &object, OBJECT_RIGHT_READ,
                                      &handles[i]));
    allocation_countdown = 0;
    assert(!process_handle_install(&process, &object, OBJECT_RIGHT_READ,
                                   &handles[PROCESS_HANDLE_SLOTS_INITIAL]));
    assert(object.ref_count == PROCESS_HANDLE_SLOTS_INITIAL + 1U);
    for (usize i = 0; i < PROCESS_HANDLE_SLOTS_INITIAL; i++)
        assert(process_handle_lookup_any(&process, handles[i],
               OBJECT_TYPE_TEST, OBJECT_RIGHT_READ) == &object);
    assert(process_handle_close(&process, handles[3]));
    assert(!process_handle_lookup_any(&process, handles[3],
           OBJECT_TYPE_TEST, 0));
    assert(process_handle_install(&process, &object, OBJECT_RIGHT_READ,
                                  &handles[16]));
    assert(handles[16] != handles[3]);
    assert(process_handle_lookup_any(&process, handles[16],
           OBJECT_TYPE_TEST, 0) == &object);

    /* Reserved handles remain invisible until activation and can be
       cancelled without leaking the transferred object reference. */
    object.ref_count++;
    process_handle_t reserved;
    assert(process_handle_reserve(&process, &object, OBJECT_RIGHT_WRITE,
                                  &reserved));
    assert(!process_handle_lookup_any(&process, reserved,
           OBJECT_TYPE_TEST, 0));
    assert(process_handle_activate(&process, reserved));
    assert(process_handle_lookup_any(&process, reserved,
           OBJECT_TYPE_TEST, OBJECT_RIGHT_WRITE) == &object);
    assert(process_handle_close(&process, reserved));
    object.ref_count++;
    assert(process_handle_reserve(&process, &object, OBJECT_RIGHT_READ,
                                  &reserved));
    assert(process_handle_cancel(&process, reserved));
    assert(!process_handle_cancel(&process, reserved));

    process_handle_close_all(&process);
    assert(object.ref_count == 1);
    assert(process.handle_capacity == 32);
    kfree(process.handle_table);

    /* The handle ABI encodes index+1 in 16 bits, so all 65,535 representable
       slots are usable and the next request fails without corrupting them. */
    process_init_fixture(&process);
    for (usize i = 0; i < PROCESS_HANDLE_SLOTS_MAX; i++)
        assert(process_handle_install(&process, &object, OBJECT_RIGHT_READ,
                                      &handles[i]));
    assert(process.handle_capacity == PROCESS_HANDLE_SLOTS_MAX);
    assert(!process_handle_install(&process, &object, OBJECT_RIGHT_READ,
                                   &handles[0]));
    assert(object.ref_count == PROCESS_HANDLE_SLOTS_MAX + 1U);
    assert(process_handle_lookup_any(&process, handles[PROCESS_HANDLE_SLOTS_MAX-1],
           OBJECT_TYPE_TEST, 0) == &object);
    process_handle_close_all(&process);
    assert(object.ref_count == 1);
    kfree(process.handle_table);
    assert(allocations == frees);
    puts("Process handles: dynamic growth, generation, reservation and exhaustion PASS");
    return 0;
}
'''
        fixture = folder / "fixture.c"
        fixture.write_text(code)
        binary = folder / "test"
        subprocess.run([cc, "-std=gnu17", "-O1", "-g", "-Wall", "-Wextra",
            "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            str(fixture), "-o", str(binary)], check=True, cwd=ROOT, timeout=60)
        subprocess.run([str(binary)], check=True, cwd=ROOT, timeout=60,
            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})


if __name__ == "__main__":
    main()
