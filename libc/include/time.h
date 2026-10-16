/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Mangrove's C time_t uses signed Unix seconds on its x86-64 LP64 ABI. */
typedef __INT64_TYPE__ time_t;
typedef __INT64_TYPE__ clock_t;

#define CLOCKS_PER_SEC 1000L
#define TIME_UTC 1

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

clock_t clock(void);
double difftime(time_t time1, time_t time0);
time_t mktime(struct tm *timeptr);
time_t time(time_t *timer);
int timespec_get(struct timespec *ts, int base);

char *asctime(const struct tm *timeptr);
char *ctime(const time_t *timer);
struct tm *gmtime(const time_t *timer);
struct tm *localtime(const time_t *timer);
size_t strftime(char *restrict s, size_t maxsize,
                const char *restrict format,
                const struct tm *restrict timeptr);

/* Reentrant conversion interfaces are Mangrove library extensions.  The
 * local-time variants currently use UTC because no zone database is present. */
char *asctime_r(const struct tm *restrict timeptr, char *restrict buffer);
char *ctime_r(const time_t *restrict timer, char *restrict buffer);
struct tm *gmtime_r(const time_t *restrict timer, struct tm *restrict result);
struct tm *localtime_r(const time_t *restrict timer,
                       struct tm *restrict result);
