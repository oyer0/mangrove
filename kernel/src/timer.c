#include <timer.h>

static volatile u64 ticks = 0;

u64 timer_ticks(void) {
    return ticks;
}

void timer_init(void) {}
u64 timer_uptime_ms(void) { return 0; }
void timer_sleep(u64 ms) { (void)ms; }
void timer_delay(u64 ms) { (void)ms; }
