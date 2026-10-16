/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _DEFAULT_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../libc/include/mangrove_errors.h"

typedef int64_t mg_result_t;
typedef struct {
    int64_t seconds;
    uint32_t nanoseconds;
    uint32_t reserved;
} mg_mangrove_time_t;
typedef struct {
    int64_t seconds;
    uint32_t nanoseconds;
    uint32_t reserved;
} mg_unix_time_t;
typedef struct { uint64_t ticks; } mg_process_cpu_time_t;

extern clock_t mg_time_clock(void);
extern double mg_time_difftime(time_t, time_t);
extern time_t mg_time_mktime(struct tm *);
extern time_t mg_time_time(time_t *);
extern int mg_time_timespec_get(struct timespec *, int);
extern char *mg_time_asctime(const struct tm *);
extern char *mg_time_asctime_r(const struct tm *, char *);
extern char *mg_time_ctime(const time_t *);
extern char *mg_time_ctime_r(const time_t *, char *);
extern struct tm *mg_time_gmtime(const time_t *);
extern struct tm *mg_time_gmtime_r(const time_t *, struct tm *);
extern struct tm *mg_time_localtime(const time_t *);
extern struct tm *mg_time_localtime_r(const time_t *, struct tm *);
extern size_t mg_time_strftime(char *, size_t, const char *,
                               const struct tm *);
extern int *probe_errno_location(void);

static bool realtime_available = true;
static mg_mangrove_time_t realtime_value;
static mg_result_t cpu_result = MG_OK;
static uint64_t cpu_ticks;
static int runtime_errno;

mg_result_t mg_clock_realtime(mg_mangrove_time_t *out)
{
    if (!realtime_available)
        return MG_ERR_TIME_UNAVAILABLE;
    *out = realtime_value;
    return MG_OK;
}

mg_result_t mg_clock_process_cpu(mg_process_cpu_time_t *out)
{
    if (cpu_result != MG_OK)
        return cpu_result;
    out->ticks = cpu_ticks;
    return MG_OK;
}

void mg_set_errno_from_result(mg_result_t result)
{
    *probe_errno_location() = result == MG_ERR_UNSUPPORTED ? ENOSYS :
        result == MG_ERR_TIME_UNAVAILABLE ? ENODATA : EIO;
}

int *probe_errno_location(void)
{
    return &runtime_errno;
}

static int same_tm(const struct tm *a, const struct tm *b)
{
    return a->tm_sec == b->tm_sec && a->tm_min == b->tm_min &&
           a->tm_hour == b->tm_hour && a->tm_mday == b->tm_mday &&
           a->tm_mon == b->tm_mon && a->tm_year == b->tm_year &&
           a->tm_wday == b->tm_wday && a->tm_yday == b->tm_yday &&
           a->tm_isdst == b->tm_isdst;
}

static void compare_breakdown(time_t value)
{
    struct tm expected, actual;
    assert(gmtime_r(&value, &expected));
    assert(mg_time_gmtime_r(&value, &actual));
    assert(same_tm(&expected, &actual));
    assert(localtime_r(&value, &expected));
    assert(mg_time_localtime_r(&value, &actual));
    assert(same_tm(&expected, &actual));
}

