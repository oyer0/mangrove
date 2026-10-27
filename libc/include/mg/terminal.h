/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/types.h>
#include <terminal_colors.h>

/* Versioned native terminal input, geometry, and presentation ABI. Ordinary
 * output controls use the canonical ESC{...} stream through stdout; the
 * libc helpers below encode that stream rather than issuing a second set of
 * output-control syscalls. */
#define MG_TERMINAL_API_VERSION 1U
#define MG_TERMINAL_WAIT_FOREVER 0xFFFFFFFFU

typedef enum {
    /* Retired output-control operation slots remain reserved to preserve the
     * version-1 operation numbering. */
    MG_TERMINAL_OP_RESERVED_ALTERNATE_ENTER = 1,
    MG_TERMINAL_OP_RESERVED_ALTERNATE_LEAVE = 2,
    MG_TERMINAL_OP_RESERVED_CURSOR_MOVE = 3,
    MG_TERMINAL_OP_RESERVED_CURSOR_VISIBILITY = 4,
    MG_TERMINAL_OP_RESERVED_CLEAR = 5,
    MG_TERMINAL_OP_SIZE = 6,
    MG_TERMINAL_OP_READ_KEY = 7,
    MG_TERMINAL_OP_UPDATE_BEGIN = 8,
    MG_TERMINAL_OP_UPDATE_END = 9,
    MG_TERMINAL_OP_INPUT_MODE = 10,
    MG_TERMINAL_OP_RESERVED_STYLED_WRITE = 11,
    MG_TERMINAL_OP_RESERVED_SEMANTIC_WRITE = 12,
    MG_TERMINAL_OP_CAPABILITIES = 13,
    MG_TERMINAL_OP_OVERLAY_SET = 14,
    MG_TERMINAL_OP_OVERLAY_CLEAR = 15,
    MG_TERMINAL_OP_READ_KEY_EVENT = 16,
    MG_TERMINAL_OP_PREPARE_INPUT_LINE = 17,
    MG_TERMINAL_OP_INPUT_CURSOR = 18,
    MG_TERMINAL_OP_SESSION_HISTORY_RESET = 19,
} mg_terminal_operation_t;

#define MG_TERMINAL_CAP_STYLED_OUTPUT TERMINAL_CAP_STYLED_OUTPUT
#define MG_TERMINAL_CAP_INTERACTIVE_OUTPUT TERMINAL_CAP_INTERACTIVE_OUTPUT

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

typedef struct PACKED {
    u32 version;
    u32 enabled;
} mg_terminal_input_mode_request_t;

typedef struct PACKED {
    u32 version;
    u32 rows;
    u32 columns;
} mg_terminal_size_t;

typedef struct PACKED {
    u32 version;
    u32 capabilities;
} mg_terminal_capabilities_t;

/* A temporary cell grid rendered below the current cursor.  It is visual
 * terminal state only: it is not appended to the terminal ring buffer or
 * scrollback history. */
typedef struct PACKED {
    u32 codepoint;
    u8 foreground;
    u8 background;
    u16 reserved;
} mg_terminal_overlay_cell_t;

typedef struct PACKED {
    u32 version;
    u32 rows;
    u32 columns;
    u32 reserved;
    u64 cell_count;
} mg_terminal_overlay_request_t;

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
    u32 keycode;
    u32 modifiers;
    u32 codepoint;
    u32 flags;
} mg_terminal_key_event_t;

#define MG_TERMINAL_KEY_EVENT_REPEAT (1U << 0)
/* A byte that was queued before a process claimed key-event input. */
#define MG_TERMINAL_KEY_EVENT_COOKED_BYTE (1U << 1)

