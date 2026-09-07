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

/* USB disconnect does not deliver a final empty report. Keep report and
 * typematic state per xHCI device instance so detach can discard exactly the
 * state that would otherwise synthesize keys forever. */
typedef struct {
    bool active;
    u64 generation;
    u8 previous_keys[6];
    u8 repeat_key;
    u8 repeat_modifiers;
    editor_action_t repeat_action;
    u64 next_repeat_time;
} usb_keyboard_source_t;

static usb_keyboard_source_t usb_keyboard_sources[256];

static char hid_to_ascii(u8 key, bool is_shift) {
    if (key >= 128) return 0;
    return is_shift ? usb_hid_to_ascii_upper[key] : usb_hid_to_ascii_lower[key];
}

static bool emit_raw_key(u8 key, bool is_shift)
{
    char character;

    /* USB HID 0x29 is Escape and is intentionally absent from the normal
     * printable key tables.  Raw terminal clients receive it as its native
     * single-key value. */
    if (key == 0x29U) {
        console_input_key(0x1BU);
        return true;
    }
    character = hid_to_ascii(key, is_shift);
    if (!character) return false;
    console_input_key((u32)(u8)character);
    return true;
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

    /* Basic deletion is part of the byte-oriented console input contract.
       Keep the semantic packets for navigation and word deletion, but emit
       the conventional control bytes that simple line readers (such as the
       login prompt) already understand. */
    if (action == EDITOR_ACTION_DELETE_LEFT) {
        console_input('\b');
        return true;
    }
    if (action == EDITOR_ACTION_DELETE_RIGHT) {
        console_input((char)0x7f);
        return true;
    }

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

static void begin_repeat(usb_keyboard_source_t *source, u8 key,
                         u8 modifiers)
{
    source->repeat_modifiers = modifiers;
    source->repeat_action = key_to_editor_action(
        key, modifier_control(modifiers), modifier_shift(modifiers),
        modifier_alt(modifiers));
    source->next_repeat_time = timer_uptime_ms() + KEY_REPEAT_DELAY_MS;
    /* keyboard_update() runs from the PIT path and may preempt a HID report
       callback between any two stores here.  Publish the key last so it can
       never observe a live repeat source with an uninitialized deadline. */
    __atomic_store_n(&source->repeat_key, key, __ATOMIC_RELEASE);
}

static void stop_repeat(usb_keyboard_source_t *source)
{
    __atomic_store_n(&source->repeat_key, 0, __ATOMIC_RELEASE);
    source->repeat_modifiers = 0;
    source->repeat_action = EDITOR_ACTION_NONE;
    source->next_repeat_time = 0;
}

void keyboard_update(void)
{
    for (u32 slot = 1; slot < 256; slot++) {
        usb_keyboard_source_t *source = &usb_keyboard_sources[slot];
        u8 repeat_key = __atomic_load_n(&source->repeat_key,
                                        __ATOMIC_ACQUIRE);
        u64 now;

        if (!source->active || repeat_key == 0)
            continue;
        now = timer_uptime_ms();
        if (now < source->next_repeat_time)
            continue;

        if (source->repeat_action != EDITOR_ACTION_NONE) {
            /* An editor action is only repeatable while the modifiers that
             * defined it are still present. Do not turn Alt+l into literal l
             * input when Alt is released mid-hold. */
            editor_action_t current_action = key_to_editor_action(
                repeat_key,
                modifier_control(source->repeat_modifiers),
                modifier_shift(source->repeat_modifiers),
                modifier_alt(source->repeat_modifiers));
            if (current_action == source->repeat_action)
                (void)emit_editor_action(source->repeat_action);
        } else if (console_raw_input_active()) {
            (void)emit_raw_key(repeat_key,
                               modifier_shift(source->repeat_modifiers));
        } else {
            char c = hid_to_ascii(repeat_key,
                                  modifier_shift(source->repeat_modifiers));
            if (c) console_input(c);
        }
        source->next_repeat_time = now + KEY_REPEAT_RATE_MS;
    }
}

void usb_keyboard_remove(u8 slot_id, u64 device_generation)
{
    usb_keyboard_source_t *source;

    if (slot_id == 0 || device_generation == 0)
        return;
    source = &usb_keyboard_sources[slot_id];
    if (source->active && source->generation == device_generation) {
        stop_repeat(source);
        source->active = false;
        __builtin_memset(source, 0, sizeof(*source));
    }
}

void usb_keyboard_handler(u8 slot_id, u64 device_generation,
                          u8 modifier_mask, const u8 *key_codes, u8 count)
{
    bool is_control = modifier_control(modifier_mask);
    bool is_shift = modifier_shift(modifier_mask);
    bool is_alt = modifier_alt(modifier_mask);
    u8 newly_pressed_key = 0;
    usb_keyboard_source_t *source;

    if (slot_id == 0 || device_generation == 0 || !key_codes)
        return;
    if (count > 6)
        count = 6;

    source = &usb_keyboard_sources[slot_id];
    if (!source->active || source->generation != device_generation) {
        __builtin_memset(source, 0, sizeof(*source));
        source->active = true;
        source->generation = device_generation;
    }

    for (u32 i = 0; i < count; i++) {
        u8 key = key_codes[i];
        bool is_new = true;

        /* Verify this key wasn't already held down in the previous report
         * from this same physical device instance. */
        for (u32 j = 0; j < 6; j++) {
            if (source->previous_keys[j] == key) {
                is_new = false;
                break;
            }
        }

        if (is_new && key < 128) {
            char c;
            editor_action_t action = key_to_editor_action(
                key, is_control, is_shift, is_alt);

            if (console_raw_input_active()) {
                if (emit_raw_key(key, is_shift))
                    newly_pressed_key = key;
                continue;
            }

            if (emit_editor_action(action)) {
                newly_pressed_key = key;
                continue;
            }

            c = is_shift ? usb_hid_to_ascii_upper[key]
                         : usb_hid_to_ascii_lower[key];
            if (c) {
                console_input(c);
                newly_pressed_key = key;
            }
        }
    }

    if (newly_pressed_key != 0) {
        begin_repeat(source, newly_pressed_key, modifier_mask);
    } else if (source->repeat_key != 0) {
        bool still_held = false;
        for (u32 i = 0; i < count; i++) {
            if (key_codes[i] == source->repeat_key) {
                still_held = true;
                break;
            }
        }

        if (still_held) {
            source->repeat_modifiers = modifier_mask;
        } else if (count > 0) {
            begin_repeat(source, key_codes[count - 1], modifier_mask);
        } else {
            stop_repeat(source);
        }
    }

    for (u32 i = 0; i < 6; i++)
        source->previous_keys[i] = (i < count) ? key_codes[i] : 0;
}
