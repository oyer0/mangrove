/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <drivers/input/keyboard.h>
#include <console.h>
#include <editor_actions.h>
#include <spinlock.h>
#include <stdbool.h>
#include <timer.h>
#include <input_trace.h>

#define KEY_REPEAT_DELAY_MS 400
#define KEY_REPEAT_RATE_MS  40

/* USB HID Usage ID for Caps Lock. */
#define USB_HID_KEY_CAPS_LOCK 0x39U
#define USB_HID_KEY_SCROLL_LOCK 0x47U
#define USB_HID_KEY_NUM_LOCK 0x53U

static volatile keyboard_lock_state_t keyboard_locks;
static spinlock_t keyboard_led_lock;
static bool keyboard_led_lock_ready;
static keyboard_led_sink_t keyboard_led_sink;

static void keyboard_sync_leds(void)
{
    u64 flags;
    keyboard_lock_state_t locks;
    keyboard_led_sink_t sink;

    if (!keyboard_led_lock_ready) return;
    flags = spin_lock_irqsave(&keyboard_led_lock);
    locks = __atomic_load_n(&keyboard_locks, __ATOMIC_ACQUIRE);
    sink = keyboard_led_sink;
    if (sink)
        sink(locks);
    spin_unlock_irqrestore(&keyboard_led_lock, flags);
}

static void keyboard_toggle_lock_state(u8 lock)
{
    keyboard_lock_state_t old_state;
    keyboard_lock_state_t new_state;

    do {
        old_state = __atomic_load_n(&keyboard_locks, __ATOMIC_ACQUIRE);
        new_state = old_state ^ lock;
    } while (!__atomic_compare_exchange_n(&keyboard_locks, &old_state,
                                          new_state, false,
                                          __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE));

}

static u8 hid_lock_for_key(u8 key)
{
    switch (key) {
        case USB_HID_KEY_CAPS_LOCK:
            return KEYBOARD_LOCK_CAPS;
        case USB_HID_KEY_NUM_LOCK:
            return KEYBOARD_LOCK_NUM;
        case USB_HID_KEY_SCROLL_LOCK:
            return KEYBOARD_LOCK_SCROLL;
        default:
            return 0;
    }
}

keyboard_lock_state_t keyboard_get_lock_state(void)
{
    return __atomic_load_n(&keyboard_locks, __ATOMIC_ACQUIRE);
}

void keyboard_register_led_sink(keyboard_led_sink_t sink)
{
    u64 flags;
    keyboard_lock_state_t locks;

    if (!keyboard_led_lock_ready)
        return;
    flags = spin_lock_irqsave(&keyboard_led_lock);
    keyboard_led_sink = sink;
    locks = __atomic_load_n(&keyboard_locks, __ATOMIC_ACQUIRE);
    if (keyboard_led_sink)
        keyboard_led_sink(locks);
    spin_unlock_irqrestore(&keyboard_led_lock, flags);
}

static void usb_keyboard_sources_init(void);

/* Logical input state is initialized early. Physical transports start only
 * after their bus, clock and interrupt routing are available. */
void keyboard_init(void)
{
    spinlock_init(&keyboard_led_lock);
    keyboard_led_lock_ready = true;
    usb_keyboard_sources_init();
    __atomic_store_n(&keyboard_locks, 0, __ATOMIC_RELEASE);
    keyboard_sync_leds();
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
    spinlock_t state_lock;
    bool active;
    u64 generation;
    u8 previous_keys[6];
    /* Atomic zero/nonzero hint lets the timer skip idle source locks; the
     * remaining repeat state is read only while state_lock is held. */
    u8 repeat_key;
    u8 repeat_modifiers;
    editor_action_t repeat_action;
    u64 next_repeat_time;
} usb_keyboard_source_t;

static usb_keyboard_source_t usb_keyboard_sources[256];

static void usb_keyboard_sources_init(void)
{
    for (u32 slot = 0; slot < 256U; slot++) {
        usb_keyboard_source_t *source = &usb_keyboard_sources[slot];

        spinlock_init(&source->state_lock);
        source->active = false;
        source->generation = 0;
        __builtin_memset(source->previous_keys, 0,
                         sizeof(source->previous_keys));
        __atomic_store_n(&source->repeat_key, 0, __ATOMIC_RELAXED);
        source->repeat_modifiers = 0;
        source->repeat_action = EDITOR_ACTION_NONE;
        source->next_repeat_time = 0;
    }
}

