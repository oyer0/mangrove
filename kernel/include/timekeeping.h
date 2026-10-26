/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/time.h>

/* Deployment assertion, not RTC detection: CMOS contains no timezone tag.
 * Call once during boot. Without UTC trust, realtime remains unavailable. */
void timekeeping_init(bool rtc_utc_trusted);

typedef struct {
    mg_mangrove_time_t time;
    u32 resolution_ns;
} timekeeping_realtime_sample_t;

/* Coherent validated UTC time and source resolution. Output is unchanged on
 * failure. Resolution describes precision, not accuracy or synchronization. */
bool timekeeping_realtime_sample(timekeeping_realtime_sample_t *out);
/* Writes a coherent public status and timestamp; time fields are zero unless
 * status is MG_REALTIME_TRUSTED. */
bool timekeeping_realtime_info(mg_realtime_info_t *out);
u64 timekeeping_monotonic_ms(void);
bool timekeeping_realtime(mg_mangrove_time_t *out);
bool timekeeping_realtime_available(void);
u64 timekeeping_boot_id(void);
