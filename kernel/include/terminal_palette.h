/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <terminal_colors.h>
#include <types.h>

/* The only logical-color-to-framebuffer mapping.  Userspace sees the
 * logical enum, never these pixel values.  The fixed system console uses a
 * black background; light gray is the default foreground and Mangrove green
 * remains available as a palette entry. */
static const u32 terminal_palette_rgb[TERMINAL_COLOR_COUNT] = {
    [TERMINAL_COLOR_BLACK]           = 0x000000U,
    [TERMINAL_COLOR_RED]             = 0xE05252U,
    [TERMINAL_COLOR_GREEN]           = 0x32C76DU,
    [TERMINAL_COLOR_YELLOW]          = 0xD9A441U,
    [TERMINAL_COLOR_BLUE]            = 0x67A9FFU,
    [TERMINAL_COLOR_MAGENTA]         = 0xC58AF9U,
    [TERMINAL_COLOR_CYAN]            = 0x55DDE0U,
    [TERMINAL_COLOR_LIGHT_GRAY]      = 0xD7DEE8U,
    [TERMINAL_COLOR_DARK_GRAY]       = 0x697386U,
    [TERMINAL_COLOR_BRIGHT_RED]      = 0xFF8A8AU,
    [TERMINAL_COLOR_BRIGHT_GREEN]    = 0x7BE495U,
    [TERMINAL_COLOR_BRIGHT_YELLOW]   = 0xFFD166U,
    [TERMINAL_COLOR_BRIGHT_BLUE]     = 0x8AB4FFU,
    [TERMINAL_COLOR_BRIGHT_MAGENTA]  = 0xE5B3FFU,
    [TERMINAL_COLOR_BRIGHT_CYAN]     = 0x7FE7E9U,
    [TERMINAL_COLOR_WHITE]           = 0xF5F7FAU,
};
