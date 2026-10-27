/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/types.h>

/* Consume one byte-oriented say -e escape unit. The result is one output byte
 * for ordinary and recognized escapes, or two bytes for an unknown escape.
 * Unknown and trailing backslashes are preserved literally. */
bool say_escape_next(const u8 *input, usize length, usize *offset,
                     u8 output[2], usize *out_length);
