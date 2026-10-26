/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/types.h>

void mgtlstest_heap_trace_start(void);
void mgtlstest_heap_trace_stop(usize *out_peak_bytes,
                              usize *out_live_bytes);
uintptr_t mgtlstest_stack_mark(void);
usize mgtlstest_stack_peak_bytes(uintptr_t mark);
