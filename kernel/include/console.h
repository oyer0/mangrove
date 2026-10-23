/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>
#include <terminal.h>

struct kernel_thread;

void console_init(void);
void console_input(char c);
/* Keyboard characters are routed to the raw key queue while an owned
 * alternate-screen input mode is active. */
void console_input_key(u32 key);
void console_input_key_event(u32 keycode, u32 modifiers, u32 codepoint);
void console_input_key_event_flags(u32 keycode, u32 modifiers,
                                   u32 codepoint, u32 flags);
/* Remove queued typematic events for a released physical key.  Release is
 * represented by the keyboard driver's state transition rather than by a
 * raw queue packet, so stale repeats must be discarded at that boundary. */
void console_discard_key_repeats(u32 keycode);
bool console_raw_input_active(void);
bool console_set_raw_input(bool enabled, struct kernel_thread *owner);
/* Normal-screen line editors receive cooked type-ahead before raw events. */
bool console_set_raw_input_with_cooked_bytes(bool enabled,
                                             struct kernel_thread *owner);
/* On normal-screen release, preserve queued unmodified text as cooked bytes. */
bool console_set_raw_input_preserving_text(bool enabled,
                                           struct kernel_thread *owner);
/* Reads one key using scheduler-backed indefinite, polling, or timed wait. */
i64 console_read_key(u32 timeout_ms);
i64 console_read_key_event(u32 timeout_ms, terminal_key_event_t *event);
u64 console_read_bytes(void *buffer, u64 length);
/* Native object reads need errors distinct from an empty input queue. Returns
 * a positive confirmed byte count or a negative MG_ERR_* status. */
i64 console_read_bytes_checked(void *buffer, u64 length);
void console_cancel_waiter(struct kernel_thread *thread);
