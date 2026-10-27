/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/filesystem.h>
#include <mg/error.h>
#include <mg/object.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/help.h"
#include "../common/text_pager.h"

static void report_path_error(const char *prefix, const char *path,
                              mg_result_t error)
{
    static const char separator[] = ": ";
    static const char newline[] = "\n";
    static const char fallback[] = "view: unable to format error message safely\n";
    const char *message = error_string(error);
    usize prefix_length = strlen(prefix);
    usize path_length = strlen(path);
    usize error_length;
    usize total;
    char *output;
    usize offset = 0;

    if (!message) message = "unknown error";
    error_length = strlen(message);
    if (prefix_length > (usize)-1 - path_length ||
        prefix_length + path_length > (usize)-1 - sizeof(separator) + 1U ||
        prefix_length + path_length + sizeof(separator) - 1U >
            (usize)-1 - error_length ||
        prefix_length + path_length + sizeof(separator) - 1U + error_length >
            (usize)-1 - sizeof(newline) + 1U) {
        (void)console_write_literal(fallback, sizeof(fallback) - 1U);
        return;
    }
    total = prefix_length + path_length + sizeof(separator) - 1U +
            error_length + sizeof(newline) - 1U;
    output = malloc(total);
    if (!output) {
        (void)console_write_literal(fallback, sizeof(fallback) - 1U);
        return;
    }

    memcpy(output + offset, prefix, prefix_length);
    offset += prefix_length;
    memcpy(output + offset, path, path_length);
    offset += path_length;
    memcpy(output + offset, separator, sizeof(separator) - 1U);
    offset += sizeof(separator) - 1U;
    memcpy(output + offset, message, error_length);
    offset += error_length;
    memcpy(output + offset, newline, sizeof(newline) - 1U);
    offset += sizeof(newline) - 1U;
    (void)console_write_literal(output, offset);
    free(output);
}

int main(int argc, char **argv)
{
    char *text = NULL;
    usize length = 0U;
    mg_result_t result;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 2 || !argv[1] || !argv[1][0]) {
        command_usage_error(argv[0], "view FILE",
                            argc > 1 ? argv[1] : NULL);
        return 1;
    }

    result = text_pager_read_file(argv[1], &text, &length);
    if (result != MG_OK) {
        report_path_error("view: ", argv[1], result);
        return 1;
    }
    result = text_pager_run(argv[1], argv[1], text, length);
    free(text);
    if (result != MG_OK) {
        report_path_error("view: could not display ", argv[1], result);
        return 1;
    }
    return 0;
}
