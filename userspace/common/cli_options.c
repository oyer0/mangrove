/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cli_options.h"

#include <string.h>

static const cli_option_spec_t *find_short(const cli_options_parser_t *parser,
                                           char name)
{
    for (usize i = 0; i < parser->spec_count; i++)
        if (parser->specs[i].short_name == name && name)
            return &parser->specs[i];
    return NULL;
}

static const cli_option_spec_t *find_long(const cli_options_parser_t *parser,
                                          const char *name, usize length)
{
    for (usize i = 0; i < parser->spec_count; i++) {
        const char *candidate = parser->specs[i].long_name;

        if (candidate && strlen(candidate) == length &&
            !memcmp(candidate, name, length))
            return &parser->specs[i];
    }
    return NULL;
}

static cli_options_item_kind_t fail(cli_options_parser_t *parser,
                                    cli_options_error_t error,
                                    const char *argument,
                                    cli_options_item_t *item)
{
    parser->error = error;
    parser->error_argument = argument;
    item->kind = CLI_OPTIONS_ERROR;
    item->option = NULL;
    item->value = NULL;
    return item->kind;
}

void cli_options_init(cli_options_parser_t *parser, int argc, char **argv,
                      const cli_option_spec_t *specs, usize spec_count)
{
    if (!parser) return;
    parser->argc = argc;
    parser->argv = argv;
    parser->specs = specs;
    parser->spec_count = spec_count;
    parser->index = argc > 0 ? 1 : 0;
    parser->short_offset = 0;
    parser->options_enabled = true;
    parser->error = CLI_OPTIONS_ERROR_NONE;
    parser->error_argument = NULL;
}

cli_options_item_kind_t cli_options_next(cli_options_parser_t *parser,
                                        cli_options_item_t *item)
{
    const char *argument;
    const cli_option_spec_t *option;

    if (!parser || !item || parser->argc < 0 ||
        (parser->argc && !parser->argv) ||
        (parser->spec_count && !parser->specs)) {
        if (item) {
            item->kind = CLI_OPTIONS_ERROR;
            item->option = NULL;
            item->value = NULL;
        }
        if (parser) parser->error = CLI_OPTIONS_ERROR_INVALID_ARGUMENTS;
        return CLI_OPTIONS_ERROR;
    }

    item->option = NULL;
    item->value = NULL;
    for (;;) {
        if (!parser->short_offset) {
            if (parser->index >= parser->argc) {
                item->kind = CLI_OPTIONS_DONE;
                return item->kind;
            }
            argument = parser->argv[parser->index++];
            if (!argument)
                return fail(parser, CLI_OPTIONS_ERROR_INVALID_ARGUMENTS,
                            NULL, item);
            if (!parser->options_enabled || argument[0] != '-' ||
                argument[1] == '\0') {
                item->kind = CLI_OPTIONS_OPERAND;
                item->value = argument;
                return item->kind;
            }
            if (argument[1] == '-' && argument[2] == '\0') {
                parser->options_enabled = false;
                continue;
            }
            if (argument[1] == '-') {
                const char *name = argument + 2;
                const char *equals = strchr(name, '=');
                usize name_length = equals ? (usize)(equals - name) :
                                             strlen(name);

                option = find_long(parser, name, name_length);
                if (!option)
                    return fail(parser, CLI_OPTIONS_ERROR_UNKNOWN_OPTION,
                                argument, item);
                if (!option->takes_value) {
                    if (equals)
                        return fail(parser,
                                    CLI_OPTIONS_ERROR_UNEXPECTED_VALUE,
                                    argument, item);
                    item->kind = CLI_OPTIONS_OPTION;
                    item->option = option;
                    return item->kind;
                }
                if (equals) {
                    item->value = equals + 1;
                } else if (parser->index < parser->argc &&
                           parser->argv[parser->index]) {
                    item->value = parser->argv[parser->index++];
                } else {
                    return fail(parser, CLI_OPTIONS_ERROR_MISSING_VALUE,
                                argument, item);
                }
                item->kind = CLI_OPTIONS_OPTION;
                item->option = option;
                return item->kind;
            }
            parser->short_offset = 1U;
            parser->index--;
        }

        argument = parser->argv[parser->index];
        if (!argument || argument[parser->short_offset] == '\0') {
            parser->short_offset = 0;
            continue;
        }
        option = find_short(parser, argument[parser->short_offset]);
        if (!option)
            return fail(parser, CLI_OPTIONS_ERROR_UNKNOWN_OPTION,
                        argument, item);
        parser->short_offset++;
        if (option->takes_value) {
            if (argument[parser->short_offset]) {
                item->value = argument + parser->short_offset;
                parser->short_offset = 0;
                parser->index++;
            } else if (parser->index + 1 < parser->argc &&
                       parser->argv[parser->index + 1]) {
                item->value = parser->argv[parser->index + 1];
                parser->short_offset = 0;
                parser->index += 2;
            } else {
                return fail(parser, CLI_OPTIONS_ERROR_MISSING_VALUE,
                            argument, item);
            }
        } else if (!argument[parser->short_offset]) {
            parser->short_offset = 0;
            parser->index++;
        }
        item->kind = CLI_OPTIONS_OPTION;
        item->option = option;
        return item->kind;
    }
}

cli_options_error_t cli_options_parse_flags(int argc, char **argv,
                                            const cli_option_spec_t *specs,
                                            usize spec_count, bool *enabled,
                                            const char **error_argument)
{
    cli_options_parser_t parser;
    cli_options_item_t item;
    cli_options_item_kind_t kind;

    if (!specs || !enabled || !error_argument ||
        spec_count > (usize)-1 / sizeof(*enabled))
        return CLI_OPTIONS_ERROR_INVALID_ARGUMENTS;
    memset(enabled, 0, spec_count * sizeof(*enabled));
    *error_argument = NULL;
    cli_options_init(&parser, argc, argv, specs, spec_count);
    while ((kind = cli_options_next(&parser, &item)) != CLI_OPTIONS_DONE) {
        if (kind == CLI_OPTIONS_ERROR) {
            *error_argument = parser.error_argument;
            return parser.error;
        }
        if (kind == CLI_OPTIONS_OPERAND || item.option->takes_value) {
            *error_argument = item.value;
            return CLI_OPTIONS_ERROR_INVALID_ARGUMENTS;
        }
        for (usize index = 0; index < spec_count; index++) {
            if (item.option == &specs[index]) {
                enabled[index] = true;
                break;
            }
        }
    }
    return CLI_OPTIONS_ERROR_NONE;
}

const char *cli_options_error_message(cli_options_error_t error)
{
    switch (error) {
        case CLI_OPTIONS_ERROR_NONE: return "No option error";
        case CLI_OPTIONS_ERROR_INVALID_ARGUMENTS: return "Invalid arguments";
        case CLI_OPTIONS_ERROR_UNKNOWN_OPTION: return "Unknown option";
        case CLI_OPTIONS_ERROR_MISSING_VALUE: return "Missing option value";
        case CLI_OPTIONS_ERROR_UNEXPECTED_VALUE:
            return "Option does not take a value";
        default: return "Invalid option syntax";
    }
}
