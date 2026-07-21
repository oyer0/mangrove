#pragma once

#include <types.h>

#define TIMER_FREQUENCY 1000

void timer_init (void);

u64 timer_ticks(void);
u64 timer_uptime_ms(void);

void timer_sleep(u64 ms);
void timer_delay(u64 ms);
