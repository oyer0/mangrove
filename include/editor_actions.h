#pragma once

/* Semantic line-editor actions carried over the console input stream. */
typedef enum editor_action {
    EDITOR_ACTION_NONE = 0,
    EDITOR_ACTION_MOVE_LEFT,
    EDITOR_ACTION_MOVE_RIGHT,
    EDITOR_ACTION_MOVE_WORD_LEFT,
    EDITOR_ACTION_MOVE_WORD_RIGHT,
    EDITOR_ACTION_MOVE_LINE_START,
    EDITOR_ACTION_MOVE_LINE_END,
    EDITOR_ACTION_SELECT_LEFT,
    EDITOR_ACTION_SELECT_RIGHT,
    EDITOR_ACTION_SELECT_WORD_LEFT,
    EDITOR_ACTION_SELECT_WORD_RIGHT,
    EDITOR_ACTION_SELECT_LINE_START,
    EDITOR_ACTION_SELECT_LINE_END,
    EDITOR_ACTION_DELETE_LEFT,
    EDITOR_ACTION_DELETE_RIGHT,
    EDITOR_ACTION_DELETE_WORD_LEFT,
    EDITOR_ACTION_HISTORY_PREVIOUS,
    EDITOR_ACTION_HISTORY_NEXT,
} editor_action_t;

#define EDITOR_ACTION_ESCAPE '\x1b'
#define EDITOR_ACTION_CSI    '['
#define EDITOR_ACTION_MARKER 'M'

/*
 * Editor actions travel through the byte-oriented console input queue.  Keep
 * their payload outside control-character space: console input normalizes
 * carriage return, and raw control values would make the protocol ambiguous.
 */
#define EDITOR_ACTION_WIRE_BASE 0x40U
#define EDITOR_ACTION_ENCODE(action) \
    ((char)(EDITOR_ACTION_WIRE_BASE + (unsigned int)(action)))
#define EDITOR_ACTION_DECODE(byte) \
    ((editor_action_t)((unsigned char)(byte) >= EDITOR_ACTION_WIRE_BASE \
        ? (unsigned char)(byte) - EDITOR_ACTION_WIRE_BASE \
        : EDITOR_ACTION_NONE))