static char hid_to_ascii(u8 key, bool is_shift) {
    /* These keypad keys have distinct identities, but invariant text even
     * with Shift held. Both native PS/2 and USB retain those identities. */
    if (key == 0x58U) return '\n';
    if (key == 0x54U) return '/';
    if (key >= 128) return 0;
    return is_shift ? usb_hid_to_ascii_upper[key] : usb_hid_to_ascii_lower[key];
}

static bool hid_key_is_letter(u8 key)
{
    return key >= 0x04U && key <= 0x1DU;
}

static char hid_to_ascii_with_locks(u8 key, bool is_shift)
{
    bool uppercase = is_shift;

    if (hid_key_is_letter(key))
        uppercase = is_shift !=
            ((__atomic_load_n(&keyboard_locks, __ATOMIC_ACQUIRE) &
              KEYBOARD_LOCK_CAPS) != 0);
    return hid_to_ascii(key, uppercase);
}

static bool emit_raw_key(u8 key, u8 modifiers, bool is_shift, bool repeat)
{
    char character;
    u32 flags = repeat ? TERMINAL_KEY_EVENT_REPEAT : 0;

    /* USB HID 0x29 is Escape and is intentionally absent from the normal
     * printable key tables.  Raw terminal clients receive it as its native
     * single-key value. */
    if (key == 0x29U) {
        console_input_key_event_flags(key, modifiers, 0x1BU, flags);
        return true;
    }
    character = hid_to_ascii_with_locks(key, is_shift);
    /* Raw consumers need the physical key identity even for non-printable
     * keys such as arrows, paging keys, and delete. */
    console_input_key_event_flags(key, modifiers,
                                  character ? (u32)(u8)character : 0, flags);
    return true;
}

