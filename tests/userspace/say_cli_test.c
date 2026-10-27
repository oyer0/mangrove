/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <string.h>

#include <mg/error.h>
#include <mg/types.h>

int say_main(int argc, char **argv);

static u8 output[1024];
static usize output_length;
static bool used_escape_output;
static u32 usage_errors;

static mg_result_t capture(const void *buffer, usize length, bool escapes)
{
    if (length > sizeof(output) - output_length) return MG_ERR_BUFFER_TOO_SMALL;
    memcpy(output + output_length, buffer, length);
    output_length += length;
    used_escape_output = escapes;
    return (mg_result_t)length;
}

mg_result_t console_write(const void *buffer, usize length)
{
    return capture(buffer, length, true);
}

mg_result_t console_write_literal(const void *buffer, usize length)
{
    return capture(buffer, length, false);
}

int command_print_help(const char *argv0)
{
    (void)argv0;
    return 0;
}

void command_usage_error(const char *argv0, const char *usage,
                         const char *option)
{
    (void)argv0;
    (void)usage;
    (void)option;
    usage_errors++;
}

static void reset(void)
{
    memset(output, 0, sizeof(output));
    output_length = 0U;
    used_escape_output = false;
    usage_errors = 0U;
}

static void test_literal_default(void)
{
    char *argv[] = {"say", "left\\n", "\\e{cls}"};
    static const u8 expected[] = "left\\n \\e{cls}\n";
    reset();
    assert(say_main(3, argv) == 0);
    assert(!used_escape_output);
    assert(output_length == sizeof(expected) - 1U);
    assert(!memcmp(output, expected, sizeof(expected) - 1U));
}

static void test_escape_option(void)
{
    char *argv[] = {"say", "--escapes",
                    "\\e{fg:b_green}ok\\e{ok}\\n\\q"};
    static const u8 expected[] = {
        0x1bU, '{', 'f', 'g', ':', 'b', '_', 'g', 'r', 'e', 'e', 'n', '}',
        'o', 'k', 0x1bU, '{', 'o', 'k', '}', '\n', '\\', 'q', '\n'
    };
    reset();
    assert(say_main(3, argv) == 0);
    assert(used_escape_output);
    assert(output_length == sizeof(expected));
    assert(!memcmp(output, expected, sizeof(expected)));
}

static void test_end_of_options_and_unknown_option(void)
{
    char *argv[] = {"say", "-e", "--", "-literal\\t"};
    static const u8 expected[] = "-literal\t\n";
    reset();
    assert(say_main(4, argv) == 0);
    assert(used_escape_output);
    assert(output_length == sizeof(expected) - 1U);
    assert(!memcmp(output, expected, sizeof(expected) - 1U));

    char *bad[] = {"say", "--markup", "text"};
    reset();
    assert(say_main(3, bad) == 1);
    assert(!output_length && usage_errors == 1U);
}

int main(void)
{
    test_literal_default();
    test_escape_option();
    test_end_of_options_and_unknown_option();
    return 0;
}
