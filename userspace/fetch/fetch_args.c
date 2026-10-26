/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fetch_args.h"

#include <stdlib.h>
#include <string.h>

static const cli_option_spec_t options[] = {
    {'O', "output-document", true},
    {'q', "quiet", false},
    {'i', "input-file", true},
};

void fetch_arguments_destroy(fetch_arguments_t *arguments)
{
    if (!arguments) return;
    free((void *)arguments->urls);
    *arguments = (fetch_arguments_t){0};
}

fetch_arguments_result_t fetch_arguments_parse(int argc, char **argv,
                                                fetch_arguments_t *out,
                                                const char **out_bad_argument)
{
    cli_options_parser_t parser;
    cli_options_item_t item;
    cli_options_item_kind_t kind;
    usize capacity = 0;

    if (!out || !out_bad_argument) return FETCH_ARGUMENTS_INVALID;
    *out = (fetch_arguments_t){0};
    *out_bad_argument = NULL;
    cli_options_init(&parser, argc, argv, options,
                     sizeof(options) / sizeof(options[0]));
    while ((kind = cli_options_next(&parser, &item)) != CLI_OPTIONS_DONE) {
        if (kind == CLI_OPTIONS_ERROR) {
            *out_bad_argument = parser.error_argument;
            fetch_arguments_destroy(out);
            switch (parser.error) {
                case CLI_OPTIONS_ERROR_UNKNOWN_OPTION:
                    return FETCH_ARGUMENTS_UNKNOWN_OPTION;
                case CLI_OPTIONS_ERROR_MISSING_VALUE:
                    return FETCH_ARGUMENTS_MISSING_VALUE;
                case CLI_OPTIONS_ERROR_UNEXPECTED_VALUE:
                    return FETCH_ARGUMENTS_UNEXPECTED_VALUE;
                default:
                    return FETCH_ARGUMENTS_INVALID;
            }
        }
        if (kind == CLI_OPTIONS_OPERAND) {
            if (out->url_count == capacity) {
                usize next = capacity ? capacity * 2U : 8U;
                const char **grown;
                if (next < capacity || next > (usize)-1 / sizeof(*grown)) {
                    *out_bad_argument = item.value;
                    fetch_arguments_destroy(out);
                    return FETCH_ARGUMENTS_NO_MEMORY;
                }
                grown = (const char **)realloc((void *)out->urls,
                                               next * sizeof(*grown));
                if (!grown) {
                    *out_bad_argument = item.value;
                    fetch_arguments_destroy(out);
                    return FETCH_ARGUMENTS_NO_MEMORY;
                }
                out->urls = grown;
                capacity = next;
            }
            out->urls[out->url_count++] = item.value;
            continue;
        }
        if (item.option == &options[0]) {
            if (out->output_path) {
                *out_bad_argument = item.value;
                fetch_arguments_destroy(out);
                return FETCH_ARGUMENTS_DUPLICATE_OPTION;
            }
            out->output_path = item.value;
        } else if (item.option == &options[1]) {
            out->quiet = true;
        } else if (item.option == &options[2]) {
            if (out->input_file) {
                *out_bad_argument = item.value;
                fetch_arguments_destroy(out);
                return FETCH_ARGUMENTS_DUPLICATE_OPTION;
            }
            out->input_file = item.value;
        } else {
            *out_bad_argument = item.value;
            fetch_arguments_destroy(out);
            return FETCH_ARGUMENTS_INVALID;
        }
    }
    if ((out->output_path && !out->output_path[0]) ||
        (out->input_file && !out->input_file[0])) {
        *out_bad_argument = out->output_path ? out->output_path : out->input_file;
        fetch_arguments_destroy(out);
        return FETCH_ARGUMENTS_INVALID;
    }
    if (out->input_file && out->url_count) {
        *out_bad_argument = out->input_file;
        fetch_arguments_destroy(out);
        return FETCH_ARGUMENTS_INVALID;
    }
    return FETCH_ARGUMENTS_OK;
}
