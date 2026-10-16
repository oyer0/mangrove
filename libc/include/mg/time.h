/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/types.h>

/* Mangrove's native civil-time epoch is UTC 2000-01-01 00:00:00.  These
 * types are deliberately distinct from Unix timestamps, whose epoch is
 * UTC 1970-01-01 00:00:00. */
#define MG_MANGROVE_EPOCH_YEAR       2000
#define MG_TIME_NANOSECONDS_PER_SEC  1000000000U

typedef struct PACKED {
    /* Elapsed milliseconds from the current boot-local monotonic origin. */
    u64 milliseconds;
} mg_monotonic_time_t;

typedef struct PACKED {
    /* Scheduler-accounted process CPU time in nominal 1 ms ticks. */
    u64 ticks;
} mg_process_cpu_time_t;

typedef struct PACKED {
    /* Signed seconds from the Mangrove epoch, plus a canonical fraction. */
    i64 seconds;
    u32 nanoseconds;
    u32 reserved;
} mg_mangrove_time_t;

typedef struct PACKED {
    /* Explicit interoperability type; this is not the kernel's clock type. */
    i64 seconds;
    u32 nanoseconds;
    u32 reserved;
} mg_unix_time_t;

typedef struct PACKED {
    i32 year;
    u8 month;
    u8 day;
    u8 hour;
    u8 minute;
    u8 second;
    u32 nanoseconds;
} mg_calendar_time_t;

/* Read-only clock APIs.  Realtime returns MG_ERR_TIME_UNAVAILABLE when the
 * deployment has not established RTC-as-UTC trust, or no validated realtime
 * is available. Current CMOS-backed realtime has whole-second resolution. */
mg_result_t mg_clock_monotonic(mg_monotonic_time_t *out);
mg_result_t mg_clock_realtime(mg_mangrove_time_t *out);
/* CPU ticks accumulated for the calling process since process creation. */
mg_result_t mg_clock_process_cpu(mg_process_cpu_time_t *out);
/* Stable identifier for the current kernel boot, used to group records. */
u64 mg_clock_boot_id(void);

mg_result_t mangrove_time_from_calendar(const mg_calendar_time_t *calendar,
                                        mg_mangrove_time_t *out);
mg_result_t mangrove_time_to_calendar(const mg_mangrove_time_t *time,
                                      mg_calendar_time_t *out);
mg_result_t mangrove_time_to_unix(const mg_mangrove_time_t *time,
                                  mg_unix_time_t *out);
mg_result_t unix_time_to_mangrove(const mg_unix_time_t *time,
                                  mg_mangrove_time_t *out);
