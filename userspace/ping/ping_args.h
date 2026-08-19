#pragma once

#include <mg/types.h>

#define PING_DEFAULT_COUNT 4U
#define PING_MAX_COUNT     16U

/* Parse the intentionally small Mangrove ping command line.  The returned
 * host points into argv and remains owned by the caller. */
bool ping_parse_arguments(int argc, char **argv, u32 *count_out,
                          const char **host_out);
