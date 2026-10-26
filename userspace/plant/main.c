/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include "../common/help.h"
#include "../common/cli_options.h"

int main(int argc, char **argv)
{
    static const cli_option_spec_t specs[] = {
        { 'h', "help", false },
    };
    cli_options_parser_t parser;
    cli_options_item_t item;
    cli_options_item_kind_t kind;
    bool failed = false;
    bool have_target = false;
    bool show_help = false;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    cli_options_init(&parser, argc, argv, specs,
                     sizeof(specs) / sizeof(specs[0]));
    while ((kind = cli_options_next(&parser, &item)) != CLI_OPTIONS_DONE) {
        if (kind == CLI_OPTIONS_ERROR) {
            if (parser.error == CLI_OPTIONS_ERROR_UNKNOWN_OPTION)
                command_usage_error(argv[0], "plant <name>...",
                                    parser.error_argument);
            else {
                printf("%s%s%s.\n", cli_options_error_message(parser.error),
                       parser.error_argument ? ": " : "",
                       parser.error_argument ? parser.error_argument : "");
                command_usage_error(argv[0], "plant <name>...", NULL);
            }
            return 1;
        }
        if (kind == CLI_OPTIONS_OPTION) show_help = true;
        else have_target = true;
    }
    if (show_help) return command_print_help(argv[0]);
    if (!have_target) {
        command_usage_error(argv[0], "plant <name>...", NULL);
        return 1;
    }

    cli_options_init(&parser, argc, argv, specs,
                     sizeof(specs) / sizeof(specs[0]));
    while ((kind = cli_options_next(&parser, &item)) != CLI_OPTIONS_DONE) {
        if (kind != CLI_OPTIONS_OPERAND) continue;
        mg_result_t result;

        result = file_create(item.value);
        if (result == MG_ERR_ALREADY_EXISTS) {
            printf("Could not create \"%s\": already exists.\n",
                   item.value);
            failed = true;
            continue;
        }
        if (result_is_error(result)) {
            printf("Could not create \"%s\": %s.\n", item.value,
                   error_string(result));
            failed = true;
        }
    }
    return failed ? 1 : 0;
}
