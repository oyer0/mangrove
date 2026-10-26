/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ping_args.h"

#include "../common/cli_options.h"
#include <string.h>

static const cli_option_spec_t ping_options[] = {
    { 'c', "count", true },
    { 'i', "interval", true },
    { 'h', "help", false },
};

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

static bool parse_interval_ms(const char *text, u64 *interval_ms_out)
{
    const u64 max = ~(u64)0;
    u64 seconds = 0;
    u64 fractional_ms = 0;
    bool decimal = false;
    bool have_seconds_digit = false;
    bool have_fraction_digit = false;
    bool fractional_round_up = false;
    u32 fractional_digits = 0;

    if (!text || !*text || !interval_ms_out) return false;
    for (const char *cursor = text; *cursor; cursor++) {
        if (*cursor == '.' && !decimal) {
            decimal = true;
            continue;
        }
        if (*cursor < '0' || *cursor > '9') return false;
        u64 digit = (u64)(*cursor - '0');
        if (!decimal) {
            have_seconds_digit = true;
            if (seconds > (max - digit) / 10U) return false;
            seconds = seconds * 10U + digit;
        } else {
            have_fraction_digit = true;
            if (fractional_digits < 3U) {
                fractional_ms = fractional_ms * 10U + digit;
            } else if (digit != 0) {
                fractional_round_up = true;
            }
            fractional_digits++;
        }
    }
    if (!have_seconds_digit || (decimal && !have_fraction_digit))
        return false;
    while (fractional_digits < 3U) {
        fractional_ms *= 10U;
        fractional_digits++;
    }
    if (seconds > (max - fractional_ms) / 1000U)
        return false;
    /* Compare the requested decimal against 0.002 before rounding to the
     * millisecond clock resolution. Values such as 0.001999 are rejected. */
    if (seconds == 0 && fractional_ms < PING_MIN_INTERVAL_MS)
        return false;
    u64 value = seconds * 1000U + fractional_ms;
    if (fractional_round_up) {
        if (value == max) return false;
        value++;
    }
    if (value < PING_MIN_INTERVAL_MS) return false;
    *interval_ms_out = value;
    return true;
}

ping_parse_result_t ping_parse_arguments(int argc, char **argv,
                                         u32 *count_out,
                                         u64 *interval_ms_out,
                                         const char **host_out,
                                         const char **bad_argument_out,
                                         cli_options_error_t *option_error_out)
{
    cli_options_parser_t parser;
    cli_options_item_t item;
    cli_options_item_kind_t kind;
    bool count_seen = false;
    bool interval_seen = false;

    if (!argv || !count_out || !interval_ms_out || !host_out || !bad_argument_out ||
        !option_error_out)
        return PING_PARSE_INVALID_ARGUMENTS;
    *count_out = PING_DEFAULT_COUNT;
    *interval_ms_out = PING_DEFAULT_INTERVAL_MS;
    *host_out = NULL;
    *bad_argument_out = NULL;
    *option_error_out = CLI_OPTIONS_ERROR_NONE;
    cli_options_init(&parser, argc, argv, ping_options,
                     sizeof(ping_options) / sizeof(ping_options[0]));
    while ((kind = cli_options_next(&parser, &item)) != CLI_OPTIONS_DONE) {
        if (kind == CLI_OPTIONS_ERROR) {
            *bad_argument_out = parser.error_argument;
            *option_error_out = parser.error;
            return PING_PARSE_INVALID_ARGUMENTS;
        }
        if (kind == CLI_OPTIONS_OPERAND) {
            if (*host_out) {
                *option_error_out = CLI_OPTIONS_ERROR_INVALID_ARGUMENTS;
                *bad_argument_out = item.value;
                return PING_PARSE_INVALID_ARGUMENTS;
            }
            *host_out = item.value;
            continue;
        }
        if (item.option->short_name == 'h') {
            *bad_argument_out = "--help";
            *option_error_out = CLI_OPTIONS_ERROR_NONE;
            return PING_PARSE_INVALID_ARGUMENTS;
        }
        if (item.option->short_name == 'c') {
            if (count_seen || !parse_count(item.value, count_out)) {
                *bad_argument_out = item.value;
                *option_error_out = CLI_OPTIONS_ERROR_INVALID_ARGUMENTS;
                return PING_PARSE_INVALID_COUNT;
            }
            count_seen = true;
        } else if (item.option->short_name == 'i') {
            if (interval_seen || !parse_interval_ms(item.value,
                                                     interval_ms_out)) {
                *bad_argument_out = item.value;
                *option_error_out = CLI_OPTIONS_ERROR_INVALID_ARGUMENTS;
                return PING_PARSE_INVALID_INTERVAL;
            }
            interval_seen = true;
        }
    }
    if (!*host_out) {
        *option_error_out = CLI_OPTIONS_ERROR_INVALID_ARGUMENTS;
        return PING_PARSE_INVALID_ARGUMENTS;
    }
    return PING_PARSE_OK;
}
