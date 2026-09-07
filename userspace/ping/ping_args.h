/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <mg/types.h>

#define PING_DEFAULT_COUNT 4U
#define PING_MAX_COUNT     1000U

typedef enum ping_parse_result {
    PING_PARSE_OK,
    PING_PARSE_INVALID_ARGUMENTS,
    PING_PARSE_INVALID_COUNT,
} ping_parse_result_t;

/* Parse the intentionally small Mangrove ping command line.  The returned
 * host points into argv and remains owned by the caller. */
ping_parse_result_t ping_parse_arguments(int argc, char **argv,
                                         u32 *count_out,
                                         const char **host_out);
