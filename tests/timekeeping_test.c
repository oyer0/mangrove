/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/time.h>

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
        unix_time.seconds != expected_seconds ||
        unix_time.nanoseconds != input.nanoseconds ||
        unix_time_to_mangrove(&unix_time, &round_trip) != MG_OK ||
        round_trip.seconds != mangrove.seconds ||
        round_trip.nanoseconds != mangrove.nanoseconds)
        return 1;
    return 0;
}

int main(void)
{
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
        check_time(2040, 1, 1, 0, 0, 0, 1262304000LL))
        return 1;
    return 0;
}
