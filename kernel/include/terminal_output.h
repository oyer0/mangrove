/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/* Per-process incremental state for native ESC terminal output. The kernel
 * terminal lock serializes access to this structure. */
#define TERMINAL_ESCAPE_SEQUENCE_MAX 128U

#define TERMINAL_TEXT_ATTR_BOLD      (1U << 0)
#define TERMINAL_TEXT_ATTR_DIM       (1U << 1)
#define TERMINAL_TEXT_ATTR_UNDERLINE (1U << 2)
#define TERMINAL_TEXT_ATTR_ITALIC    (1U << 3)
#define TERMINAL_TEXT_ATTR_REVERSE   (1U << 4)

typedef struct {
    u8 pending[TERMINAL_ESCAPE_SEQUENCE_MAX];
    u16 pending_length;
    u8 foreground;
    u8 background;
    /* Preserved per process even where the active renderer has no visual
     * implementation for a requested text attribute. */
    u8 attributes;
    bool initialized;
} terminal_output_state_t;
