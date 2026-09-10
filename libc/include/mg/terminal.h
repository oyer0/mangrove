/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/types.h>
#include <terminal_colors.h>

/* Versioned native terminal-control ABI.  These structures contain no
 * userspace pointers; the syscall copies them through separately validated
 * buffers. */
#define MG_TERMINAL_API_VERSION 1U
#define MG_TERMINAL_WAIT_FOREVER 0xFFFFFFFFU

typedef enum {
    MG_TERMINAL_OP_ALTERNATE_ENTER = 1,
    MG_TERMINAL_OP_ALTERNATE_LEAVE = 2,
    MG_TERMINAL_OP_CURSOR_MOVE = 3,
    MG_TERMINAL_OP_CURSOR_VISIBILITY = 4,
    MG_TERMINAL_OP_CLEAR = 5,
    MG_TERMINAL_OP_SIZE = 6,
    MG_TERMINAL_OP_READ_KEY = 7,
    MG_TERMINAL_OP_UPDATE_BEGIN = 8,
    MG_TERMINAL_OP_UPDATE_END = 9,
    MG_TERMINAL_OP_INPUT_MODE = 10,
    MG_TERMINAL_OP_STYLED_WRITE = 11,
    MG_TERMINAL_OP_SEMANTIC_WRITE = 12,
    MG_TERMINAL_OP_CAPABILITIES = 13,
} mg_terminal_operation_t;

#define MG_TERMINAL_CAP_STYLED_OUTPUT TERMINAL_CAP_STYLED_OUTPUT

typedef terminal_color_t mg_terminal_color_t;
typedef terminal_style_role_t mg_terminal_style_role_t;
typedef terminal_style_t mg_terminal_style_t;

#define MG_TERMINAL_COLOR_BLACK          TERMINAL_COLOR_BLACK
#define MG_TERMINAL_COLOR_RED            TERMINAL_COLOR_RED
#define MG_TERMINAL_COLOR_GREEN          TERMINAL_COLOR_GREEN
#define MG_TERMINAL_COLOR_YELLOW         TERMINAL_COLOR_YELLOW
#define MG_TERMINAL_COLOR_BLUE           TERMINAL_COLOR_BLUE
#define MG_TERMINAL_COLOR_MAGENTA        TERMINAL_COLOR_MAGENTA
#define MG_TERMINAL_COLOR_CYAN           TERMINAL_COLOR_CYAN
#define MG_TERMINAL_COLOR_LIGHT_GRAY     TERMINAL_COLOR_LIGHT_GRAY
#define MG_TERMINAL_COLOR_DARK_GRAY      TERMINAL_COLOR_DARK_GRAY
#define MG_TERMINAL_COLOR_BRIGHT_RED     TERMINAL_COLOR_BRIGHT_RED
#define MG_TERMINAL_COLOR_BRIGHT_GREEN   TERMINAL_COLOR_BRIGHT_GREEN
#define MG_TERMINAL_COLOR_BRIGHT_YELLOW  TERMINAL_COLOR_BRIGHT_YELLOW
#define MG_TERMINAL_COLOR_BRIGHT_BLUE    TERMINAL_COLOR_BRIGHT_BLUE
#define MG_TERMINAL_COLOR_BRIGHT_MAGENTA TERMINAL_COLOR_BRIGHT_MAGENTA
#define MG_TERMINAL_COLOR_BRIGHT_CYAN    TERMINAL_COLOR_BRIGHT_CYAN
#define MG_TERMINAL_COLOR_WHITE          TERMINAL_COLOR_WHITE
#define MG_TERMINAL_COLOR_COUNT          TERMINAL_COLOR_COUNT

#define MG_TERMINAL_STYLE_DEFAULT        TERMINAL_STYLE_DEFAULT
#define MG_TERMINAL_STYLE_DIRECTORY      TERMINAL_STYLE_DIRECTORY
#define MG_TERMINAL_STYLE_ERROR          TERMINAL_STYLE_ERROR
#define MG_TERMINAL_STYLE_WARNING        TERMINAL_STYLE_WARNING
#define MG_TERMINAL_STYLE_SUCCESS        TERMINAL_STYLE_SUCCESS
#define MG_TERMINAL_STYLE_HEADING        TERMINAL_STYLE_HEADING
#define MG_TERMINAL_STYLE_COUNT          TERMINAL_STYLE_COUNT

static inline mg_terminal_style_t mg_terminal_style_for_role(
    mg_terminal_style_role_t role)
{
    return terminal_style_for_role((terminal_style_role_t)role);
}

typedef enum {
    MG_TERMINAL_CLEAR_LINE = 1,
    MG_TERMINAL_CLEAR_TO_END = 2,
    MG_TERMINAL_CLEAR_REGION = 3,
    MG_TERMINAL_CLEAR_SCREEN = 4,
} mg_terminal_clear_mode_t;

typedef struct PACKED {
    u32 version;
    u32 row;
    u32 column;
} mg_terminal_cursor_request_t;

typedef struct PACKED {
    u32 version;
    u32 visible;
} mg_terminal_visibility_request_t;

typedef struct PACKED {
    u32 version;
    u32 mode;
    u32 first_row;
    u32 first_column;
    u32 last_row;
    u32 last_column;
} mg_terminal_clear_request_t;

typedef struct PACKED {
    u32 version;
    u32 rows;
    u32 columns;
} mg_terminal_size_t;

typedef struct PACKED {
    u32 version;
    u32 capabilities;
} mg_terminal_capabilities_t;

typedef struct PACKED {
    u32 version;
    u32 timeout_ms;
} mg_terminal_key_request_t;

typedef struct PACKED {
    u32 version;
    u32 key;
} mg_terminal_key_result_t;

typedef struct PACKED {
    u32 version;
    u32 foreground;
    u32 background;
    u32 reserved;
    u64 length;
} mg_terminal_styled_write_request_t;

typedef struct PACKED {
    u32 version;
    u32 style;
    u32 reserved;
    u64 length;
} mg_terminal_semantic_write_request_t;

/* Native terminal operations.  Full-screen state operations are available
 * to the process that owns the active alternate screen. */
mg_result_t terminal_alternate_enter(void);
mg_result_t terminal_alternate_leave(void);
mg_result_t terminal_cursor_move(u32 row, u32 column);
mg_result_t terminal_cursor_set_visible(bool visible);
mg_result_t terminal_clear_line(void);
mg_result_t terminal_clear_to_end(void);
mg_result_t terminal_clear_region(u32 first_row, u32 first_column,
                                  u32 last_row, u32 last_column);
mg_result_t terminal_clear_screen(void);
mg_result_t terminal_get_size(mg_terminal_size_t *out_size);
mg_result_t terminal_get_capabilities(
    mg_terminal_capabilities_t *out_capabilities);
mg_result_t terminal_read_key(u32 timeout_ms, u32 *out_key);
mg_result_t terminal_input_raw(bool enabled);
mg_result_t terminal_update_begin(void);
mg_result_t terminal_update_end(void);

/* The supplied attributes apply only to this complete write.  The buffer is
 * serialized with terminal mutation, so another writer cannot change its
 * colors halfway through the operation. */
mg_result_t terminal_write_styled(const void *buffer, usize length,
                                  mg_terminal_color_t foreground,
                                  mg_terminal_color_t background);

/* The fixed console palette resolves the semantic role while the complete
 * write is serialized with terminal mutation. */
mg_result_t terminal_write_semantic(const void *buffer, usize length,
                                    mg_terminal_style_role_t role);
