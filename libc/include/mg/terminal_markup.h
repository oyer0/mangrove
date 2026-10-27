/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/terminal.h>

/* Parse an explicit, trusted markup template and send its records through an
 * authorized terminal-control stream when stdout is interactive. With a
 * redirected or unavailable control stream, recognized operations are
 * omitted and only visible text is written to stdout. Unknown or malformed
 * directives are always emitted literally.
 *
 * Do not pass untrusted file, network, or log content as a template. Write
 * such content with console_write() or a literal MGTC text record instead. */
mg_result_t terminal_markup_write_template(const void *text, usize length);
