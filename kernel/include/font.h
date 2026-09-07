#pragma once


#include <bootinfo.h>
#include <types.h>

/*
 * Initializes the font renderer.
 * Must be called once during kernel startup.
 */
void font_init(BOOT_INFO *boot_info);

/* Looks up a Unicode scalar value in the PSF2 Unicode table.  ASCII has a
 * direct-glyph fallback for legacy fonts that omit the table. */
bool font_lookup_codepoint(u32 codepoint, u32 *glyph_index);

/*
 * Draws one Unicode code point at the given pixel position.  Unmapped values
 * use the font's replacement glyph, '?' or space in that order.
 */
void draw_codepoint(
    u32 codepoint,
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
