/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

void keyboard_init(void);
void keyboard_update(void);

/* xHCI identifies a keyboard report by the slot and boot-local device
 * instance that produced it.  Keep pressed/repeat state bound to that source:
 * a disconnect has no final HID "all released" report. */
void usb_keyboard_handler(u8 slot_id, u64 device_generation,
                          u8 modifier_mask, const u8 *key_codes, u8 count);
void usb_keyboard_remove(u8 slot_id, u64 device_generation);
