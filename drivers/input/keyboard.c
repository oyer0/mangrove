#include <keyboard.h>
#include <console.h>
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
static bool repeat_is_shift = false;
static u64 next_repeat_time = 0;

static char hid_to_ascii(u8 key, bool is_shift) {
    if (key >= 128) return 0;
    return is_shift ? usb_hid_to_ascii_upper[key] : usb_hid_to_ascii_lower[key];
}

void keyboard_update(void) {
    if (repeat_key != 0) {
        u64 now = timer_uptime_ms();
        if (now >= next_repeat_time) {
            char c = hid_to_ascii(repeat_key, repeat_is_shift);
            if (c) {
                console_input(c);
            }
            next_repeat_time = now + KEY_REPEAT_RATE_MS;
        }
    }
}

void usb_keyboard_handler(u8 modifier_mask, const u8 *key_codes, u8 count)
{
    /* Check Left Shift (Bit 1) or Right Shift (Bit 5) */
    bool is_shift = (modifier_mask & 0x02) || (modifier_mask & 0x20);
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
            char c = is_shift ? usb_hid_to_ascii_upper[key] : usb_hid_to_ascii_lower[key];
            if (c) {
                /* Feed key directly into console input stream */
                console_input(c);
                newly_pressed_key = key;
            }
        }
    }

    /* Update typematic repeat key tracking */
    if (newly_pressed_key != 0) {
        repeat_key = newly_pressed_key;
        repeat_is_shift = is_shift;
        next_repeat_time = timer_uptime_ms() + KEY_REPEAT_DELAY_MS;
    } else if (repeat_key != 0) {
        bool still_held = false;
        for (int i = 0; i < count; i++) {
            if (key_codes[i] == repeat_key) {
                still_held = true;
                break;
            }
        }

        if (still_held) {
            repeat_is_shift = is_shift;
        } else {
            if (count > 0) {
                repeat_key = key_codes[count - 1];
                repeat_is_shift = is_shift;
                next_repeat_time = timer_uptime_ms() + KEY_REPEAT_DELAY_MS;
            } else {
                repeat_key = 0;
            }
        }
    }

    /* Save state for the next interrupt report */
    for (int i = 0; i < 6; i++) {
        prev_usb_keys[i] = (i < count) ? key_codes[i] : 0;
    }
}
