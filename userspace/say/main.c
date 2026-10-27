/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mg/error.h>
#include <mg/terminal_markup.h>
#include "../common/help.h"

static int say_markup(int argc, char **argv)
{
    usize length = 1U; /* final newline */

    for (int index = 2; index < argc; index++) {
        usize part_length = strlen(argv[index]);
        if (part_length > (usize)-1 - length) {
            fprintf(stderr, "say: markup text is too large\n");
            return 1;
        }
        length += part_length;
        if (index + 1 < argc) {
            if (length == (usize)-1) {
                fprintf(stderr, "say: markup text is too large\n");
                return 1;
            }
            length++;
        }
    }

    char *message = (char *)malloc(length);
    if (!message) {
        fprintf(stderr, "say: cannot allocate markup output\n");
        return 1;
    }
    usize offset = 0U;
    for (int index = 2; index < argc; index++) {
        if (index > 2) message[offset++] = ' ';
        usize part_length = strlen(argv[index]);
        memcpy(message + offset, argv[index], part_length);
        offset += part_length;
    }
    message[offset++] = '\n';

    mg_result_t result = terminal_markup_write_template(message, offset);
    free(message);
    if (result < 0) {
        fprintf(stderr, "say: markup output failed: %s\n",
                error_string(result));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc > 1 && strcmp(argv[1], "--markup") == 0)
        return say_markup(argc, argv);
    for (int i = 1; i < argc; i++) {
        if (i > 1) printf(" ");
        printf("%s", argv[i]);
    }
    printf("\n");
    return 0;
}
