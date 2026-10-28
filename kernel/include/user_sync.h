/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>
#include <mg/types.h>
#include <mangrove_errors.h>

struct process;
struct kernel_thread;

#define USER_SYNC_WAIT_FOREVER (~(u64)0)

void user_sync_init(void);
mg_result_t user_sync_wait_u32(struct process *process,
                               struct kernel_thread *thread,
                               volatile u32 *word, u32 expected,
                               u64 timeout_ms);
mg_result_t user_sync_wake_u32(struct process *process,
                               volatile u32 *word, u32 max_waiters);
