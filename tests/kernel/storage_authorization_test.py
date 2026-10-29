#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Concurrent production process storage-authorization state transitions."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = sys.argv[1] if len(sys.argv) > 1 else "cc"


def function(name):
    source = (ROOT / "kernel/src/process.c").read_text()
    pattern = r"^(?:static )?[A-Za-z_][^\n;{}]*\b" + name + r"\s*\("
    for match in re.finditer(pattern, source, re.M):
        start = source.index("{", match.end())
        if ";" in source[match.end():start]:
            continue
        end, depth = start + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end] + "\n"
    raise AssertionError(f"missing production function: {name}")


def function_from(path, name):
    source = (ROOT / path).read_text()
    pattern = r"^(?:static )?[A-Za-z_][^\n;{}]*\b" + name + r"\s*\("
    for match in re.finditer(pattern, source, re.M):
        start = source.index("{", match.end())
        if ";" in source[match.end():start]:
            continue
        end, depth = start + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end] + "\n"
    raise AssertionError(f"missing production function: {path}:{name}")


prefix = r'''#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef int32_t i32;
typedef int64_t i64;
typedef pthread_mutex_t spinlock_t;
enum { PROCESS_STATE_ACTIVE, PROCESS_STATE_EXITING, PROCESS_STATE_TERMINATED };
enum { MG_OK=0, MG_ERR_BAD_ARGUMENT=-4, MG_ERR_BUSY=-14,
       MG_ERR_ACCESS_DENIED=-8, MG_ERR_PRIVILEGE_REQUIRED=-25 };
typedef struct process {
    int state;
    bool termination_pending;
    bool exit_requested;
    u32 storage_authorized_operation;
    u64 storage_authorized_thread_id;
    bool storage_management_session;
    bool storage_management_authorizing;
} process_t;
static spinlock_t process_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 spin_lock_irqsave(spinlock_t *lock) {
    assert(pthread_mutex_lock(lock) == 0); return 0;
}
static void spin_unlock_irqrestore(spinlock_t *lock, u64 flags) {
    assert(flags == 0); assert(pthread_mutex_unlock(lock) == 0);
}
'''

production = "\n".join(function(name) for name in (
    "process_storage_authorization_clear_locked",
    "process_storage_authorization_revoke_thread_locked",
    "process_storage_management_begin",
    "process_storage_management_finish",
    "process_storage_authorization_grant",
    "process_storage_authorization_consume",
    "process_storage_authorization_cancel",
))

for transition in ("process_exit", "process_abort", "process_terminate_external",
                   "process_publish_terminated_locked"):
    assert "process_storage_authorization_clear_locked(process)" in function_from(
        "kernel/src/process.c", transition), transition
for transition in ("process_thread_entry_completed", "process_worker_exit"):
    assert "process_storage_authorization_revoke_thread_locked" in function_from(
        "kernel/src/process.c", transition), transition

syscalls = (ROOT / "kernel/src/syscall.c").read_text()
assert "storage_authorized_operation" not in syscalls
assert "storage_authorized_thread_id" not in syscalls
session_start = syscalls.index("case SYSCALL_STORAGE_SESSION_AUTHORIZE: {")
session_end = syscalls.index("        case SYSCALL_STORAGE_FORMAT:", session_start)
session_body = syscalls[session_start:session_end]
assert session_body.index("process_storage_management_begin") < session_body.index(
    "pass_authorize_current") < session_body.index(
        "process_storage_management_finish")
