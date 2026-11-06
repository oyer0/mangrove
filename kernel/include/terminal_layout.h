/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

#define TERMINAL_MARGIN_X 10U
#define TERMINAL_MARGIN_Y 10U
#define TERMINAL_LINE_SPACING 2U

static inline u32 terminal_layout_columns(u32 framebuffer_width,
                                          u32 cell_width)
{
    u32 margin = TERMINAL_MARGIN_X * 2U;

    if (!cell_width || framebuffer_width <= margin)
        return 0U;
    return (framebuffer_width - margin) / cell_width;
}

static inline u32 terminal_layout_rows(u32 framebuffer_height,
                                       u32 cell_height)
{
    u32 margin = TERMINAL_MARGIN_Y * 2U;
    u32 line_height = cell_height + TERMINAL_LINE_SPACING;

    if (!line_height || framebuffer_height <= margin)
        return 0U;
    return (framebuffer_height - margin) / line_height;
}
