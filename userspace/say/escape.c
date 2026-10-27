/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "escape.h"

bool say_escape_next(const u8 *input, usize length, usize *offset,
                     u8 output[2], usize *out_length)
{
    u8 value;

    if (!input || !offset || !output || !out_length || *offset >= length)
        return false;
    value = input[(*offset)++];
    *out_length = 1U;
    if (value != '\\') {
        output[0] = value;
        return true;
    }
    if (*offset >= length) {
        output[0] = '\\';
        return true;
    }

    value = input[(*offset)++];
    switch (value) {
        case '\\': output[0] = '\\'; break;
        case 'n': output[0] = '\n'; break;
        case 'r': output[0] = '\r'; break;
        case 't': output[0] = '\t'; break;
        case 'e': output[0] = 0x1bU; break;
        default:
            output[0] = '\\';
            output[1] = value;
            *out_length = 2U;
            break;
    }
    return true;
}
