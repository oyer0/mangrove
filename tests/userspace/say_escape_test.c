/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <string.h>
#include <userspace/say/escape.h>

static usize expand(const char *input, u8 *output, usize capacity)
{
    usize input_length = strlen(input);
    usize offset = 0U;
    usize output_length = 0U;

    while (offset < input_length) {
        u8 bytes[2];
        usize count;
        assert(say_escape_next((const u8 *)input, input_length, &offset,
                               bytes, &count));
        assert(count <= capacity - output_length);
        memcpy(output + output_length, bytes, count);
        output_length += count;
    }
    return output_length;
}

int main(void)
{
    static const char source[] = "plain\\n\\r\\t\\e\\\\\\q\\";
    static const u8 expected[] = {
        'p', 'l', 'a', 'i', 'n', '\n', '\r', '\t', 0x1bU, '\\',
        '\\', 'q', '\\'
    };
    u8 output[sizeof(expected)];

    assert(expand(source, output, sizeof(output)) == sizeof(expected));
    assert(!memcmp(output, expected, sizeof(expected)));

    /* UTF-8 is byte-preserving, and unknown or trailing escapes remain
     * literal rather than silently losing user text. */
    static const char utf8[] = "é\\x";
    static const u8 utf8_expected[] = {0xc3U, 0xa9U, '\\', 'x'};
    assert(expand(utf8, output, sizeof(output)) == sizeof(utf8_expected));
    assert(!memcmp(output, utf8_expected, sizeof(utf8_expected)));

    usize offset = 0U;
    u8 bytes[2];
    usize count = 0U;
    assert(!say_escape_next((const u8 *)"", 0U, &offset, bytes, &count));
    assert(!say_escape_next((const u8 *)"x", 1U, &offset, NULL, &count));
    return 0;
}
