/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <string.h>

#include <mg/object.h>
#include <mg/types.h>

#include "../../userspace/common/help.h"

static u8 output[512];
static usize output_length;

mg_result_t console_write_literal(const void *buffer, usize length)
{
    if (length > sizeof(output) - output_length)
        return MG_ERR_BUFFER_TOO_SMALL;
    memcpy(output + output_length, buffer, length);
    output_length += length;
    return (mg_result_t)length;
}

#include "../../userspace/common/help.c"

int main(void)
{
    static const char option[] = "\x1b{cls}";
    static const char name[] = "\x1b{at:1,1}";
    static const char expected[] =
        "Unknown option: \x1b{cls}\n"
        "Usage: view FILE\n"
        "Try '\x1b{at:1,1} --help' for more information.\n";

    command_usage_error(name, "view FILE", option);
    assert(output_length == sizeof(expected) - 1U);
    assert(memcmp(output, expected, sizeof(expected) - 1U) == 0);
    return 0;
}
