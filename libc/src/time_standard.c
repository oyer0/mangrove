/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <time.h>

#include <errno.h>
#include <limits.h>
#include <mg/error.h>
#include <mg/time.h>

#define SECONDS_PER_DAY 86400LL
#define UNIX_DAYS_TO_1970 719468LL
#define I64_MAX_VALUE ((int64_t)0x7fffffffffffffffLL)
#define I64_MIN_VALUE ((int64_t)(-0x7fffffffffffffffLL - 1LL))

static int64_t floor_div(int64_t value, int64_t divisor)
{
    int64_t quotient = value / divisor;
    int64_t remainder = value % divisor;
    return quotient - (remainder < 0 ? 1 : 0);
}

static int64_t floor_mod(int64_t value, int64_t divisor)
{
    int64_t remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

/* Proleptic Gregorian days relative to 1970-01-01. */
static int64_t days_from_civil(int64_t year, unsigned month, unsigned day)
{
    int64_t adjusted_year = year - (month <= 2U ? 1 : 0);
    int64_t era = floor_div(adjusted_year, 400);
    unsigned year_of_era = (unsigned)(adjusted_year - era * 400);
    unsigned month_prime = month > 2U ? month - 3U : month + 9U;
    unsigned day_of_year = (153U * month_prime + 2U) / 5U + day - 1U;
    unsigned day_of_era = year_of_era * 365U + year_of_era / 4U -
                          year_of_era / 100U + day_of_year;
    return era * 146097 + (int64_t)day_of_era - UNIX_DAYS_TO_1970;
}

static bool leap_year(int64_t year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int weeks_in_iso_year(int64_t year)
{
    int january_first = (int)floor_mod(days_from_civil(year, 1U, 1U) + 4, 7);
    int monday_index = (january_first + 6) % 7;
    return monday_index == 3 || (monday_index == 2 && leap_year(year))
        ? 53 : 52;
}

struct tm *gmtime_r(const time_t *timer, struct tm *result)
{
    mg_unix_time_t unix_value;
    mg_mangrove_time_t mangrove_value;
    mg_calendar_time_t calendar;
    struct tm converted;
    int64_t tm_year;
    int64_t days;

    if (!timer || !result) {
        errno = EINVAL;
        return NULL;
    }
    unix_value.seconds = *timer;
    unix_value.nanoseconds = 0;
    unix_value.reserved = 0;
    if (unix_time_to_mangrove(&unix_value, &mangrove_value) != MG_OK ||
        mangrove_time_to_calendar(&mangrove_value, &calendar) != MG_OK) {
        errno = EOVERFLOW;
        return NULL;
    }
    tm_year = (int64_t)calendar.year - 1900;
    if (tm_year < INT_MIN || tm_year > INT_MAX) {
        errno = EOVERFLOW;
        return NULL;
    }
    days = floor_div(*timer, SECONDS_PER_DAY);

    converted.tm_sec = calendar.second;
    converted.tm_min = calendar.minute;
    converted.tm_hour = calendar.hour;
    converted.tm_mday = calendar.day;
    converted.tm_mon = (int)calendar.month - 1;
    converted.tm_year = (int)tm_year;
    converted.tm_wday = (int)floor_mod(days + 4, 7);
    converted.tm_yday = (int)(days - days_from_civil(calendar.year, 1U, 1U));
    converted.tm_isdst = 0;
    *result = converted;
    return result;
}

struct tm *localtime_r(const time_t *timer, struct tm *result)
{
    /* No zone database is present; the local-time policy is UTC. */
    return gmtime_r(timer, result);
}

static struct tm static_broken_down;
static char static_asctime[26];

struct tm *gmtime(const time_t *timer)
{
    return gmtime_r(timer, &static_broken_down);
}

struct tm *localtime(const time_t *timer)
{
    return localtime_r(timer, &static_broken_down);
}

time_t time(time_t *timer)
{
    mg_mangrove_time_t native_time;
    mg_unix_time_t unix_time;
    mg_result_t result;

    result = mg_clock_realtime(&native_time);
    if (result != MG_OK) {
        mg_set_errno_from_result(result);
        if (timer)
            *timer = (time_t)-1;
        return (time_t)-1;
    }
    if (mangrove_time_to_unix(&native_time, &unix_time) != MG_OK) {
        errno = EOVERFLOW;
        if (timer)
            *timer = (time_t)-1;
        return (time_t)-1;
    }
    if (timer)
        *timer = unix_time.seconds;
    return unix_time.seconds;
}

double difftime(time_t time1, time_t time0)
{
    if (time1 >= time0)
        return (double)((uint64_t)time1 - (uint64_t)time0);
    return -(double)((uint64_t)time0 - (uint64_t)time1);
}

clock_t clock(void)
{
    mg_process_cpu_time_t cpu_time;
    mg_result_t result = mg_clock_process_cpu(&cpu_time);

    if (result != MG_OK) {
        mg_set_errno_from_result(result);
        return (clock_t)-1;
    }
    if (cpu_time.ticks > (uint64_t)I64_MAX_VALUE) {
        errno = EOVERFLOW;
        return (clock_t)-1;
    }
    return (clock_t)cpu_time.ticks;
}

int timespec_get(struct timespec *ts, int base)
{
    mg_mangrove_time_t native_time;
    mg_unix_time_t unix_time;
    struct timespec converted;
    mg_result_t result;

    if (base != TIME_UTC)
        return 0;
    if (!ts) {
        errno = EINVAL;
        return 0;
    }
    result = mg_clock_realtime(&native_time);
    if (result != MG_OK) {
        mg_set_errno_from_result(result);
        return 0;
    }
    if (mangrove_time_to_unix(&native_time, &unix_time) != MG_OK) {
        errno = EOVERFLOW;
        return 0;
    }
    converted.tv_sec = unix_time.seconds;
    converted.tv_nsec = native_time.nanoseconds;
    *ts = converted;
    return TIME_UTC;
}

static const char *const weekday_short[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char *const weekday_long[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
    "Saturday"
};
static const char *const month_short[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const char *const month_long[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static bool valid_tm(const struct tm *value)
{
    return value && value->tm_sec >= 0 && value->tm_sec <= 60 &&
           value->tm_min >= 0 && value->tm_min <= 59 &&
           value->tm_hour >= 0 && value->tm_hour <= 23 &&
           value->tm_mday >= 1 && value->tm_mday <= 31 &&
           value->tm_mon >= 0 && value->tm_mon <= 11 &&
           value->tm_wday >= 0 && value->tm_wday <= 6 &&
           value->tm_yday >= 0 && value->tm_yday <= 365;
}

static void two_digits(char *out, unsigned value)
{
    out[0] = (char)('0' + (value / 10U) % 10U);
    out[1] = (char)('0' + value % 10U);
}

char *asctime_r(const struct tm *timeptr, char *buffer)
{
    int64_t year;
    if (!buffer || !valid_tm(timeptr)) {
        errno = EINVAL;
        return NULL;
    }
    year = (int64_t)timeptr->tm_year + 1900;
    if (year < 1000 || year > 9999) {
        errno = EOVERFLOW;
        return NULL;
    }
    buffer[0] = weekday_short[timeptr->tm_wday][0];
    buffer[1] = weekday_short[timeptr->tm_wday][1];
    buffer[2] = weekday_short[timeptr->tm_wday][2];
    buffer[3] = ' ';
    buffer[4] = month_short[timeptr->tm_mon][0];
    buffer[5] = month_short[timeptr->tm_mon][1];
    buffer[6] = month_short[timeptr->tm_mon][2];
    buffer[7] = ' ';
    buffer[8] = timeptr->tm_mday < 10 ? ' ' :
                (char)('0' + timeptr->tm_mday / 10);
    buffer[9] = (char)('0' + timeptr->tm_mday % 10);
    buffer[10] = ' ';
    two_digits(buffer + 11, (unsigned)timeptr->tm_hour);
    buffer[13] = ':';
    two_digits(buffer + 14, (unsigned)timeptr->tm_min);
    buffer[16] = ':';
    two_digits(buffer + 17, (unsigned)timeptr->tm_sec);
    buffer[19] = ' ';
    buffer[20] = (char)('0' + (year / 1000) % 10);
    buffer[21] = (char)('0' + (year / 100) % 10);
    buffer[22] = (char)('0' + (year / 10) % 10);
    buffer[23] = (char)('0' + year % 10);
    buffer[24] = '\n';
    buffer[25] = '\0';
    return buffer;
}

char *asctime(const struct tm *timeptr)
{
    return asctime_r(timeptr, static_asctime);
}

char *ctime_r(const time_t *timer, char *buffer)
{
    struct tm broken_down;
    if (!timer || !buffer || !localtime_r(timer, &broken_down))
        return NULL;
    return asctime_r(&broken_down, buffer);
}

char *ctime(const time_t *timer)
{
    return ctime_r(timer, static_asctime);
}

static bool mktime_value(const struct tm *input, time_t *out)
{
    int64_t year = (int64_t)input->tm_year + 1900;
    int64_t month = input->tm_mon;
    int64_t total_seconds;
    int64_t days;
    int64_t day_adjustment;
    int64_t seconds_of_day;

    year += floor_div(month, 12);
    month = floor_mod(month, 12);
    days = days_from_civil(year, (unsigned)month + 1U, 1U) +
           (int64_t)input->tm_mday - 1;
    total_seconds = (int64_t)input->tm_hour * 3600 +
                    (int64_t)input->tm_min * 60 + input->tm_sec;
    day_adjustment = floor_div(total_seconds, SECONDS_PER_DAY);
    seconds_of_day = total_seconds - day_adjustment * SECONDS_PER_DAY;
    days += day_adjustment;
    if (days > I64_MAX_VALUE / SECONDS_PER_DAY ||
        days < I64_MIN_VALUE / SECONDS_PER_DAY)
        return false;
    *out = days * SECONDS_PER_DAY + seconds_of_day;
    return true;
}

time_t mktime(struct tm *timeptr)
{
    struct tm normalized;
    time_t value;
    if (!timeptr) {
        errno = EINVAL;
        return (time_t)-1;
    }
    if (!mktime_value(timeptr, &value) || !gmtime_r(&value, &normalized)) {
        errno = EOVERFLOW;
        return (time_t)-1;
    }
    normalized.tm_isdst = 0;
    *timeptr = normalized;
    return value;
}

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
    bool failed;
} strftime_writer_t;

static void put_bytes(strftime_writer_t *writer, const char *value,
                      size_t length)
{
    if (writer->failed ||
        length > writer->capacity - writer->length - 1U) {
        writer->failed = true;
        return;
    }
    for (size_t i = 0; i < length; i++)
        writer->buffer[writer->length++] = value[i];
}

static size_t text_length(const char *value)
{
    size_t length = 0;
    while (value[length]) length++;
    return length;
}

static void put_text(strftime_writer_t *writer, const char *value)
{
    put_bytes(writer, value, text_length(value));
}

static void put_unsigned(strftime_writer_t *writer, uint64_t value,
                         unsigned minimum_width, char pad)
{
    char digits[24];
    unsigned count = 0;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value);
    while (count < minimum_width)
        digits[count++] = pad;
    while (count)
        put_bytes(writer, &digits[--count], 1U);
}

static void put_signed(strftime_writer_t *writer, int64_t value,
                       unsigned minimum_width, char pad)
{
    uint64_t magnitude;
    if (value < 0) {
        put_bytes(writer, "-", 1U);
        magnitude = (uint64_t)(-(value + 1)) + 1U;
    } else {
        magnitude = (uint64_t)value;
    }
    put_unsigned(writer, magnitude, minimum_width, pad);
}

static void put_year(strftime_writer_t *writer, int64_t year)
{
    put_signed(writer, year, 4U, '0');
}

static void put_two(strftime_writer_t *writer, unsigned value, char pad)
{
    if (value < 10U)
        put_bytes(writer, &pad, 1U);
    put_unsigned(writer, value, 1U, '0');
}

static int iso_week_of_year(const struct tm *value, int64_t year,
                            int64_t *week_year)
{
    int weekday_monday = (value->tm_wday + 6) % 7;
    int week = (value->tm_yday + 10 - weekday_monday) / 7;
    int64_t result_year = year;
    if (week < 1) {
        result_year--;
        week = weeks_in_iso_year(result_year);
    } else if (week > weeks_in_iso_year(result_year)) {
        result_year++;
        week = 1;
    }
    if (week_year)
        *week_year = result_year;
    return week;
}

static void put_iso_week(strftime_writer_t *writer, const struct tm *value,
                         int64_t year, bool week_year)
{
    int64_t result_year;
    int week = iso_week_of_year(value, year, &result_year);
    if (week_year)
        put_year(writer, result_year);
    else
        put_two(writer, (unsigned)week, '0');
}

static void put_format(strftime_writer_t *writer, const struct tm *value,
                       const char *format)
{
    for (size_t i = 0; format[i] && !writer->failed; i++) {
        char conversion;
        char modifier = 0;
        int64_t year = (int64_t)value->tm_year + 1900;
        if (format[i] != '%') {
            put_bytes(writer, format + i, 1U);
            continue;
        }
        conversion = format[++i];
        if (!conversion) {
            writer->failed = true;
            break;
        }
        if (conversion == 'E' || conversion == 'O') {
            modifier = conversion;
            conversion = format[++i];
            if (!conversion) {
                writer->failed = true;
                break;
            }
        }
        switch (conversion) {
        case '%': put_bytes(writer, "%", 1U); break;
        case 'a': put_text(writer, weekday_short[value->tm_wday]); break;
        case 'A': put_text(writer, weekday_long[value->tm_wday]); break;
        case 'b': case 'h': put_text(writer, month_short[value->tm_mon]); break;
        case 'B': put_text(writer, month_long[value->tm_mon]); break;
        case 'c':
            put_text(writer, weekday_short[value->tm_wday]);
            put_bytes(writer, " ", 1U);
            put_text(writer, month_short[value->tm_mon]);
            put_bytes(writer, " ", 1U);
            put_two(writer, (unsigned)value->tm_mday, ' ');
            put_bytes(writer, " ", 1U);
            put_two(writer, (unsigned)value->tm_hour, '0');
            put_bytes(writer, ":", 1U);
            put_two(writer, (unsigned)value->tm_min, '0');
            put_bytes(writer, ":", 1U);
            put_two(writer, (unsigned)value->tm_sec, '0');
            put_bytes(writer, " ", 1U);
            put_year(writer, year);
            break;
        case 'C': put_signed(writer, year / 100, 2U, '0'); break;
        case 'd': put_two(writer, (unsigned)value->tm_mday, '0'); break;
        case 'D':
            put_two(writer, (unsigned)value->tm_mon + 1U, '0');
            put_bytes(writer, "/", 1U);
            put_two(writer, (unsigned)value->tm_mday, '0');
            put_bytes(writer, "/", 1U);
            put_two(writer, (unsigned)floor_mod(year, 100), '0');
            break;
        case 'e': put_two(writer, (unsigned)value->tm_mday, ' '); break;
        case 'F':
            put_year(writer, year);
            put_bytes(writer, "-", 1U);
            put_two(writer, (unsigned)value->tm_mon + 1U, '0');
            put_bytes(writer, "-", 1U);
            put_two(writer, (unsigned)value->tm_mday, '0');
            break;
        case 'g': {
            int64_t week_year;
            (void)iso_week_of_year(value, year, &week_year);
            put_two(writer, (unsigned)floor_mod(week_year, 100), '0');
            break;
        }
        case 'G': put_iso_week(writer, value, year, true); break;
        case 'H': put_two(writer, (unsigned)value->tm_hour, '0'); break;
        case 'I': {
            unsigned hour = (unsigned)value->tm_hour % 12U;
            put_two(writer, hour ? hour : 12U, '0');
            break;
        }
        case 'j': put_unsigned(writer, (uint64_t)value->tm_yday + 1U, 3U, '0'); break;
        case 'm': put_two(writer, (unsigned)value->tm_mon + 1U, '0'); break;
        case 'M': put_two(writer, (unsigned)value->tm_min, '0'); break;
        case 'n': put_bytes(writer, "\n", 1U); break;
        case 'p': put_text(writer, value->tm_hour < 12 ? "AM" : "PM"); break;
        case 'r':
            put_two(writer, (unsigned)value->tm_hour % 12U ?
                     (unsigned)value->tm_hour % 12U : 12U, '0');
            put_bytes(writer, ":", 1U);
            put_two(writer, (unsigned)value->tm_min, '0');
            put_bytes(writer, ":", 1U);
            put_two(writer, (unsigned)value->tm_sec, '0');
            put_bytes(writer, " ", 1U);
            put_text(writer, value->tm_hour < 12 ? "AM" : "PM");
            break;
        case 'R':
            put_two(writer, (unsigned)value->tm_hour, '0');
            put_bytes(writer, ":", 1U);
            put_two(writer, (unsigned)value->tm_min, '0');
            break;
        case 'S': put_two(writer, (unsigned)value->tm_sec, '0'); break;
        case 't': put_bytes(writer, "\t", 1U); break;
        case 'T':
            put_two(writer, (unsigned)value->tm_hour, '0');
            put_bytes(writer, ":", 1U);
            put_two(writer, (unsigned)value->tm_min, '0');
            put_bytes(writer, ":", 1U);
            put_two(writer, (unsigned)value->tm_sec, '0');
            break;
        case 'u': put_unsigned(writer, (uint64_t)((value->tm_wday + 6) % 7) + 1U, 1U, '0'); break;
        case 'U': put_two(writer, (unsigned)(value->tm_yday + 7 - value->tm_wday) / 7U, '0'); break;
        case 'V': put_iso_week(writer, value, year, false); break;
        case 'w': put_unsigned(writer, (unsigned)value->tm_wday, 1U, '0'); break;
        case 'W': put_two(writer, (unsigned)(value->tm_yday + 7 -
                     ((value->tm_wday + 6) % 7)) / 7U, '0'); break;
        case 'x':
            put_two(writer, (unsigned)value->tm_mon + 1U, '0');
            put_bytes(writer, "/", 1U);
            put_two(writer, (unsigned)value->tm_mday, '0');
            put_bytes(writer, "/", 1U);
            put_two(writer, (unsigned)floor_mod(year, 100), '0');
            break;
        case 'X':
            put_two(writer, (unsigned)value->tm_hour, '0');
            put_bytes(writer, ":", 1U);
            put_two(writer, (unsigned)value->tm_min, '0');
            put_bytes(writer, ":", 1U);
            put_two(writer, (unsigned)value->tm_sec, '0');
            break;
        case 'y': put_two(writer, (unsigned)floor_mod(year, 100), '0'); break;
        case 'Y': put_year(writer, year); break;
        case 'z': put_text(writer, "+0000"); break;
        case 'Z': put_text(writer, "UTC"); break;
        default: writer->failed = true; break;
        }
        if (modifier) {
            bool allowed = modifier == 'E'
                ? (conversion == 'c' || conversion == 'C' || conversion == 'x' ||
                   conversion == 'X' || conversion == 'y' || conversion == 'Y')
                : (conversion == 'd' || conversion == 'e' || conversion == 'H' ||
                   conversion == 'I' || conversion == 'm' || conversion == 'M' ||
                   conversion == 'S' || conversion == 'u' || conversion == 'U' ||
                   conversion == 'V' || conversion == 'w' || conversion == 'W' ||
                   conversion == 'y');
            if (!allowed) writer->failed = true;
        }
    }
}

size_t strftime(char *restrict s, size_t maxsize,
                const char *restrict format,
                const struct tm *restrict timeptr)
{
    if (!s || !format || !valid_tm(timeptr)) {
        errno = EINVAL;
        return 0;
    }
    if (!maxsize)
        return 0;
    strftime_writer_t writer = { s, maxsize, 0, false };
    put_format(&writer, timeptr, format);
    if (writer.failed) {
        s[0] = '\0';
        errno = ERANGE;
        return 0;
    }
    s[writer.length] = '\0';
    return writer.length;
}
