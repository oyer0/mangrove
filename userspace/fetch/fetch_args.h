/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "../common/cli_options.h"

typedef struct fetch_arguments {
    bool quiet;
    const char *output_path;
    const char *input_file;
    const char **urls;
    usize url_count;
} fetch_arguments_t;

typedef enum fetch_arguments_result {
    FETCH_ARGUMENTS_OK,
    FETCH_ARGUMENTS_INVALID,
    FETCH_ARGUMENTS_NO_MEMORY,
    FETCH_ARGUMENTS_DUPLICATE_OPTION,
    FETCH_ARGUMENTS_UNKNOWN_OPTION,
    FETCH_ARGUMENTS_MISSING_VALUE,
    FETCH_ARGUMENTS_UNEXPECTED_VALUE,
} fetch_arguments_result_t;

fetch_arguments_result_t fetch_arguments_parse(int argc, char **argv,
                                                fetch_arguments_t *out,
                                                const char **out_bad_argument);
void fetch_arguments_destroy(fetch_arguments_t *arguments);
