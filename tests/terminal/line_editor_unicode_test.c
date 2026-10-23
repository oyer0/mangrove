/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/line_editor.h>
#include <stdio.h>
#include <string.h>

static int fail(const char *name)
{
    printf("Line editor Unicode test failed: %s\n", name);
    return 1;
}

int main(void)
{
    static const char original[] =
        "A\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x99\x82";
    char buffer[32];
    mg_line_editor_t editor;

    line_editor_init(&editor, buffer, sizeof(buffer), "");
    memcpy(buffer, original, sizeof(original));
    editor.length = sizeof(original) - 1U;
    editor.cursor = editor.length;
    if (!line_editor_apply_action(&editor, EDITOR_ACTION_DELETE_LEFT) ||
        editor.cursor != 6U || editor.length != 6U ||
        memcmp(buffer, "A\xC3\xA9\xE4\xB8\xAD", 6U) != 0)
        return fail("backspace removes one supplementary scalar");

    memcpy(buffer, original, sizeof(original));
    editor.length = sizeof(original) - 1U;
    editor.cursor = 3U;
    if (!line_editor_apply_action(&editor, EDITOR_ACTION_MOVE_LEFT) ||
        editor.cursor != 1U ||
        !line_editor_apply_action(&editor, EDITOR_ACTION_DELETE_RIGHT) ||
        editor.cursor != 1U || editor.length != 8U ||
        memcmp(buffer, "A\xE4\xB8\xAD\xF0\x9F\x99\x82", 8U) != 0)
        return fail("movement and delete preserve UTF-8 boundaries");

    return 0;
}
