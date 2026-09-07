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

/* Pure decoding/validation helper, also used by host-side deterministic
 * tests.  The input is a complete stable CMOS sample. */
bool rtc_decode_sample(const rtc_register_sample_t *sample,
                       rtc_calendar_t *out);

/* Read and validate a complete UTC calendar from the standard x86 CMOS RTC.
 * This operation is bounded and never enables RTC periodic interrupts. */
bool rtc_read_calendar(rtc_calendar_t *out);
