#include <keyboard.h>
#include <console.h>
#include <editor_actions.h>
#include <io.h>
#include <stdbool.h>
#include <timer.h>

#define KEY_REPEAT_DELAY_MS 400
#define KEY_REPEAT_RATE_MS  40

static void ps2_flush(void)
{
    while (inb(0x64) & 0x01)
    {
        inb(0x60);
    }
}

/*
 * Disable PS/2 hardware entirely so only USB handles keyboard input.
 */
void keyboard_init(void)
{
    // Wait for the 8042 input buffer to be empty
    while (inb(0x64) & 0x02) {}
    
    // Disable First PS/2 Port (Keyboard)
    outb(0x64, 0xAD);

    // Wait for buffer to be empty
    while (inb(0x64) & 0x02) {}

    // Disable Second PS/2 Port (Mouse)
    outb(0x64, 0xA7);

    // Clear out any junk sitting in the PS/2 output buffer
    ps2_flush();
}

/* ==============================================================================
 * USB HID Keyboard Implementation
 * ============================================================================== */

static const char usb_hid_to_ascii_lower[128] = {
    0, 0, 0, 0, 
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '\n', 0, '\b', '\t', ' ', '-', '=', '[', ']', '\\', '\\', ';', '\'', '`', ',', '.', '/'
};

static const char usb_hid_to_ascii_upper[128] = {
    0, 0, 0, 0, 
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    '\n', 0, '\b', '\t', ' ', '_', '+', '{', '}', '|', '|', ':', '"', '~', '<', '>', '?'
};

static u8 prev_usb_keys[6] = {0};
static u8 repeat_key = 0;
static u8 repeat_modifiers = 0;
static editor_action_t repeat_action = EDITOR_ACTION_NONE;
static u64 next_repeat_time = 0;

static char hid_to_ascii(u8 key, bool is_shift) {
    if (key >= 128) return 0;
    return is_shift ? usb_hid_to_ascii_upper[key] : usb_hid_to_ascii_lower[key];
}

static editor_action_t key_to_editor_action(u8 key, bool control,
                                             bool shift, bool alt)
{
    /* Ctrl has no line-editor binding in Mangrove's initial keymap. */
    if (control) return EDITOR_ACTION_NONE;

    if (alt) {
        /* Home-row navigation bindings.  Keep all defaults in this keymap. */
        switch (key) {
            case 0x0B: return shift ? EDITOR_ACTION_SELECT_LEFT
                                    : EDITOR_ACTION_MOVE_LEFT;       /* Alt+h */
            case 0x0F: return shift ? EDITOR_ACTION_SELECT_RIGHT
                                    : EDITOR_ACTION_MOVE_RIGHT;      /* Alt+l */
            case 0x05: return shift ? EDITOR_ACTION_SELECT_WORD_LEFT
                                    : EDITOR_ACTION_MOVE_WORD_LEFT;  /* Alt+b */
            case 0x1A: return shift ? EDITOR_ACTION_SELECT_WORD_RIGHT
                                    : EDITOR_ACTION_MOVE_WORD_RIGHT; /* Alt+w */
            case 0x04: return shift ? EDITOR_ACTION_SELECT_LINE_START
                                    : EDITOR_ACTION_MOVE_LINE_START; /* Alt+a */
            case 0x08: return shift ? EDITOR_ACTION_SELECT_LINE_END
                                    : EDITOR_ACTION_MOVE_LINE_END;   /* Alt+e */
            default: break;
        }
    }

    switch (key) {
        case 0x50: /* Left */
            return shift ? EDITOR_ACTION_SELECT_LEFT : EDITOR_ACTION_MOVE_LEFT;
        case 0x4F: /* Right */
            return shift ? EDITOR_ACTION_SELECT_RIGHT : EDITOR_ACTION_MOVE_RIGHT;
        case 0x4A: /* Home */
            return shift ? EDITOR_ACTION_SELECT_LINE_START
                         : EDITOR_ACTION_MOVE_LINE_START;
        case 0x4D: /* End */
            return shift ? EDITOR_ACTION_SELECT_LINE_END
                         : EDITOR_ACTION_MOVE_LINE_END;
        case 0x2A: /* Backspace */
            return alt ? EDITOR_ACTION_DELETE_WORD_LEFT
                       : EDITOR_ACTION_DELETE_LEFT;
        case 0x4C: return EDITOR_ACTION_DELETE_RIGHT; /* Delete */
        case 0x52: return EDITOR_ACTION_HISTORY_PREVIOUS; /* Up */
        case 0x51: return EDITOR_ACTION_HISTORY_NEXT; /* Down */
        default: return EDITOR_ACTION_NONE;
    }
}

