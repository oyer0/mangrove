/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/* Lock state is expressed as logical state rather than a transport-specific
 * LED byte.  PS/2 and USB HID encode these bits differently. */
typedef u8 keyboard_lock_state_t;

/* A sink receives the logical lock state and must only publish the desired
 * transport state. It must not block: callers include IRQ1 and the xHCI
 * event worker. */
typedef void (*keyboard_led_sink_t)(keyboard_lock_state_t state);

#define KEYBOARD_LOCK_CAPS   (1U << 0)
#define KEYBOARD_LOCK_NUM    (1U << 1)
#define KEYBOARD_LOCK_SCROLL (1U << 2)

void keyboard_init(void);
void keyboard_update(void);
keyboard_lock_state_t keyboard_get_lock_state(void);
void keyboard_register_led_sink(keyboard_led_sink_t sink);

/* Canonical key make/repeat from a native transport. HID usage identities and
 * modifier bits match the existing terminal ABI. Releases belong to the
 * transport's held-key state; the terminal ABI has no release event. */
void keyboard_input_key(u8 key, u8 modifiers, bool repeat);

/* xHCI identifies a keyboard report by the slot and boot-local device
 * instance that produced it.  Keep pressed/repeat state bound to that source:
 * a disconnect has no final HID "all released" report. */
void usb_keyboard_handler(u8 slot_id, u64 device_generation,
                          u8 modifier_mask, const u8 *key_codes, u8 count);
void usb_keyboard_remove(u8 slot_id, u64 device_generation);
