#pragma once

#include <mg/error.h>
#include <mg/types.h>

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
} mg_terminal_operation_t;

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
    u32 timeout_ms;
} mg_terminal_key_request_t;

typedef struct PACKED {
    u32 version;
    u32 key;
} mg_terminal_key_result_t;

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
mg_result_t terminal_read_key(u32 timeout_ms, u32 *out_key);
mg_result_t terminal_input_raw(bool enabled);
mg_result_t terminal_update_begin(void);
mg_result_t terminal_update_end(void);