static editor_action_t key_to_editor_action(u8 key, bool control,
                                             bool shift, bool alt)
{
    /* Ctrl has no line-editor binding in Mangrove's initial keymap. */
    if (control) return EDITOR_ACTION_NONE;
    (void)shift;

    if (alt) {
        /* Compatibility actions for cooked loam line-editor consumers. Shoot
         * opts into key events and interprets its own conventional keymap. */
        switch (key) {
            case 0x0B: return EDITOR_ACTION_MOVE_LEFT;       /* Alt+h */
            case 0x0F: return EDITOR_ACTION_MOVE_RIGHT;      /* Alt+l */
            case 0x05: return EDITOR_ACTION_MOVE_WORD_LEFT;  /* Alt+b */
            case 0x1A: return EDITOR_ACTION_MOVE_WORD_RIGHT; /* Alt+w */
            case 0x04: return EDITOR_ACTION_MOVE_LINE_START; /* Alt+a */
            case 0x08: return EDITOR_ACTION_MOVE_LINE_END;   /* Alt+e */
            default: break;
        }
    }

    switch (key) {
        case 0x50: /* Left */
            return EDITOR_ACTION_MOVE_LEFT;
        case 0x4F: /* Right */
            return EDITOR_ACTION_MOVE_RIGHT;
        case 0x4A: /* Home */
            return EDITOR_ACTION_MOVE_LINE_START;
        case 0x4D: /* End */
            return EDITOR_ACTION_MOVE_LINE_END;
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

/* Both transports publish through the same console queues and keymap. No
 * device I/O or waiting is allowed here: PS/2 calls from IRQ1. */
static bool emit_key_press(u8 key, u8 modifiers, bool repeat)
{
    bool control = modifier_control(modifiers);
    bool shift = modifier_shift(modifiers);
    bool alt = modifier_alt(modifiers);
    if (key == 0x06U && control && !shift && !alt && !(modifiers & 0x88U)) {
        if (!repeat) (void)console_control_c();
        return false;
    }
    if (console_raw_input_active())
        return emit_raw_key(key, modifiers, shift, repeat);
    if (emit_editor_action(key_to_editor_action(key, control, shift, alt)))
        return true;
    if (control || alt) return false;
    char c = hid_to_ascii_with_locks(key, shift);
    if (c) console_input(c);
    return c != 0;
}

void keyboard_input_key(u8 key, u8 modifiers, bool repeat)
{
    u8 lock = hid_lock_for_key(key);
    if (lock) {
        if (!repeat) {
            keyboard_toggle_lock_state(lock);
            keyboard_sync_leds();
        }
        return;
    }
    (void)emit_key_press(key, modifiers, repeat);
}

static void begin_repeat(usb_keyboard_source_t *source, u8 key,
                         u8 modifiers)
{
    u8 previous_key = __atomic_load_n(&source->repeat_key,
                                      __ATOMIC_RELAXED);

    /* A newly pressed key takes over typematic generation for this device.
     * Do not leave the former key's already queued repeat for a raw client:
     * that direction may otherwise be delivered after the new key is
     * released. Initial keypress events are not repeats and remain ordered. */
    if (previous_key != 0 && previous_key != key)
        console_discard_key_repeats(previous_key);
    source->repeat_modifiers = modifiers;
    source->repeat_action = key_to_editor_action(
        key, modifier_control(modifiers), modifier_shift(modifiers),
        modifier_alt(modifiers));
    source->next_repeat_time = timer_uptime_ms() + KEY_REPEAT_DELAY_MS;
    __atomic_store_n(&source->repeat_key, key, __ATOMIC_RELEASE);
}

static void stop_repeat(usb_keyboard_source_t *source)
{
    u8 repeat_key = __atomic_load_n(&source->repeat_key, __ATOMIC_RELAXED);

    __atomic_store_n(&source->repeat_key, 0, __ATOMIC_RELEASE);
    if (repeat_key) console_discard_key_repeats(repeat_key);
    source->repeat_modifiers = 0;
    source->repeat_action = EDITOR_ACTION_NONE;
    source->next_repeat_time = 0;
}

static bool key_can_repeat(u8 key, u8 modifiers)
{
    if (hid_lock_for_key(key) != 0)
        return false;
    if (console_raw_input_active()) return true;
    if (key_to_editor_action(key, modifier_control(modifiers),
                             modifier_shift(modifiers),
                             modifier_alt(modifiers)) != EDITOR_ACTION_NONE)
        return true;
    if (modifier_control(modifiers) || modifier_alt(modifiers))
        return false;
    if (key == 0x29U) return true;
    return hid_to_ascii_with_locks(key, modifier_shift(modifiers)) != 0;
}

void keyboard_update(void)
{
    for (u32 slot = 1; slot < 256; slot++) {
        usb_keyboard_source_t *source = &usb_keyboard_sources[slot];
        u8 repeat_key = __atomic_load_n(&source->repeat_key,
                                        __ATOMIC_ACQUIRE);
        u64 flags;
        u64 now;

        if (repeat_key == 0)
            continue;
        flags = spin_lock_irqsave(&source->state_lock);
        repeat_key = __atomic_load_n(&source->repeat_key, __ATOMIC_RELAXED);
        if (!source->active || repeat_key == 0) {
            spin_unlock_irqrestore(&source->state_lock, flags);
            continue;
        }
        now = timer_uptime_ms();
        if (now < source->next_repeat_time) {
            spin_unlock_irqrestore(&source->state_lock, flags);
            continue;
        }

        input_trace_record(INPUT_TRACE_REPEAT, repeat_key,
                           source->repeat_action, slot);
        if (console_raw_input_active()) {
            (void)emit_raw_key(repeat_key, source->repeat_modifiers,
                               modifier_shift(source->repeat_modifiers), true);
        } else if (source->repeat_action != EDITOR_ACTION_NONE) {
            /* An editor action is only repeatable while the modifiers that
             * defined it are still present. Do not turn Alt+l into literal l
             * input when Alt is released mid-hold. */
            editor_action_t current_action = key_to_editor_action(
                repeat_key,
                modifier_control(source->repeat_modifiers),
                modifier_shift(source->repeat_modifiers),
                modifier_alt(source->repeat_modifiers));
            if (current_action == source->repeat_action) {
                (void)emit_editor_action(source->repeat_action);
            }
        } else {
            if (!modifier_control(source->repeat_modifiers) &&
                !modifier_alt(source->repeat_modifiers)) {
                char c = hid_to_ascii_with_locks(
                    repeat_key, modifier_shift(source->repeat_modifiers));
                if (c) console_input(c);
            }
        }
        source->next_repeat_time = now + KEY_REPEAT_RATE_MS;
        spin_unlock_irqrestore(&source->state_lock, flags);
    }
}

void usb_keyboard_remove(u8 slot_id, u64 device_generation)
{
    usb_keyboard_source_t *source;

    if (slot_id == 0 || device_generation == 0)
        return;
    source = &usb_keyboard_sources[slot_id];
    u64 flags = spin_lock_irqsave(&source->state_lock);
    if (source->active && source->generation == device_generation) {
        stop_repeat(source);
        source->active = false;
        source->generation = 0;
        __builtin_memset(source->previous_keys, 0,
                         sizeof(source->previous_keys));
        source->repeat_modifiers = 0;
        source->repeat_action = EDITOR_ACTION_NONE;
        source->next_repeat_time = 0;
    }
    spin_unlock_irqrestore(&source->state_lock, flags);
}

void usb_keyboard_handler(u8 slot_id, u64 device_generation,
                          u8 modifier_mask, const u8 *key_codes, u8 count)
{
    u8 newly_pressed_key = 0;
    bool sync_leds = false;
    usb_keyboard_source_t *source;
    u64 flags;

    if (slot_id == 0 || device_generation == 0 || !key_codes)
        return;
    if (count > 6)
        count = 6;

    source = &usb_keyboard_sources[slot_id];
    flags = spin_lock_irqsave(&source->state_lock);
    if (!source->active || source->generation != device_generation) {
        source->generation = 0;
        __builtin_memset(source->previous_keys, 0,
                         sizeof(source->previous_keys));
        __atomic_store_n(&source->repeat_key, 0, __ATOMIC_RELEASE);
        source->repeat_modifiers = 0;
        source->repeat_action = EDITOR_ACTION_NONE;
        source->next_repeat_time = 0;
        source->active = true;
        source->generation = device_generation;
    }

#if INPUT_TRACE_DIAGNOSTICS
    u32 navigation_mask = 0;
    u8 trace_key = __atomic_load_n(&source->repeat_key, __ATOMIC_RELAXED);
    for (u32 index = 0; index < count; index++) {
        u8 key = key_codes[index];
        if (key == 0x0B || key == 0x0D || key == 0x0E || key == 0x0F) {
            navigation_mask |= 1U << (key - 0x0B);
            trace_key = key;
        }
    }
    input_trace_record(INPUT_TRACE_REPORT, trace_key, navigation_mask,
                       modifier_mask | ((u32)slot_id << 8));
#endif
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
            if (hid_lock_for_key(key) != 0) {
                /* HID reports retain held keys, so this branch is reached
                 * only for the make transition and never for repeat/release. */
                keyboard_toggle_lock_state(hid_lock_for_key(key));
                sync_leds = true;
                continue;
            }
            if (emit_key_press(key, modifier_mask, false))
                newly_pressed_key = key;
        }
    }

    if (newly_pressed_key != 0) {
        begin_repeat(source, newly_pressed_key, modifier_mask);
    } else if (__atomic_load_n(&source->repeat_key, __ATOMIC_RELAXED) != 0) {
        u8 current_repeat = __atomic_load_n(&source->repeat_key,
                                             __ATOMIC_RELAXED);
        bool still_held = false;
        for (u32 i = 0; i < count; i++) {
            if (key_codes[i] == current_repeat) {
                still_held = true;
                break;
            }
        }

        if (still_held) {
            source->repeat_modifiers = modifier_mask;
        } else if (count > 0) {
            bool replacement = false;

            for (u32 i = count; i > 0; i--) {
                u8 candidate = key_codes[i - 1];
                if (key_can_repeat(candidate, modifier_mask)) {
                    begin_repeat(source, candidate, modifier_mask);
                    replacement = true;
                    break;
                }
            }
            if (!replacement) stop_repeat(source);
        } else {
            stop_repeat(source);
        }
    }

    for (u32 i = 0; i < 6; i++)
        source->previous_keys[i] = (i < count) ? key_codes[i] : 0;
    spin_unlock_irqrestore(&source->state_lock, flags);
    if (sync_leds) keyboard_sync_leds();
}
