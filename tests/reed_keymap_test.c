/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>

/* Include the production dispatcher so this fixture exercises its real map. */
#include "../userspace/reed/editor.c"

int main(void)
{
    mg_terminal_key_event_t event = {0};

    event.keycode = HID_KEY_U;
    event.modifiers = 0;
    assert(normal_action(&event) == REED_ACTION_UNDO);

    event.modifiers = MG_TERMINAL_MODIFIER_LEFT_SHIFT;
    assert(normal_action(&event) == REED_ACTION_REDO);

    event.modifiers = MG_TERMINAL_MODIFIER_RIGHT_SHIFT;
    assert(normal_action(&event) == REED_ACTION_REDO);

    event.modifiers = MG_TERMINAL_MODIFIER_LEFT_ALT;
    assert(normal_action(&event) == REED_ACTION_NONE);

    event.modifiers = MG_TERMINAL_MODIFIER_LEFT_CTRL |
                      MG_TERMINAL_MODIFIER_LEFT_SHIFT;
    assert(normal_action(&event) == REED_ACTION_NONE);

    puts("Reed keymap: plain u undoes; left/right Shift+U redo; Alt+U is not redo");
    return 0;
}
