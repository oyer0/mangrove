/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/types.h>

typedef struct cli_option_spec {
    char short_name;             /* Zero when this option has no short form. */
    const char *long_name;       /* Null when this option has no long form. */
    bool takes_value;
} cli_option_spec_t;

typedef enum cli_options_error {
    CLI_OPTIONS_ERROR_NONE,
    CLI_OPTIONS_ERROR_INVALID_ARGUMENTS,
    CLI_OPTIONS_ERROR_UNKNOWN_OPTION,
    CLI_OPTIONS_ERROR_MISSING_VALUE,
    CLI_OPTIONS_ERROR_UNEXPECTED_VALUE
} cli_options_error_t;

typedef enum cli_options_item_kind {
    CLI_OPTIONS_DONE,
    CLI_OPTIONS_OPTION,
    CLI_OPTIONS_OPERAND,
    CLI_OPTIONS_ERROR
} cli_options_item_kind_t;

typedef struct cli_options_item {
    cli_options_item_kind_t kind;
    const cli_option_spec_t *option;
    const char *value;
} cli_options_item_t;

typedef struct cli_options_parser {
    int argc;
    char **argv;
    const cli_option_spec_t *specs;
    usize spec_count;
    int index;
    usize short_offset;
    bool options_enabled;
    cli_options_error_t error;
    const char *error_argument;
} cli_options_parser_t;

/* Options are recognized throughout argv until `--`. A required value may
 * begin with `-`; it is consumed as the value rather than parsed as an option.
 * Returned option values and operands remain owned by argv. */
void cli_options_init(cli_options_parser_t *parser, int argc, char **argv,
                      const cli_option_spec_t *specs, usize spec_count);
cli_options_item_kind_t cli_options_next(cli_options_parser_t *parser,
                                        cli_options_item_t *item);

/* Convenience for flag-only commands. `enabled` has one entry per spec; any
 * operand or value-taking flag is rejected. */
cli_options_error_t cli_options_parse_flags(int argc, char **argv,
                                            const cli_option_spec_t *specs,
                                            usize spec_count, bool *enabled,
                                            const char **error_argument);
const char *cli_options_error_message(cli_options_error_t error);
