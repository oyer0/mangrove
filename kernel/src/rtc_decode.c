/* SPDX-License-Identifier: GPL-3.0-only */
#include <rtc.h>

static bool rtc_is_leap_year(i32 year)
{
    return (year % 4 == 0) &&
           ((year % 100 != 0) || (year % 400 == 0));
}

static u8 rtc_days_in_month(i32 year, u8 month)
{
    static const u8 days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    if (month == 2 && rtc_is_leap_year(year))
        return 29;
    return days[month - 1U];
}

static bool rtc_decode_bcd(u8 value, u8 *out)
{
    u8 low = value & 0x0fU;
    u8 high = (value >> 4) & 0x0fU;

    if (!out || low > 9U || high > 9U)
        return false;
    *out = (u8)(high * 10U + low);
    return true;
}

static bool rtc_decode_value(u8 value, bool binary, u8 *out)
{
    if (!out)
        return false;
    if (binary) {
        *out = value;
        return true;
    }
    return rtc_decode_bcd(value, out);
}

bool rtc_decode_sample(const rtc_register_sample_t *sample,
                       rtc_calendar_t *out)
{
    bool binary;
    bool twenty_four_hour;
    u8 seconds;
    u8 minutes;
    u8 hours;
    u8 day;
    u8 month;
    u8 year;
    u8 century = 0;
    i32 full_year;

    if (!sample || !out)
        return false;

    binary = (sample->status_b & (1U << 2)) != 0;
    twenty_four_hour = (sample->status_b & (1U << 1)) != 0;
    if (!rtc_decode_value(sample->seconds, binary, &seconds) ||
        !rtc_decode_value(sample->minutes, binary, &minutes) ||
        !rtc_decode_value(sample->hours & (twenty_four_hour ? 0xffU : 0x7fU),
                          binary, &hours) ||
        !rtc_decode_value(sample->day, binary, &day) ||
        !rtc_decode_value(sample->month, binary, &month) ||
        !rtc_decode_value(sample->year, binary, &year))
        return false;

    if (!twenty_four_hour) {
        bool pm = (sample->hours & 0x80U) != 0;
        if (hours < 1U || hours > 12U)
            return false;
        if (hours == 12U)
            hours = 0;
        if (pm)
            hours = (u8)(hours + 12U);
    }

    if (sample->century_valid) {
        if (!rtc_decode_value(sample->century, binary, &century))
            return false;
        full_year = (i32)century * 100 + year;
    } else {
        /* Standard PCs without an FADT century register expose a two-digit
         * year.  The 70/00 boundary is the documented modern-PC fallback:
         * it covers the complete 1970..2069 interval and, importantly,
         * treats 00..69 as post-1999 rather than pre-2000. */
        full_year = (year < 70U ? 2000 : 1900) + year;
    }

    if (full_year < 1 ||
        month < 1U || month > 12U || day < 1U ||
        day > rtc_days_in_month(full_year, month) ||
        hours > 23U || minutes > 59U || seconds > 59U)
        return false;

    out->year = full_year;
    out->month = month;
    out->day = day;
    out->hour = hours;
    out->minute = minutes;
    out->second = seconds;
    return true;
}
