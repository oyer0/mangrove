/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include <string.h>
#include "../common/cli_options.h"
#include "../common/help.h"
#include "escape.h"

#define SAY_OUTPUT_CHUNK 256U

static const cli_option_spec_t say_options[] = {
    { 'e', "escapes", false },
    { 'n', "no-newline", false },
    { 'h', "help", false },
};

typedef struct {
    u8 bytes[SAY_OUTPUT_CHUNK];
    usize length;
    bool escapes;
    bool newline;
} say_output_t;

static bool output_flush(say_output_t *output)
{
    mg_result_t result;

    if (!output || !output->length) return true;
    result = output->escapes ? console_write(output->bytes, output->length) :
        console_write_literal(output->bytes, output->length);
    if (result != (mg_result_t)output->length) return false;
    output->length = 0U;
    return true;
}

static bool output_bytes(say_output_t *output, const u8 *bytes, usize length)
{
    if (!output || (length && !bytes)) return false;
    for (usize index = 0U; index < length; index++) {
        if (output->length == sizeof(output->bytes) &&
            !output_flush(output)) return false;
        output->bytes[output->length++] = bytes[index];
    }
    return true;
}

static bool output_argument(say_output_t *output, const char *argument)
{
    usize length;

    if (!output || !argument) return false;
    length = strlen(argument);
    if (!output->escapes)
        return output_bytes(output, (const u8 *)argument, length);

    usize offset = 0U;
    while (offset < length) {
        u8 expanded[2];
        usize expanded_length;
        if (!say_escape_next((const u8 *)argument, length, &offset,
                             expanded, &expanded_length) ||
            !output_bytes(output, expanded, expanded_length))
            return false;
    }
    return true;
}

static bool parse_options(int argc, char **argv, bool *escapes,
                          bool *newline,
                          bool *show_help, const char **error_argument,
                          cli_options_error_t *error)
{
    cli_options_parser_t parser;
    cli_options_item_t item;
    cli_options_item_kind_t kind;

    if (!escapes || !newline || !show_help || !error_argument || !error)
        return false;
    *escapes = false;
    *newline = true;
    *show_help = false;
    *error_argument = NULL;
    *error = CLI_OPTIONS_ERROR_NONE;
    cli_options_init(&parser, argc, argv, say_options,
                     sizeof(say_options) / sizeof(say_options[0]));
    while ((kind = cli_options_next(&parser, &item)) != CLI_OPTIONS_DONE) {
        if (kind == CLI_OPTIONS_ERROR) {
            *error_argument = parser.error_argument;
            *error = parser.error;
            return false;
        }
        if (kind == CLI_OPTIONS_OPTION) {
            if (item.option->short_name == 'e') *escapes = true;
            else if (item.option->short_name == 'n') *newline = false;
            else if (item.option->short_name == 'h') *show_help = true;
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    bool escapes;
    bool newline;
    bool show_help;
    const char *error_argument;
    cli_options_error_t parse_error;
    cli_options_parser_t parser;
    cli_options_item_t item;
    cli_options_item_kind_t kind;
    say_output_t output = {{0}, 0U, false, true};
    bool wrote_operand = false;

    if (!parse_options(argc, argv, &escapes, &newline, &show_help, &error_argument,
                       &parse_error)) {
        fprintf(stderr, "say: %s%s%s\n", cli_options_error_message(parse_error),
                error_argument ? ": " : "",
                error_argument ? error_argument : "");
        command_usage_error(argv[0],
                            "say [-e|--escapes] [-n|--no-newline] [--] [text ...]",
                            NULL);
        return 1;
    }
    if (show_help) return command_print_help(argv[0]);

    output.escapes = escapes;
    output.newline = newline;
    cli_options_init(&parser, argc, argv, say_options,
                     sizeof(say_options) / sizeof(say_options[0]));
    while ((kind = cli_options_next(&parser, &item)) != CLI_OPTIONS_DONE) {
        if (kind == CLI_OPTIONS_ERROR) return 1; /* Validated above. */
        if (kind != CLI_OPTIONS_OPERAND) continue;
        if (wrote_operand && !output_bytes(&output, (const u8 *)" ", 1U))
            goto output_error;
        if (!output_argument(&output, item.value)) goto output_error;
        wrote_operand = true;
    }
    if ((output.newline &&
         !output_bytes(&output, (const u8 *)"\n", 1U)) ||
        !output_flush(&output)) goto output_error;
    return 0;

output_error:
    fprintf(stderr, "say: output failed\n");
    return 1;
}