/* USB HID usage identities carried by the versioned key-event interface. */
#define MG_TERMINAL_KEY_ENTER     0x28U
#define MG_TERMINAL_KEY_ESCAPE    0x29U
#define MG_TERMINAL_KEY_BACKSPACE 0x2aU
#define MG_TERMINAL_KEY_TAB       0x2bU
#define MG_TERMINAL_KEY_DELETE    0x4cU
#define MG_TERMINAL_KEY_HOME      0x4aU
#define MG_TERMINAL_KEY_END       0x4dU
#define MG_TERMINAL_KEY_PAGE_UP   0x4bU
#define MG_TERMINAL_KEY_PAGE_DOWN 0x4eU
#define MG_TERMINAL_KEY_RIGHT     0x4fU
#define MG_TERMINAL_KEY_LEFT      0x50U
#define MG_TERMINAL_KEY_DOWN      0x51U
#define MG_TERMINAL_KEY_UP        0x52U

#define MG_TERMINAL_MODIFIER_LEFT_CTRL   (1U << 0)
#define MG_TERMINAL_MODIFIER_LEFT_SHIFT  (1U << 1)
#define MG_TERMINAL_MODIFIER_LEFT_ALT    (1U << 2)
#define MG_TERMINAL_MODIFIER_LEFT_GUI    (1U << 3)
#define MG_TERMINAL_MODIFIER_RIGHT_CTRL  (1U << 4)
#define MG_TERMINAL_MODIFIER_RIGHT_SHIFT (1U << 5)
#define MG_TERMINAL_MODIFIER_RIGHT_ALT   (1U << 6)
#define MG_TERMINAL_MODIFIER_RIGHT_GUI   (1U << 7)

/* Output-control conveniences in the C library emit canonical ESC{...}
 * commands to stdout. They do not write to the framebuffer or bypass
 * redirection. The styled-write helpers emit literal payload bytes between
 * control commands and finish in the default style. */
mg_result_t terminal_alternate_enter(void);
mg_result_t terminal_alternate_leave(void);
mg_result_t terminal_cursor_move(u32 row, u32 column);
mg_result_t terminal_cursor_set_visible(bool visible);
mg_result_t terminal_clear_line(void);
mg_result_t terminal_clear_to_end(void);
/* Clear the visible screen and explicitly move the cursor to its origin. */
mg_result_t terminal_clear_screen(void);
/* Erase terminal contents and retained rows at an authenticated session
 * boundary. The kernel restricts this operation to logind. */
mg_result_t terminal_reset_session_history(void);
mg_result_t terminal_get_size(mg_terminal_size_t *out_size);
mg_result_t terminal_get_capabilities(
    mg_terminal_capabilities_t *out_capabilities);
mg_result_t terminal_overlay_set(const mg_terminal_overlay_cell_t *cells,
                                 u32 rows, u32 columns);
mg_result_t terminal_overlay_clear(void);
/* Starts a fresh interactive input line without overwriting partial output. */
mg_result_t terminal_prepare_input_line(void);
mg_result_t terminal_read_key(u32 timeout_ms, u32 *out_key);
mg_result_t terminal_read_key_event(u32 timeout_ms,
                                    mg_terminal_key_event_t *out_event);
mg_result_t terminal_input_raw(bool enabled);
mg_result_t terminal_update_begin(void);
mg_result_t terminal_update_end(void);

/* These convenience helpers use the canonical output stream. Payload bytes
 * are written literally, so text containing ESC cannot invoke controls. A
 * redirected stdout receives only payload text. The helper resets stream
 * style to the default after a successful styled write. */
mg_result_t terminal_write_styled(const void *buffer, usize length,
                                  mg_terminal_color_t foreground,
                                  mg_terminal_color_t background);

mg_result_t terminal_write_semantic(const void *buffer, usize length,
                                    mg_terminal_style_role_t role);

/* Reposition within the input anchored by terminal_prepare_input_line().
 * Offsets/extents count display cells, not UTF-8 bytes. */
typedef struct mg_terminal_input_cursor_request { u32 offset; u32 extent; } mg_terminal_input_cursor_request_t;
mg_result_t terminal_input_cursor(u32 offset, u32 extent);
