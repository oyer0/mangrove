/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/types.h>
#include <mg/error.h>

typedef mg_handle_t mg_thread_t;
typedef u64 mg_thread_id_t;
typedef u64 mg_thread_result_t;
typedef mg_thread_result_t (*mg_thread_entry_t)(void *argument);

typedef struct {
    mg_thread_t handle;
    mg_thread_id_t id;
} mg_thread_created_t;

/* Create an independent Ring 3 worker in the calling process. The entry must
 * point into executable memory owned by this process. The argument is passed
 * unchanged and is not copied; keep the pointed-to object valid until the
 * worker has finished using it. Workers are joinable by default, use a 32 KiB
 * stack plus one unmapped guard page, and receive a separate static TLS
 * instance. out_thread receives a generation-tagged handle and an
 * identity-only thread ID. Failure leaves no runnable worker. */
mg_result_t mg_thread_create(mg_thread_created_t *out_thread,
                             mg_thread_entry_t entry, void *argument);
/* Join waits without a timeout, then consumes the join entitlement and handle
 * and returns the worker's 64-bit result. Exactly one join can succeed.
 * Joining self is MG_ERR_BAD_ARGUMENT; an already claimed/detached worker is
 * MG_ERR_BUSY. Waits can also return MG_ERR_INTERRUPTED or MG_ERR_SHUTDOWN. */
mg_result_t mg_thread_join(mg_thread_t thread,
                           mg_thread_result_t *out_result);
/* Detach consumes the join entitlement and arranges automatic reaping.
 * Ordinary handle_close() refuses a joinable worker handle. */
mg_result_t mg_thread_detach(mg_thread_t thread);
/* Finish only a worker and return its result to a future join. Calling from the
 * initial process thread returns MG_ERR_BAD_ARGUMENT; process_exit() remains
 * process-wide. A valid worker call does not return. */
mg_result_t mg_thread_exit(mg_thread_result_t result);
/* Identity only; IDs are not handles or authority. */
mg_result_t mg_thread_self(mg_thread_id_t *out_id);
