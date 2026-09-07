#include <mg/time.h>

#define MG_SECONDS_PER_MINUTE 60LL
#define MG_SECONDS_PER_HOUR   (60LL * MG_SECONDS_PER_MINUTE)
#define MG_SECONDS_PER_DAY    (24LL * MG_SECONDS_PER_HOUR)
#define MG_EPOCH_SERIAL       730425LL
#define MG_I64_MAX            ((i64)0x7fffffffffffffffLL)
#define MG_I64_MIN            ((i64)(-0x7fffffffffffffffLL - 1LL))

/* This is a proleptic Gregorian day serial.  Its fixed anchor is an
 * implementation detail of the conversion algorithm; public seconds remain
 * explicitly relative to the Mangrove epoch, never Unix time. */
static i64 civil_serial(i32 year, u8 month, u8 day)
{
    i64 adjusted_year = (i64)year - (month <= 2U ? 1 : 0);
    i64 era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) /
              400;
    i64 year_of_era = adjusted_year - era * 400;
    i64 month_prime = (i64)month + (month > 2U ? -3 : 9);
    i64 day_of_year = (153 * month_prime + 2) / 5 + (i64)day - 1;
    i64 day_of_era = year_of_era * 365 + year_of_era / 4 -
                     year_of_era / 100 + day_of_year;

    return era * 146097 + day_of_era;
}

static bool leap_year(i32 year)
{
    return (year % 4 == 0) &&
           ((year % 100 != 0) || (year % 400 == 0));
}

static u8 days_in_month(i32 year, u8 month)
{
    static const u8 days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    return month == 2U && leap_year(year) ? 29U : days[month - 1U];
}

static bool calendar_valid(const mg_calendar_time_t *calendar)
{
    return calendar && calendar->month >= 1U && calendar->month <= 12U &&
           calendar->day >= 1U &&
           calendar->day <= days_in_month(calendar->year, calendar->month) &&
           calendar->hour <= 23U && calendar->minute <= 59U &&
           calendar->second <= 59U &&
           calendar->nanoseconds < MG_TIME_NANOSECONDS_PER_SEC;
}

static bool serial_to_calendar(i64 serial, mg_calendar_time_t *out)
{
    i64 z = serial;
    i64 era;
    i64 day_of_era;
    i64 year_of_era;
    i64 day_of_year;
    i64 year;
    i64 month_prime;
    i64 day;
    i64 month;

    era = (z >= 0 ? z : z - 146096) / 146097;
    day_of_era = z - era * 146097;
    year_of_era = (day_of_era - day_of_era / 1460 +
                   day_of_era / 36524 - day_of_era / 146096) / 365;
    year = year_of_era + era * 400;
    day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 -
                                year_of_era / 100);
    month_prime = (5 * day_of_year + 2) / 153;
    day = day_of_year - (153 * month_prime + 2) / 5 + 1;
    month = month_prime + (month_prime < 10 ? 3 : -9);
    year += month <= 2;

    if (!out || year < (i64)(-0x80000000LL) ||
        year > (i64)0x7fffffffLL)
        return false;
    out->year = (i32)year;
    out->month = (u8)month;
    out->day = (u8)day;
    return true;
}

mg_result_t mangrove_time_from_calendar(const mg_calendar_time_t *calendar,
                                        mg_mangrove_time_t *out)
{
    i64 days;
    i64 seconds;

    if (!calendar_valid(calendar) || !out)
        return MG_ERR_BAD_ARGUMENT;
    days = civil_serial(calendar->year, calendar->month, calendar->day) -
           MG_EPOCH_SERIAL;
    if (days > MG_I64_MAX / MG_SECONDS_PER_DAY ||
        days < MG_I64_MIN / MG_SECONDS_PER_DAY)
        return MG_ERR_BAD_ARGUMENT;
    seconds = days * MG_SECONDS_PER_DAY +
              (i64)calendar->hour * MG_SECONDS_PER_HOUR +
              (i64)calendar->minute * MG_SECONDS_PER_MINUTE +
              calendar->second;
    out->seconds = seconds;
    out->nanoseconds = calendar->nanoseconds;
    out->reserved = 0;
    return MG_OK;
}

mg_result_t mangrove_time_to_calendar(const mg_mangrove_time_t *time,
                                      mg_calendar_time_t *out)
{
    i64 days;
    i64 remainder;

    if (!time || !out || time->nanoseconds >= MG_TIME_NANOSECONDS_PER_SEC)
        return MG_ERR_BAD_ARGUMENT;

    days = time->seconds / MG_SECONDS_PER_DAY;
    remainder = time->seconds % MG_SECONDS_PER_DAY;
    if (remainder < 0) {
        remainder += MG_SECONDS_PER_DAY;
        days--;
    }
    if (!serial_to_calendar(days + MG_EPOCH_SERIAL, out))
        return MG_ERR_BAD_ARGUMENT;
    out->hour = (u8)(remainder / MG_SECONDS_PER_HOUR);
    remainder %= MG_SECONDS_PER_HOUR;
    out->minute = (u8)(remainder / MG_SECONDS_PER_MINUTE);
    out->second = (u8)(remainder % MG_SECONDS_PER_MINUTE);
    out->nanoseconds = time->nanoseconds;
    return MG_OK;
}

mg_result_t mangrove_time_to_unix(const mg_mangrove_time_t *time,
                                  mg_unix_time_t *out)
{
    if (!time || !out || time->nanoseconds >= MG_TIME_NANOSECONDS_PER_SEC)
        return MG_ERR_BAD_ARGUMENT;
    out->seconds = time->seconds;
    out->nanoseconds = time->nanoseconds;
    out->reserved = 0;
    return MG_OK;
}

mg_result_t unix_time_to_mangrove(const mg_unix_time_t *time,
                                  mg_mangrove_time_t *out)
{
    if (!time || !out || time->nanoseconds >= MG_TIME_NANOSECONDS_PER_SEC)
        return MG_ERR_BAD_ARGUMENT;
    out->seconds = time->seconds;
    out->nanoseconds = time->nanoseconds;
    out->reserved = 0;
    return MG_OK;
}
