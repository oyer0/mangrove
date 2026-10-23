/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <types.h>

/* Optional navigation diagnostic. Records never allocate, print, or take an
 * input/terminal lock. Its leaf lock may follow any of those locks. */
enum {
    INPUT_TRACE_REPORT = 1, INPUT_TRACE_REPEAT, INPUT_TRACE_QUEUE,
    INPUT_TRACE_READ, INPUT_TRACE_CANCEL, INPUT_TRACE_PRESENT,
    INPUT_TRACE_BATCH, INPUT_TRACE_FLUSH_BEGIN, INPUT_TRACE_FLUSH_END,
    INPUT_TRACE_USB_IRQ, INPUT_TRACE_USB_WORK
};
#if INPUT_TRACE_DIAGNOSTICS
void input_trace_record(u32 kind, u32 key, u32 first, u32 second);
void input_trace_raw(bool enabled);
#else
#define input_trace_record(kind, key, first, second) ((void)0)
#define input_trace_raw(enabled) ((void)0)
#endif
