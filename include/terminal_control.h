/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/* MGTC concatenates these self-delimiting records:
 * "MGTC" | version:u8 | type:u8 | payload length:u32-le | payload. */
#define MG_TERMINAL_CONTROL_VERSION 1U
#define MG_TERMINAL_CONTROL_HEADER_SIZE 10U
#define MG_TERMINAL_CONTROL_MAX_BATCH 65536U
#define MG_TERMINAL_CONTROL_MAX_TEXT \
    (MG_TERMINAL_CONTROL_MAX_BATCH - MG_TERMINAL_CONTROL_HEADER_SIZE)

typedef enum {
    MG_TERMINAL_CONTROL_TEXT = 1,
    MG_TERMINAL_CONTROL_FOREGROUND = 2,
    MG_TERMINAL_CONTROL_BACKGROUND = 3,
    MG_TERMINAL_CONTROL_STYLE = 4,
    MG_TERMINAL_CONTROL_RESET = 5,
    MG_TERMINAL_CONTROL_CURSOR_POSITION = 6,
    MG_TERMINAL_CONTROL_CURSOR_VISIBILITY = 7,
    MG_TERMINAL_CONTROL_CLEAR = 8,
} mg_terminal_control_record_type_t;

typedef enum {
    MG_TERMINAL_CONTROL_CLEAR_LINE = 1,
    MG_TERMINAL_CONTROL_CLEAR_TO_END = 2,
    MG_TERMINAL_CONTROL_CLEAR_SCREEN = 3,
} mg_terminal_control_clear_t;
