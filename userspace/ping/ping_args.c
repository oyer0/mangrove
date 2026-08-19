#include "ping_args.h"
#include <string.h>

static bool parse_count(const char *text, u32 *count_out)
{
    u32 value = 0;
    u32 digits = 0;

    if (!text || !count_out) return false;
    while (*text >= '0' && *text <= '9') {
        value = value * 10U + (u32)(*text++ - '0');
        if (++digits > 3 || value > PING_MAX_COUNT) return false;
    }
    if (!digits || *text || value == 0) return false;
    *count_out = value;
    return true;
}

bool ping_parse_arguments(int argc, char **argv, u32 *count_out,
                          const char **host_out)
{
    if (!argv || !count_out || !host_out) return false;
    *count_out = PING_DEFAULT_COUNT;
    *host_out = 0;
    if (argc == 2 && argv[1] && argv[1][0] && argv[1][0] != '-') {
        *host_out = argv[1];
        return true;
    }
    if (argc == 4 && argv[1] && argv[2] && argv[3] &&
        ((!strcmp(argv[1], "-c")) || (!strcmp(argv[1], "--count"))) &&
        argv[3][0] && parse_count(argv[2], count_out)) {
        *host_out = argv[3];
        return true;
    }
    return false;
}