static void calendar_cases(void)
{
    static const time_t cases[] = {
        (time_t)0,
        (time_t)-1,
        (time_t)946684800,
        (time_t)951782400,   /* 2000-02-29 */
        (time_t)4107542400LL,/* 2100-03-01 */
        (time_t)2147483647,
        (time_t)-2208988800LL
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        compare_breakdown(cases[i]);

    time_t minus_one = (time_t)-1;
    struct tm broken;
    assert(mg_time_gmtime_r(&minus_one, &broken));
    assert(broken.tm_year == 69 && broken.tm_mon == 11 &&
           broken.tm_mday == 31 && broken.tm_hour == 23 &&
           broken.tm_min == 59 && broken.tm_sec == 59 &&
           broken.tm_wday == 3 && broken.tm_yday == 364);

    time_t out_of_range = (time_t)INT64_MAX;
    struct tm untouched = { .tm_sec = 1, .tm_min = 2, .tm_hour = 3,
        .tm_mday = 4, .tm_mon = 5, .tm_year = 6, .tm_wday = 1,
        .tm_yday = 2, .tm_isdst = 0 };
    struct tm before = untouched;
    runtime_errno = 0;
    assert(!mg_time_gmtime_r(&out_of_range, &untouched));
    assert(runtime_errno == EOVERFLOW && same_tm(&untouched, &before));

    struct tm normalized = { .tm_sec = 60, .tm_min = 59, .tm_hour = 23,
        .tm_mday = 0, .tm_mon = 12, .tm_year = 101, .tm_isdst = -1 };
    struct tm host_normalized = normalized;
    time_t expected = mktime(&host_normalized);
    time_t actual = mg_time_mktime(&normalized);
    assert(actual == expected && same_tm(&normalized, &host_normalized));
    assert(normalized.tm_year == 102 && normalized.tm_mon == 0 &&
           normalized.tm_mday == 1 && normalized.tm_hour == 0 &&
           normalized.tm_min == 0 && normalized.tm_sec == 0 &&
           normalized.tm_isdst == 0);

    struct tm leap = { .tm_sec = 0, .tm_min = 0, .tm_hour = 0,
        .tm_mday = 29, .tm_mon = 1, .tm_year = 100, .tm_isdst = 0 };
    host_normalized = leap;
    expected = mktime(&host_normalized);
    assert(mg_time_mktime(&leap) == expected);
    assert(same_tm(&leap, &host_normalized));

    struct tm invalid_sentinel = { .tm_sec = 0, .tm_min = 0, .tm_hour = 0,
        .tm_mday = 1, .tm_mon = 0, .tm_year = 70, .tm_isdst = 0 };
    (void)mg_time_mktime(&invalid_sentinel);
}

static void formatting_cases(void)
{
    static const char format[] =
        "%a|%A|%b|%B|%c|%C|%d|%D|%e|%F|%g|%G|%h|%H|%I|%j|%m|%M|%p|"
        "%r|%R|%S|%T|%u|%U|%V|%w|%W|%x|%X|%y|%Y|%z|%%";
    time_t timestamp = (time_t)1709164800; /* 2024-02-29 00:00:00 UTC */
    struct tm value;
    char host[512], actual[512];
    assert(gmtime_r(&timestamp, &value));
    size_t host_count = strftime(host, sizeof(host), format, &value);
    size_t actual_count = mg_time_strftime(actual, sizeof(actual), format,
                                           &value);
    if (host_count != actual_count || strcmp(host, actual))
        fprintf(stderr, "host (%zu): %s\nactual (%zu): %s\n",
                host_count, host, actual_count, actual);
    assert(host_count == actual_count && !strcmp(host, actual));
    assert(mg_time_strftime(actual, sizeof(actual), "%Z", &value) == 3 &&
           !strcmp(actual, "UTC"));
    assert(mg_time_strftime(actual, sizeof(actual),
           "%Ec|%EC|%Ex|%EY|%Od|%OH|%OU|%OV|%Oy", &value) ==
           strftime(host, sizeof(host),
           "%Ec|%EC|%Ex|%EY|%Od|%OH|%OU|%OV|%Oy", &value));
    assert(!strcmp(actual, host));

    /* Check week-year transitions and week numbers throughout complete
     * Gregorian years, independently against the host C locale. */
    for (time_t day = (time_t)0; day < (time_t)4133980800LL;
         day += (time_t)86400) {
        struct tm sample;
        assert(gmtime_r(&day, &sample));
        size_t reference = strftime(host, sizeof(host),
                                    "%G|%g|%V|%U|%W", &sample);
        size_t produced = mg_time_strftime(actual, sizeof(actual),
                                           "%G|%g|%V|%U|%W", &sample);
        assert(reference == produced && !strcmp(host, actual));
    }

    char asc_host[64], asc_actual[64], ctime_host[64], ctime_actual[64];
    assert(asctime_r(&value, asc_host));
    assert(mg_time_asctime_r(&value, asc_actual));
    assert(!strcmp(asc_host, asc_actual));
    assert(!strcmp(mg_time_asctime(&value), asc_host));
    assert(ctime_r(&timestamp, ctime_host));
    assert(mg_time_ctime_r(&timestamp, ctime_actual));
    assert(!strcmp(ctime_host, ctime_actual));
    assert(!strcmp(mg_time_ctime(&timestamp), ctime_host));

    memset(actual, 0x7f, sizeof(actual));
    runtime_errno = 0;
    assert(mg_time_strftime(actual, 5, "%Y-%m-%d", &value) == 0);
    assert(actual[0] == '\0' && runtime_errno == ERANGE);
    assert(mg_time_strftime(actual, sizeof(actual), "%Q", &value) == 0);
    assert(actual[0] == '\0');
}

static void clock_cases(void)
{
    time_t output = -7;
    realtime_value = (mg_mangrove_time_t){ 0, 0, 0 };
    assert(mg_time_time(&output) == (time_t)946684800);
    assert(output == (time_t)946684800);

    struct timespec ts = { -9, -9 };
    assert(mg_time_timespec_get(&ts, TIME_UTC) == TIME_UTC);
    assert(ts.tv_sec == (time_t)946684800 && ts.tv_nsec == 0);
    struct timespec preserved = ts;
    assert(mg_time_timespec_get(&ts, TIME_UTC + 1) == 0);
    assert(ts.tv_sec == preserved.tv_sec && ts.tv_nsec == preserved.tv_nsec);

    realtime_available = false;
    output = 99;
    runtime_errno = 0;
    assert(mg_time_time(&output) == (time_t)-1 && output == (time_t)-1);
    assert(runtime_errno == ENODATA);
    ts = (struct timespec){ 99, 99 };
    assert(mg_time_timespec_get(&ts, TIME_UTC) == 0);
    assert(ts.tv_sec == 99 && ts.tv_nsec == 99);
    assert(runtime_errno == ENODATA);
    realtime_available = true;

    cpu_ticks = 1234;
    assert(mg_time_clock() == (clock_t)1234);
    cpu_result = MG_ERR_UNSUPPORTED;
    runtime_errno = 0;
    assert(mg_time_clock() == (clock_t)-1 && runtime_errno == ENOSYS);
    cpu_result = MG_OK;
    cpu_ticks = (uint64_t)INT64_MAX + 1U;
    runtime_errno = 0;
    assert(mg_time_clock() == (clock_t)-1 && runtime_errno == EOVERFLOW);
    assert(mg_time_difftime((time_t)INT64_MAX, (time_t)INT64_MIN) ==
           (double)UINT64_MAX);
    assert(mg_time_difftime((time_t)-1, (time_t)1) == -2.0);
}

int main(void)
{
    (void)setenv("TZ", "UTC", 1);
    tzset();
    calendar_cases();
    formatting_cases();
    clock_cases();
    puts("C time interfaces, calendar, UTC formatting and clock failures passed");
    return 0;
}
