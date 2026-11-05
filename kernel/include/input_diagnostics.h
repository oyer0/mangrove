/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <types.h>

/* Opt-in live input counters. Producers never allocate, render or acquire a
 * diagnostic lock. The display may take approximate atomic snapshots while
 * an input handler is stalled. This is a private diagnostic layout. */
typedef struct {
    u64 version;
    u64 irq1, irq12, eoi1, eoi12;
    u64 kbd_bytes, aux_bytes, decoded, errors, protocol, budget;
    u64 raw_put, raw_drop, raw_merge, raw_cancel, raw_get, raw_depth, raw_on;
    u64 shoot_get;
    u64 last_status, last_kbd, last_aux;
    u64 cfg_before, cfg_set, gsi1, gsi12, flags1, flags12, apic_id;
} input_diagnostics_t;

#define INPUT_DIAGNOSTIC_ROWS 4U
#define INPUT_DIAGNOSTIC_COLUMNS 124U
#if PS2_INPUT_DIAGNOSTICS
extern input_diagnostics_t pith_input_diagnostics;
#define INPUT_DIAG_ADD(field, count) \
    ((void)__atomic_fetch_add(&pith_input_diagnostics.field, (u64)(count), __ATOMIC_RELAXED))
#define INPUT_DIAG_INC(field) INPUT_DIAG_ADD(field, 1)
#define INPUT_DIAG_SET(field, value) \
    __atomic_store_n(&pith_input_diagnostics.field, (u64)(value), __ATOMIC_RELAXED)
void input_diagnostics_lines(char lines[INPUT_DIAGNOSTIC_ROWS][INPUT_DIAGNOSTIC_COLUMNS]);
#else
#define INPUT_DIAG_ADD(field, count) ((void)0)
#define INPUT_DIAG_INC(field) ((void)0)
#define INPUT_DIAG_SET(field, value) ((void)0)
#endif
