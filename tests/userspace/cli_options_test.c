/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <string.h>

#include "../../userspace/common/cli_options.h"

static const cli_option_spec_t specs[] = {
    { 'l', "long", false },
    { 't', NULL, false },
    { 'c', "count", true },
    { 'i', "interval", true },
    { 'r', "recursive", false },
};

static usize collect(char **argv, int argc, const char **names,
                     const char **values, usize capacity,
                     const char **operands, usize operand_capacity,
                     cli_options_error_t *error, const char **error_argument)
{
    cli_options_parser_t parser;
    cli_options_item_t item;
    usize count = 0U;
    usize operand_count = 0U;
    cli_options_item_kind_t kind;

    cli_options_init(&parser, argc, argv, specs,
                     sizeof(specs) / sizeof(specs[0]));
    while ((kind = cli_options_next(&parser, &item)) != CLI_OPTIONS_DONE) {
        if (kind == CLI_OPTIONS_ERROR) break;
        if (kind == CLI_OPTIONS_OPTION) {
            assert(count < capacity);
            names[count] = item.option->long_name ? item.option->long_name :
                item.option->short_name == 'l' ? "l" :
                item.option->short_name == 't' ? "t" :
                item.option->short_name == 'c' ? "c" :
                item.option->short_name == 'i' ? "i" : "r";
            values[count++] = item.value;
        } else {
            assert(operand_count < operand_capacity);
            operands[operand_count++] = item.value;
        }
    }
    *error = parser.error;
    *error_argument = parser.error_argument;
    return count | (operand_count << 16U);
}

static void equivalent_boolean_groups(void)
{
    char *lt[] = { "list", "-lt", NULL };
    char *tl[] = { "list", "-tl", NULL };
    char *separate[] = { "list", "-l", "-t", NULL };
    const char *names[4], *values[4], *operands[2], *bad;
    cli_options_error_t error;
    usize result;

    result = collect(lt, 2, names, values, 4, operands, 2, &error, &bad);
    assert(error == CLI_OPTIONS_ERROR_NONE && (result & 0xffffU) == 2U);
    assert(!strcmp(names[0], "long") && !strcmp(names[1], "t"));
    result = collect(tl, 2, names, values, 4, operands, 2, &error, &bad);
    assert(error == CLI_OPTIONS_ERROR_NONE && (result & 0xffffU) == 2U);
    assert(!strcmp(names[0], "t") && !strcmp(names[1], "long"));
    result = collect(separate, 3, names, values, 4, operands, 2,
                     &error, &bad);
    assert(error == CLI_OPTIONS_ERROR_NONE && (result & 0xffffU) == 2U);
}

static void short_value_consumption(void)
{
    char *attached[] = { "ping", "-c20", "host", NULL };
    char *separated[] = { "ping", "-c", "20", "host", NULL };
    char *grouped[] = { "ping", "-ci", "20", "0.2", "host", NULL };
    char *negative[] = { "ping", "-i", "-0.2", "host", NULL };
    const char *names[8], *values[8], *operands[8], *bad;
    cli_options_error_t error;
    usize result;

    result = collect(attached, 3, names, values, 8, operands, 8,
                     &error, &bad);
    assert(error == CLI_OPTIONS_ERROR_NONE && (result & 0xffffU) == 1U);
    assert(!strcmp(names[0], "count") && !strcmp(values[0], "20"));
    assert((result >> 16U) == 1U && !strcmp(operands[0], "host"));
    result = collect(separated, 4, names, values, 8, operands, 8,
                     &error, &bad);
    assert(error == CLI_OPTIONS_ERROR_NONE && !strcmp(values[0], "20"));
    result = collect(grouped, 5, names, values, 8, operands, 8,
                     &error, &bad);
    assert(error == CLI_OPTIONS_ERROR_NONE && (result & 0xffffU) == 1U);
    assert(!strcmp(names[0], "count") && !strcmp(values[0], "i"));
    assert((result >> 16U) == 3U && !strcmp(operands[0], "20"));
    result = collect(negative, 4, names, values, 8, operands, 8,
                     &error, &bad);
    assert(error == CLI_OPTIONS_ERROR_NONE && !strcmp(values[0], "-0.2"));
}

static void long_options_and_sentinel(void)
{
    char *equals[] = { "ping", "--count=20", "--interval=0.2", "host", NULL };
    char *separated[] = { "ping", "--count", "20", "--interval", "0.2", "host", NULL };
    char *sentinel[] = { "list", "--", "-filename", NULL };
    const char *names[8], *values[8], *operands[8], *bad;
    cli_options_error_t error;
    usize result;

    result = collect(equals, 4, names, values, 8, operands, 8,
                     &error, &bad);
    assert(error == CLI_OPTIONS_ERROR_NONE && (result & 0xffffU) == 2U);
    assert(!strcmp(values[0], "20") && !strcmp(values[1], "0.2"));
    result = collect(separated, 6, names, values, 8, operands, 8,
                     &error, &bad);
    assert(error == CLI_OPTIONS_ERROR_NONE && (result & 0xffffU) == 2U);
    assert(!strcmp(values[0], "20") && !strcmp(values[1], "0.2"));
    result = collect(sentinel, 3, names, values, 8, operands, 8,
                     &error, &bad);
    assert(error == CLI_OPTIONS_ERROR_NONE && !(result & 0xffffU));
    assert((result >> 16U) == 1U && !strcmp(operands[0], "-filename"));
}

static void errors_are_explicit(void)
{
    char *unknown[] = { "cmd", "--wat", NULL };
    char *missing[] = { "cmd", "-c", NULL };
    char *bad_boolean[] = { "cmd", "--long=yes", NULL };
    char *unknown_group[] = { "cmd", "-lz", NULL };
    const char *names[4], *values[4], *operands[4], *bad;
    cli_options_error_t error;

    (void)collect(unknown, 2, names, values, 4, operands, 4, &error, &bad);
    assert(error == CLI_OPTIONS_ERROR_UNKNOWN_OPTION && !strcmp(bad, "--wat"));
    (void)collect(missing, 2, names, values, 4, operands, 4, &error, &bad);
    assert(error == CLI_OPTIONS_ERROR_MISSING_VALUE && !strcmp(bad, "-c"));
    (void)collect(bad_boolean, 2, names, values, 4, operands, 4,
                  &error, &bad);
    assert(error == CLI_OPTIONS_ERROR_UNEXPECTED_VALUE);
    (void)collect(unknown_group, 2, names, values, 4, operands, 4,
                  &error, &bad);
    assert(error == CLI_OPTIONS_ERROR_UNKNOWN_OPTION);
}

int main(void)
{
    equivalent_boolean_groups();
    short_value_consumption();
    long_options_and_sentinel();
    errors_are_explicit();
    return 0;
}
