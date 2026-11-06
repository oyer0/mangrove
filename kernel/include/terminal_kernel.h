/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <terminal_colors.h>
#include <types.h>

/* Kernel-only output for bounded diagnostics. Text is written into the
 * ordinary console cell ring using explicit colors; it does not change the
 * terminal palette, process ownership, or persistent boot-log contents. */
bool terminal_write_kernel_styled(const char *text, u64 length,
                                  terminal_color_t foreground,
                                  terminal_color_t background);
/* A bounded kernel diagnostic report may write several styled rows. These
 * delimit one shared-terminal batch so the framebuffer is presented once. */
bool terminal_kernel_output_begin(void);
void terminal_kernel_output_end(void);
