/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <mg/time.h>

/* Initializes the one authoritative realtime synchronization point. */
void timekeeping_init(void);
u64 timekeeping_monotonic_ms(void);
bool timekeeping_realtime(mg_mangrove_time_t *out);
bool timekeeping_realtime_available(void);
u64 timekeeping_boot_id(void);
