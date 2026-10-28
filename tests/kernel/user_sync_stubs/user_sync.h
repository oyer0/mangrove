/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "common.h"
mg_result_t user_sync_wait_u32(struct process *process,
                               struct kernel_thread *thread,
                               volatile u32 *word, u32 expected,
                               u64 timeout_ms);
mg_result_t user_sync_wake_u32(struct process *process,
                               volatile u32 *word, u32 max_waiters);
void user_sync_init(void);
