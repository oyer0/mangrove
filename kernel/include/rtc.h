/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

typedef struct PACKED {
    i32 year;
    u8 month;
    u8 day;
    u8 hour;
    u8 minute;
    u8 second;
} rtc_calendar_t;

typedef struct PACKED {
    u8 seconds;
    u8 minutes;
    u8 hours;
    u8 day;
    u8 month;
    u8 year;
    u8 century;
    u8 status_b;
    bool century_valid;
} rtc_register_sample_t;

typedef enum rtc_calendar_result {
    RTC_CALENDAR_UNAVAILABLE = 0,
    RTC_CALENDAR_INVALID = 1,
    RTC_CALENDAR_VALID = 2,
} rtc_calendar_result_t;

/* Pure decoding/validation helper, also used by host-side deterministic
 * tests.  The input is a complete stable CMOS sample. */
bool rtc_decode_sample(const rtc_register_sample_t *sample,
                       rtc_calendar_t *out);

/* Read and validate a complete calendar from the standard x86 CMOS RTC.
 * CMOS does not identify a timezone; UTC trust belongs to timekeeping.
 * This operation is bounded and never enables RTC periodic interrupts. */
bool rtc_read_calendar(rtc_calendar_t *out);
/* Distinguishes a stable but malformed calendar from a missing/unstable RTC
 * sample. The legacy bool interface remains available to existing callers. */
rtc_calendar_result_t rtc_read_calendar_status(rtc_calendar_t *out);
