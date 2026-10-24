/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>

/* Include the production dispatcher so this fixture exercises its real map. */
#include "../../userspace/reed/editor.c"

int main(void)
{
    mg_terminal_key_event_t event = {0};

    event.keycode = HID_KEY_U;
    event.modifiers = 0;
    assert(normal_action(&event) == REED_ACTION_UNDO);
    event.modifiers = MG_TERMINAL_MODIFIER_LEFT_SHIFT;
    assert(normal_action(&event) == REED_ACTION_REDO);
    event.keycode = HID_KEY_S;
    event.modifiers = MG_TERMINAL_MODIFIER_LEFT_ALT;
    assert(normal_action(&event) == REED_ACTION_SELECT_ALL);
    event.modifiers |= MG_TERMINAL_MODIFIER_LEFT_SHIFT;
    assert(normal_action(&event) == REED_ACTION_NONE);

    puts("Reed keymap: undo, redo, and Alt+S select all are mapped");
    return 0;
}
