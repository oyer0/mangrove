/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/* Per-process incremental state for native ESC terminal output. The kernel
 * terminal lock serializes access to this structure. */
#define TERMINAL_ESCAPE_SEQUENCE_MAX 128U

typedef struct {
    u8 pending[TERMINAL_ESCAPE_SEQUENCE_MAX];
    u16 pending_length;
    u8 foreground;
    u8 background;
    bool initialized;
} terminal_output_state_t;