for case, storage_call in (
    ("SYSCALL_STORAGE_FORMAT", "storage_format_filesystem_for_id"),
    ("SYSCALL_STORAGE_LABEL", "storage_set_filesystem_label_for_id"),
    ("SYSCALL_STORAGE_GPT_INITIALIZE", "gpt_initialize_device_for_id"),
    ("SYSCALL_STORAGE_GPT_CREATE", "gpt_create_partition_for_id"),
    ("SYSCALL_STORAGE_GPT_DELETE", "gpt_delete_partition_for_id"),
):
    start = syscalls.index("case " + case + ": {")
    end = syscalls.find("        case ", start + 8)
    body = syscalls[start:end if end >= 0 else len(syscalls)]
    assert "process_storage_authorization_consume" in body, case
    assert body.index("process_storage_authorization_consume") < body.index(storage_call), case
    if "syscall_user_buffer_valid" in body:
        assert body.index("process_storage_authorization_consume") < body.index(
            "syscall_user_buffer_valid"), case
    assert "storage_authorized_operation" not in body, case

test = r'''
enum { OP_FORMAT=1, OP_LABEL=2 };
static process_t shared_process;
typedef struct { pthread_barrier_t *start; u64 id; u32 operation; bool result; }
    consume_case_t;
static void *concurrent_consume(void *argument) {
    consume_case_t *item = argument;
    int barrier_result = pthread_barrier_wait(item->start);
    assert(barrier_result == 0 || barrier_result == PTHREAD_BARRIER_SERIAL_THREAD);
    item->result = process_storage_authorization_consume(
        &shared_process, item->id, item->operation);
    return NULL;
}

static void prepare_session(void) {
    memset(&shared_process, 0, sizeof(shared_process));
    shared_process.state = PROCESS_STATE_ACTIVE;
    assert(process_storage_management_begin(&shared_process));
    assert(!process_storage_management_begin(&shared_process));
    assert(process_storage_management_finish(&shared_process, true));
    assert(shared_process.storage_management_session);
}

int main(void) {
    prepare_session();
    assert(process_storage_authorization_grant(&shared_process, 101, OP_FORMAT) == MG_OK);
    assert(!process_storage_authorization_consume(&shared_process, 202, OP_FORMAT));
    assert(shared_process.storage_authorized_operation == OP_FORMAT);
    assert(!process_storage_authorization_consume(&shared_process, 101, OP_LABEL));
    assert(shared_process.storage_authorized_operation == OP_FORMAT);
    assert(process_storage_authorization_cancel(&shared_process, 202) ==
           MG_ERR_ACCESS_DENIED);
    assert(shared_process.storage_authorized_operation == OP_FORMAT);
    assert(process_storage_authorization_grant(&shared_process, 202, OP_LABEL) == MG_ERR_BUSY);
    assert(process_storage_authorization_consume(&shared_process, 101, OP_FORMAT));
    assert(!shared_process.storage_authorized_operation &&
           !shared_process.storage_authorized_thread_id);
    /* The operation is consumed on admission. A subsequent storage error
     * cannot accidentally restore the one-shot grant. */
    assert(!process_storage_authorization_consume(&shared_process, 101, OP_FORMAT));
    assert(process_storage_authorization_cancel(&shared_process, 101) == MG_OK);

    assert(process_storage_authorization_grant(&shared_process, 303, OP_LABEL) == MG_OK);
    assert(process_storage_authorization_cancel(&shared_process, 303) == MG_OK);
    assert(!shared_process.storage_authorized_operation);
    assert(process_storage_authorization_cancel(&shared_process, 303) == MG_OK);
    assert(process_storage_authorization_grant(&shared_process, 350, OP_LABEL) == MG_OK);
    process_storage_authorization_revoke_thread_locked(&shared_process, 351);
    assert(shared_process.storage_authorized_operation == OP_LABEL &&
           shared_process.storage_authorized_thread_id == 350);
    process_storage_authorization_revoke_thread_locked(&shared_process, 350);
    assert(!shared_process.storage_authorized_operation &&
           !shared_process.storage_authorized_thread_id);
    assert(process_storage_authorization_grant(&shared_process, 351, OP_LABEL) == MG_OK);
    process_storage_authorization_revoke_thread_locked(&shared_process, 351);
    assert(!shared_process.storage_authorized_operation &&
           !shared_process.storage_authorized_thread_id);
    shared_process.storage_management_session = false;
    assert(process_storage_authorization_cancel(&shared_process, 303) ==
           MG_ERR_PRIVILEGE_REQUIRED);

    for (unsigned same_owner = 0; same_owner < 2; same_owner++) {
        pthread_barrier_t barrier;
        pthread_t workers[2];
        consume_case_t cases[2];
        const u64 owner = 400U + same_owner;
        prepare_session();
        assert(process_storage_authorization_grant(&shared_process, owner, OP_FORMAT) == MG_OK);
        assert(pthread_barrier_init(&barrier, NULL, 3) == 0);
        for (unsigned i = 0; i < 2; i++) {
            cases[i] = (consume_case_t){
                .start=&barrier,
                .id=same_owner ? owner : (i ? owner + 1U : owner),
                .operation=OP_FORMAT,
                .result=false,
            };
            assert(pthread_create(&workers[i], NULL, concurrent_consume,
                                 &cases[i]) == 0);
        }
        int barrier_result = pthread_barrier_wait(&barrier);
        assert(barrier_result == 0 || barrier_result == PTHREAD_BARRIER_SERIAL_THREAD);
        for (unsigned i = 0; i < 2; i++) assert(pthread_join(workers[i], NULL) == 0);
        assert((unsigned)cases[0].result + (unsigned)cases[1].result == 1U);
        if (!same_owner) assert(cases[0].result && !cases[1].result);
        assert(!shared_process.storage_authorized_operation &&
               !shared_process.storage_authorized_thread_id);
        assert(pthread_barrier_destroy(&barrier) == 0);
    }

    prepare_session();
    assert(process_storage_authorization_grant(&shared_process, 505, OP_FORMAT) == MG_OK);
    shared_process.termination_pending = true;
    assert(!process_storage_authorization_consume(&shared_process, 505, OP_FORMAT));
    assert(process_storage_authorization_grant(&shared_process, 505, OP_LABEL) == MG_ERR_PRIVILEGE_REQUIRED);
    shared_process.termination_pending = false;
    assert(process_storage_authorization_consume(&shared_process, 505, OP_FORMAT));

    memset(&shared_process, 0, sizeof(shared_process));
    shared_process.state = PROCESS_STATE_ACTIVE;
    assert(process_storage_management_begin(&shared_process));
    assert(!process_storage_management_finish(&shared_process, false));
    assert(!shared_process.storage_management_session &&
           !shared_process.storage_management_authorizing);
    assert(process_storage_management_begin(&shared_process));
    assert(process_storage_management_finish(&shared_process, true));
    assert(shared_process.storage_management_session);
    assert(process_storage_authorization_grant(&shared_process, 606, OP_LABEL) == MG_OK);
    shared_process.state = PROCESS_STATE_EXITING;
    process_storage_authorization_clear_locked(&shared_process);
    assert(!shared_process.storage_management_session &&
           !shared_process.storage_management_authorizing &&
           !shared_process.storage_authorized_operation &&
           !shared_process.storage_authorized_thread_id);
    assert(!process_storage_authorization_consume(&shared_process, 606, OP_LABEL));

    memset(&shared_process, 0, sizeof(shared_process));
    shared_process.state = PROCESS_STATE_ACTIVE;
    assert(process_storage_management_begin(&shared_process));
    shared_process.state = PROCESS_STATE_EXITING;
    assert(!process_storage_management_finish(&shared_process, true));
    assert(!shared_process.storage_management_session &&
           !shared_process.storage_management_authorizing);

    puts("Process storage authorization ownership and concurrency: PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="mangrove-storage-auth-") as temporary:
    directory = Path(temporary)
    source = directory / "storage_authorization_test.c"
    binary = directory / "storage_authorization_test"
    source.write_text(prefix + production + test)
    subprocess.run([
        CC, "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-pthread",
        str(source), "-o", str(binary),
    ], check=True)
    environment = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}
    subprocess.run([str(binary)], check=True, env=environment, timeout=30)
