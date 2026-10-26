/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/time.h>

#define UNIX_EPOCH_OFFSET 946684800LL

static bool same_calendar(const mg_calendar_time_t *left,
                          const mg_calendar_time_t *right)
{
    return left->year == right->year && left->month == right->month &&
           left->day == right->day && left->hour == right->hour &&
           left->minute == right->minute && left->second == right->second &&
           left->nanoseconds == right->nanoseconds;
}

static int check_time(i32 year, u8 month, u8 day, u8 hour, u8 minute,
                      u8 second, i64 expected_seconds)
{
    mg_calendar_time_t input = {
        year, month, day, hour, minute, second, 123000000U
    };
    mg_calendar_time_t output;
    mg_mangrove_time_t mangrove;
    mg_unix_time_t unix_time;
    mg_mangrove_time_t round_trip;

    if (mangrove_time_from_calendar(&input, &mangrove) != MG_OK ||
        mangrove.seconds != expected_seconds ||
        mangrove.nanoseconds != input.nanoseconds ||
        mangrove_time_to_calendar(&mangrove, &output) != MG_OK ||
        !same_calendar(&input, &output) ||
        mangrove_time_to_unix(&mangrove, &unix_time) != MG_OK ||
        unix_time.seconds != expected_seconds + UNIX_EPOCH_OFFSET ||
        unix_time.nanoseconds != input.nanoseconds ||
        unix_time_to_mangrove(&unix_time, &round_trip) != MG_OK ||
        round_trip.seconds != mangrove.seconds ||
        round_trip.nanoseconds != mangrove.nanoseconds)
        return 1;
    return 0;
}

int main(void)
{
    mg_mangrove_time_t native = {0, 0, 0};
    mg_unix_time_t unix_time = {17, 23, 31};
    mg_mangrove_time_t round_trip = {19, 29, 37};
    mg_unix_time_t before = {1798761599LL, 999999999U, 0};
    mg_unix_time_t now = {1798761600LL, 0, 0};
    mg_unix_time_t after = {1798761600LL, 1, 0};
    i32 comparison = 77;
    if (check_time(1970, 1, 1, 0, 0, 0, -946684800LL) ||
        check_time(1999, 12, 31, 23, 59, 59, -1LL) ||
        check_time(2000, 1, 1, 0, 0, 0, 0LL) ||
        check_time(2000, 1, 1, 0, 0, 1, 1LL) ||
        check_time(2000, 1, 2, 0, 0, 0, 86400LL) ||
        check_time(2000, 2, 29, 0, 0, 0, 5097600LL) ||
        check_time(2001, 3, 1, 0, 0, 0, 36720000LL) ||
        check_time(2024, 2, 29, 0, 0, 0, 762480000LL) ||
        check_time(2025, 3, 1, 0, 0, 0, 794102400LL) ||
        check_time(2038, 1, 19, 3, 14, 7, 1200798847LL) ||
        check_time(2040, 1, 1, 0, 0, 0, 1262304000LL) ||
        check_time(2100, 2, 28, 23, 59, 59, 3160857599LL) ||
        check_time(2100, 3, 1, 0, 0, 0, 3160857600LL))
        return 1;

    if (mangrove_time_to_unix(&native, &unix_time) != MG_OK ||
        unix_time.seconds != UNIX_EPOCH_OFFSET || unix_time.nanoseconds != 0 ||
        unix_time.reserved != 0 ||
        unix_time_to_mangrove(&unix_time, &round_trip) != MG_OK ||
        round_trip.seconds != 0 || round_trip.nanoseconds != 0 ||
        round_trip.reserved != 0)
        return 2;

    /* X.509 validity endpoints are inclusive. The shared timestamp helper
     * compares seconds and fractional boundaries without a lossy cast. */
    if (mg_unix_time_compare(&before, &now, &comparison) != MG_OK ||
        comparison >= 0 ||
        mg_unix_time_compare(&now, &now, &comparison) != MG_OK ||
        comparison != 0 ||
        mg_unix_time_compare(&after, &now, &comparison) != MG_OK ||
        comparison <= 0)
        return 5;
    mg_unix_time_t invalid_fraction = {0, MG_TIME_NANOSECONDS_PER_SEC, 0};
    comparison = 77;
    if (mg_unix_time_compare(&invalid_fraction, &now, &comparison) !=
            MG_ERR_BAD_ARGUMENT || comparison != 77)
        return 6;
    mg_unix_time_t minimum = {-0x7fffffffffffffffLL - 1, 0, 0};
    mg_unix_time_t maximum = {0x7fffffffffffffffLL, 999999999U, 0};
    if (mg_unix_time_compare(&minimum, &maximum, &comparison) != MG_OK ||
        comparison >= 0)
        return 7;

    native.seconds = 0x7fffffffffffffffLL - UNIX_EPOCH_OFFSET + 1;
    unix_time = (mg_unix_time_t){17, 23, 31};
    if (mangrove_time_to_unix(&native, &unix_time) == MG_OK ||
        unix_time.seconds != 17 || unix_time.nanoseconds != 23 ||
        unix_time.reserved != 31)
        return 3;
    unix_time = (mg_unix_time_t){-0x7fffffffffffffffLL - 1 +
                                 UNIX_EPOCH_OFFSET - 1, 0, 0};
    round_trip = (mg_mangrove_time_t){19, 29, 37};
    if (unix_time_to_mangrove(&unix_time, &round_trip) == MG_OK ||
        round_trip.seconds != 19 || round_trip.nanoseconds != 29 ||
        round_trip.reserved != 37)
        return 4;
    return 0;
}
