/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>

int fstest_time_phase(void)
{
    mg_mangrove_time_t native;
    mg_mangrove_time_t later_native;
    mg_unix_time_t unix_value;
    mg_monotonic_time_t mono_before, mono_after;
    struct timespec now;
    struct tm utc, local, normalized;
    time_t value, wall_start_unix, later_time;
    clock_t cpu_before, cpu_busy, cpu_sleep;
    char formatted[64], later_formatted[64];
    u64 sleep_elapsed, wall_elapsed_ms, wall_elapsed;

    if (mg_clock_realtime(&native) != MG_OK ||
        mangrove_time_to_unix(&native, &unix_value) != MG_OK)
        return 43;
    value = time(NULL);
    if (value != unix_value.seconds ||
        timespec_get(&now, TIME_UTC) != TIME_UTC ||
        now.tv_sec != value || now.tv_nsec != native.nanoseconds ||
        !gmtime_r(&value, &utc) || !localtime_r(&value, &local) ||
        utc.tm_year != local.tm_year || utc.tm_mon != local.tm_mon ||
        utc.tm_mday != local.tm_mday || utc.tm_hour != local.tm_hour ||
        utc.tm_min != local.tm_min || utc.tm_sec != local.tm_sec ||
        utc.tm_isdst != 0)
        return 43;

    normalized = utc;
    if (mktime(&normalized) != value || normalized.tm_isdst != 0 ||
        strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M:%S %z %Z",
                 &utc) == 0)
        return 43;

    cpu_before = clock();
    if (cpu_before < 0 ||
        mg_clock_monotonic(&mono_before) != MG_OK)
        return 43;
    do {
        volatile u64 work = 0;
        for (u64 i = 0; i < 10000U; i++)
            work += i ^ (i >> 3);
        (void)work;
        if (mg_clock_monotonic(&mono_after) != MG_OK ||
            mono_after.milliseconds < mono_before.milliseconds)
            return 43;
    } while (mono_after.milliseconds - mono_before.milliseconds < 20U);
    cpu_busy = clock();
    if (cpu_busy <= cpu_before ||
        mg_clock_monotonic(&mono_before) != MG_OK)
        return 43;

    cpu_before = clock();
    if (cpu_before < 0 || process_sleep_ms(30U) != MG_OK ||
        mg_clock_monotonic(&mono_after) != MG_OK ||
        mono_after.milliseconds < mono_before.milliseconds)
        return 43;
    sleep_elapsed = mono_after.milliseconds - mono_before.milliseconds;
    cpu_sleep = clock();
    if (sleep_elapsed < 15U || cpu_sleep < cpu_before ||
        (u64)(cpu_sleep - cpu_before) >= sleep_elapsed)
        return 43;

    if (mg_clock_realtime(&native) != MG_OK ||
        mangrove_time_to_unix(&native, &unix_value) != MG_OK)
        return 43;
    wall_start_unix = unix_value.seconds;
    if (mg_clock_monotonic(&mono_before) != MG_OK ||
        process_sleep_ms(1200U) != MG_OK ||
        mg_clock_monotonic(&mono_after) != MG_OK ||
        mg_clock_realtime(&later_native) != MG_OK ||
        mangrove_time_to_unix(&later_native, &unix_value) != MG_OK)
        return 43;
    wall_elapsed_ms = mono_after.milliseconds - mono_before.milliseconds;
    if (wall_elapsed_ms < 1000U || later_native.seconds < native.seconds)
        return 43;
    wall_elapsed = (u64)(later_native.seconds - native.seconds);
    later_time = unix_value.seconds;
    if (wall_elapsed == 0U || wall_elapsed > wall_elapsed_ms / 1000U + 1U ||
        !gmtime_r(&later_time, &local) ||
        strftime(later_formatted, sizeof(later_formatted),
                 "%Y-%m-%d %H:%M:%S", &local) == 0)
        return 43;

    printf("TIME1 unix=%lld nanoseconds=%ld date=%s busy_cpu=%lld "
           "sleep_cpu=%lld sleep_elapsed_ms=%llu wall_delta_s=%llu "
           "wall_elapsed_ms=%llu wall_start_unix=%lld later_unix=%lld "
           "later_date=%s\n",
           (long long)value, now.tv_nsec, formatted,
           (long long)cpu_busy, (long long)cpu_sleep,
           (unsigned long long)sleep_elapsed,
           (unsigned long long)wall_elapsed,
           (unsigned long long)wall_elapsed_ms,
           (long long)wall_start_unix, (long long)unix_value.seconds,
           later_formatted);
    puts("TIME1 complete");
    return 0;
}
