#pragma once


#include <bootinfo.h>
#include <types.h>

/*
 * Initializes the font renderer.
 * Must be called once during kernel startup.
 */
void font_init(BOOT_INFO *boot_info);

/*
 * Draws a single character at the given pixel position.
 */
void draw_char(
    char c,
    u32 x,
    u32 y,
    u32 fg_color,
    u32 bg_color
);

/*
 * Returns the dimensions of a single glyph.
 */
u32 font_width(void);
u32 font_height(void);

