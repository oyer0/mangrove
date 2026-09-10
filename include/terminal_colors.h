/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/* Stable Mangrove logical terminal colors.  The numeric values are part of
 * the native terminal-control ABI; framebuffer pixel values are not. */
typedef enum {
    TERMINAL_COLOR_BLACK = 0,
    TERMINAL_COLOR_RED,
    TERMINAL_COLOR_GREEN,
    TERMINAL_COLOR_YELLOW,
    TERMINAL_COLOR_BLUE,
    TERMINAL_COLOR_MAGENTA,
    TERMINAL_COLOR_CYAN,
    TERMINAL_COLOR_LIGHT_GRAY,
    TERMINAL_COLOR_DARK_GRAY,
    TERMINAL_COLOR_BRIGHT_RED,
    TERMINAL_COLOR_BRIGHT_GREEN,
    TERMINAL_COLOR_BRIGHT_YELLOW,
    TERMINAL_COLOR_BRIGHT_BLUE,
    TERMINAL_COLOR_BRIGHT_MAGENTA,
    TERMINAL_COLOR_BRIGHT_CYAN,
    TERMINAL_COLOR_WHITE,
    TERMINAL_COLOR_COUNT
} terminal_color_t;

_Static_assert(TERMINAL_COLOR_COUNT == 16,
               "Mangrove terminal palette must contain 16 colors");

/* Capabilities describe the current output target, not its implementation. */
#define TERMINAL_CAP_STYLED_OUTPUT (1U << 0)

typedef enum {
    TERMINAL_STYLE_DEFAULT = 0,
    TERMINAL_STYLE_DIRECTORY,
    TERMINAL_STYLE_ERROR,
    TERMINAL_STYLE_WARNING,
    TERMINAL_STYLE_SUCCESS,
    TERMINAL_STYLE_HEADING,
    TERMINAL_STYLE_COUNT
} terminal_style_role_t;

typedef struct {
    terminal_color_t foreground;
    terminal_color_t background;
} terminal_style_t;

/* Static semantic roles for the fixed Mangrove system console.  Roles resolve
 * to logical palette entries; the framebuffer RGB mapping remains private to
 * the kernel renderer. */
static inline terminal_style_t terminal_style_for_role(
    terminal_style_role_t role)
{
    terminal_style_t style = {
        TERMINAL_COLOR_LIGHT_GRAY,
        TERMINAL_COLOR_BLACK,
    };

    switch (role) {
        case TERMINAL_STYLE_DIRECTORY:
            style.foreground = TERMINAL_COLOR_BRIGHT_BLUE;
            break;
        case TERMINAL_STYLE_ERROR:
            style.foreground = TERMINAL_COLOR_BRIGHT_RED;
            break;
        case TERMINAL_STYLE_WARNING:
            style.foreground = TERMINAL_COLOR_BRIGHT_YELLOW;
            break;
        case TERMINAL_STYLE_SUCCESS:
            style.foreground = TERMINAL_COLOR_BRIGHT_GREEN;
            break;
        case TERMINAL_STYLE_HEADING:
            style.foreground = TERMINAL_COLOR_BRIGHT_CYAN;
            break;
        case TERMINAL_STYLE_DEFAULT:
        default:
            break;
    }
    return style;
}
