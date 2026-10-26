/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include <string.h>
#include "../common/help.h"
#include "../common/cli_options.h"
#include "../common/path.h"

#define MOVE_MAX_SOURCES 64U

static const char *source_basename(const char *path)
{
    const char *slash;

    if (!path || !*path) return NULL;
    slash = strrchr(path, '/');
    return slash ? slash + 1U : path;
}

int main(int argc, char **argv)
{
    static const cli_option_spec_t specs[] = {
        { 'h', "help", false },
    };
    cli_options_parser_t parser;
    cli_options_item_t item;
    cli_options_item_kind_t kind;
    const char *operands[MOVE_MAX_SOURCES + 1U];
    usize operand_count = 0U;
    usize source_count;
    const char *destination = NULL;
    mg_path_info_t destination_info;
    mg_result_t destination_result;
    bool destination_is_directory;
    bool failed = false;
    bool show_help = false;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    cli_options_init(&parser, argc, argv, specs,
                     sizeof(specs) / sizeof(specs[0]));
    while ((kind = cli_options_next(&parser, &item)) != CLI_OPTIONS_DONE) {
        if (kind == CLI_OPTIONS_ERROR) {
            if (parser.error == CLI_OPTIONS_ERROR_UNKNOWN_OPTION)
                command_usage_error(argv[0], "move <source>... <destination>",
                                    parser.error_argument);
            else {
                printf("%s%s%s.\n", cli_options_error_message(parser.error),
                       parser.error_argument ? ": " : "",
                       parser.error_argument ? parser.error_argument : "");
                command_usage_error(argv[0],
                                    "move <source>... <destination>", NULL);
            }
            return 1;
        }
        if (kind == CLI_OPTIONS_OPTION) {
            show_help = true;
            continue;
        }
        if (operand_count == sizeof(operands) / sizeof(operands[0])) {
            printf("Too many operands.\n");
            command_usage_error(argv[0],
                                "move <source>... <destination>", NULL);
            return 1;
        }
        operands[operand_count++] = item.value;
    }
    if (show_help) return command_print_help(argv[0]);
    if (operand_count < 2U || operand_count - 1U > MOVE_MAX_SOURCES) {
        command_usage_error(argv[0], "move <source>... <destination>",
                            NULL);
        return 1;
    }

    source_count = operand_count - 1U;
    destination = operands[operand_count - 1U];

    destination_result = path_info(destination, &destination_info);
    destination_is_directory = destination_result == MG_OK &&
        destination_info.type == MG_PATH_TYPE_DIRECTORY;
    if (source_count > 1U && !destination_is_directory) {
        if (destination_result == MG_OK) {
            printf("Could not move to \"%s\": multiple sources require an existing destination directory.\n",
                   destination);
        } else if (destination_result == MG_ERR_NOT_FOUND) {
            printf("Could not move to \"%s\": destination directory does not exist.\n",
                   destination);
        } else {
            printf("Could not move to \"%s\": destination lookup failed: %s.\n",
                   destination, error_string(destination_result));
        }
        return 1;
    }
    if (destination_result != MG_OK &&
        destination_result != MG_ERR_NOT_FOUND) {
        printf("Could not move to \"%s\": destination lookup failed: %s.\n",
               destination, error_string(destination_result));
        return 1;
    }

    for (usize index = 0; index < source_count; index++) {
        COMMAND_PATH_AUTO char *target = NULL;
        const char *source_argument = operands[index];
        const char *target_argument = destination;
        mg_result_t result;

        if (destination_is_directory) {
            const char *basename = source_basename(source_argument);
            if (!basename || !command_path_join_alloc(
                    destination, basename, &target)) {
                printf("Could not move \"%s\": could not construct destination path.\n",
                       source_argument);
                failed = true;
                continue;
            }
            target_argument = target;
        }

        result = path_move(source_argument, target_argument);
        if (result_is_error(result)) {
            printf("Could not move \"%s\" to \"%s\": %s.\n",
                   source_argument, target_argument, error_string(result));
            failed = true;
        }
    }
    return failed ? 1 : 0;
}
