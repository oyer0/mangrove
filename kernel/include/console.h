/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

struct kernel_thread;

void console_init(void);
void console_input(char c);
/* Keyboard characters are routed to the raw key queue while an owned
 * alternate-screen input mode is active. */
void console_input_key(u32 key);
bool console_raw_input_active(void);
bool console_set_raw_input(bool enabled, struct kernel_thread *owner);
/* Reads one key using scheduler-backed indefinite, polling, or timed wait. */
i64 console_read_key(u32 timeout_ms);
u64 console_read_bytes(void *buffer, u64 length);
void console_cancel_waiter(struct kernel_thread *thread);
