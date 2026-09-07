#include "ping_args.h"
#include <string.h>

static bool parse_count(const char *text, u32 *count_out)
{
    u32 value = 0;
    u32 digits = 0;

    if (!text || !count_out) return false;
    while (*text >= '0' && *text <= '9') {
        u32 digit = (u32)(*text++ - '0');
        if (digits >= 4 || value > (PING_MAX_COUNT - digit) / 10U)
            return false;
        value = value * 10U + digit;
        digits++;
    }
    if (!digits || *text || value == 0) return false;
    *count_out = value;
    return true;
}

ping_parse_result_t ping_parse_arguments(int argc, char **argv,
                                         u32 *count_out,
                                         const char **host_out)
{
    if (!argv || !count_out || !host_out) return PING_PARSE_INVALID_ARGUMENTS;
    *count_out = PING_DEFAULT_COUNT;
    *host_out = 0;
    if (argc == 2 && argv[1] && argv[1][0] && argv[1][0] != '-') {
        *host_out = argv[1];
        return PING_PARSE_OK;
    }
    if (argc == 4 && argv[1] && argv[2] && argv[3] &&
        ((!strcmp(argv[1], "-c")) || (!strcmp(argv[1], "--count"))) &&
        argv[3][0]) {
        *host_out = argv[3];
        if (!parse_count(argv[2], count_out))
            return PING_PARSE_INVALID_COUNT;
        return PING_PARSE_OK;
    }
    return (argc >= 2 && argv[1] &&
            ((!strcmp(argv[1], "-c")) ||
             (!strcmp(argv[1], "--count"))))
        ? PING_PARSE_INVALID_COUNT : PING_PARSE_INVALID_ARGUMENTS;
}
