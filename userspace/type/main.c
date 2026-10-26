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
    char buffer[512];
    const char *path = NULL;
    bool show_help = false;
    mg_handle_t file;
    mg_result_t result;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    cli_options_init(&parser, argc, argv, specs,
                     sizeof(specs) / sizeof(specs[0]));
    while ((kind = cli_options_next(&parser, &item)) != CLI_OPTIONS_DONE) {
        if (kind == CLI_OPTIONS_ERROR) {
            if (parser.error == CLI_OPTIONS_ERROR_UNKNOWN_OPTION)
                command_usage_error(argv[0], "type <file>",
                                    parser.error_argument);
            else {
                printf("%s%s%s.\n", cli_options_error_message(parser.error),
                       parser.error_argument ? ": " : "",
                       parser.error_argument ? parser.error_argument : "");
                command_usage_error(argv[0], "type <file>", NULL);
            }
            return 1;
        }
        if (kind == CLI_OPTIONS_OPTION) show_help = true;
        else if (path) {
            command_usage_error(argv[0], "type <file>", NULL);
            return 1;
        } else path = item.value;
    }
    if (show_help) return command_print_help(argv[0]);
    if (!path) {
        command_usage_error(argv[0], "type <file>", NULL);
        return 1;
    }
    result = file_open(path, MG_OPEN_READ);
    if (result_is_error(result)) {
        printf("Could not read \"%s\": %s.\n", path, error_string(result));
        return 1;
    }
    file = (mg_handle_t)result;
    if (console_begin_transaction() != MG_OK) {
        (void)handle_close(file);
        printf("Could not read \"%s\": console unavailable.\n", path);
        return 1;
    }
    for (;;) {
        result = object_read(file, buffer, sizeof(buffer) - 1);
        if (result == MG_ERR_END_OF_FILE || result == 0) break;
        if (result_is_error(result)) {
            (void)console_end_transaction();
            printf("\nRead error: %s.\n", error_string(result));
            (void)handle_close(file);
            return 1;
        }
        buffer[result] = '\0';
        if (printf("%s", buffer) < 0) {
            (void)console_end_transaction();
            (void)handle_close(file);
            return 1;
        }
    }
    if (console_end_transaction() != MG_OK) {
        (void)handle_close(file);
        return 1;
    }
    (void)handle_close(file);
    return 0;
}
