/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#ifdef HEAP_PROFILE_DIAGNOSTICS
#include <types.h>

void heap_profile_init(void);
bool heap_profile_request_enter(void);
void heap_profile_request_leave_locked(void);
void heap_profile_failed_locked(usize requested, u32 cpu_index, u32 context,
                                u32 caller_text_offset);
void heap_profile_allocated_locked(void *pointer, usize requested,
                                   u32 cpu_index, u32 context,
                                   u32 caller_text_offset);
void heap_profile_freed_locked(void *pointer, u32 cpu_index, u32 context,
                               u32 caller_text_offset);
bool heap_profile_set_phase_locked(u32 phase);
void heap_profile_freeze_locked(void);
bool heap_profile_copy_frozen_range(void *destination, usize offset,
                                    usize length);
#endif