static bool emit_editor_action(editor_action_t action)
{
    if (action == EDITOR_ACTION_NONE) return false;

    console_input(EDITOR_ACTION_ESCAPE);
    console_input(EDITOR_ACTION_CSI);
    console_input(EDITOR_ACTION_MARKER);
    console_input(EDITOR_ACTION_ENCODE(action));
    return true;
}

static bool modifier_control(u8 modifiers)
{
    return (modifiers & 0x01) || (modifiers & 0x10);
}

static bool modifier_shift(u8 modifiers)
{
    return (modifiers & 0x02) || (modifiers & 0x20);
}

static bool modifier_alt(u8 modifiers)
{
    return (modifiers & 0x04) || (modifiers & 0x40);
}

static void begin_repeat(u8 key, u8 modifiers)
{
    repeat_key = key;
    repeat_modifiers = modifiers;
    repeat_action = key_to_editor_action(key, modifier_control(modifiers),
                                         modifier_shift(modifiers),
                                         modifier_alt(modifiers));
    next_repeat_time = timer_uptime_ms() + KEY_REPEAT_DELAY_MS;
}

static void stop_repeat(void)
{
    repeat_key = 0;
    repeat_modifiers = 0;
    repeat_action = EDITOR_ACTION_NONE;
}

void keyboard_update(void) {
    if (repeat_key != 0) {
        u64 now = timer_uptime_ms();
        if (now >= next_repeat_time) {
            if (repeat_action != EDITOR_ACTION_NONE) {
                /*
                 * An editor action is only repeatable while the modifiers
                 * that defined it are still present.  Do not turn Alt+l into
                 * literal l input when Alt is released mid-hold.
                 */
                editor_action_t current_action = key_to_editor_action(
                    repeat_key, modifier_control(repeat_modifiers),
                    modifier_shift(repeat_modifiers),
                    modifier_alt(repeat_modifiers));
                if (current_action == repeat_action) {
                    (void)emit_editor_action(repeat_action);
                }
            } else {
                char c = hid_to_ascii(repeat_key,
                                      modifier_shift(repeat_modifiers));
                if (c) console_input(c);
            }
            next_repeat_time = now + KEY_REPEAT_RATE_MS;
        }
    }
}

void usb_keyboard_handler(u8 modifier_mask, const u8 *key_codes, u8 count)
{
    bool is_control = modifier_control(modifier_mask);
    bool is_shift = modifier_shift(modifier_mask);
    bool is_alt = modifier_alt(modifier_mask);
    u8 newly_pressed_key = 0;

    for (int i = 0; i < count; i++) {
        u8 key = key_codes[i];
        bool is_new = true;

        /* Verify this key wasn't already held down in the last report */
        for (int j = 0; j < 6; j++) {
            if (prev_usb_keys[j] == key) {
                is_new = false;
                break;
            }
        }

        if (is_new && key < 128) {
            char c;
            editor_action_t action = key_to_editor_action(
                key, is_control, is_shift, is_alt);

            if (emit_editor_action(action)) {
                newly_pressed_key = key;
                continue;
            }

            c = is_shift ? usb_hid_to_ascii_upper[key] : usb_hid_to_ascii_lower[key];
            if (c) {
                /* Feed key directly into console input stream */
                console_input(c);
                newly_pressed_key = key;
            }
        }
    }

    /* Update typematic repeat key tracking */
    if (newly_pressed_key != 0) {
        begin_repeat(newly_pressed_key, modifier_mask);
    } else if (repeat_key != 0) {
        bool still_held = false;
        for (int i = 0; i < count; i++) {
            if (key_codes[i] == repeat_key) {
                still_held = true;
                break;
            }
        }

        if (still_held) {
            repeat_modifiers = modifier_mask;
        } else {
            if (count > 0) {
                begin_repeat(key_codes[count - 1], modifier_mask);
            } else {
                stop_repeat();
            }
        }
    }

    /* Save state for the next interrupt report */
    for (int i = 0; i < 6; i++) {
        prev_usb_keys[i] = (i < count) ? key_codes[i] : 0;
    }
}
