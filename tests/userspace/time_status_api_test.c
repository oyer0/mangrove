/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <mg/time.h>

static mg_realtime_state_t next_state = MG_REALTIME_UNTRUSTED;
static unsigned long last_syscall;

long mg_syscall(unsigned long number, unsigned long arg0,
                unsigned long arg1, unsigned long arg2)
{
    assert(arg1 == 0 && arg2 == 0);
    last_syscall = number;
    if (number == 102) {
        mg_realtime_info_t *out = (mg_realtime_info_t *)(uintptr_t)arg0;
        *out = (mg_realtime_info_t){
            .state = (u32)next_state,
            .time = next_state == MG_REALTIME_TRUSTED
                ? (mg_mangrove_time_t){838123456, 0, 0}
                : (mg_mangrove_time_t){0, 0, 0},
            .resolution_ns = next_state == MG_REALTIME_TRUSTED
                ? MG_TIME_NANOSECONDS_PER_SEC : 0,
            .reserved = 0,
        };
    } else if (number == 104) {
        mg_monotonic_info_t *out =
            (mg_monotonic_info_t *)(uintptr_t)arg0;
        *out = (mg_monotonic_info_t){
            .time = {123456789},
            .source = MG_CLOCKSOURCE_HPET,
            .quality = MG_CLOCK_QUALITY_HARDWARE,
            .resolution_ns = 1000000,
        };
    }
    return MG_OK;
}

#include "../../libc/src/time.c"

int main(void)
{
    _Static_assert(sizeof(mg_realtime_info_t) == 28,
                   "realtime info ABI size changed");
    _Static_assert(sizeof(mg_monotonic_info_t) == 24,
                   "monotonic info ABI size changed");
    mg_realtime_info_t info;
    for (next_state = MG_REALTIME_UNTRUSTED;
         next_state <= MG_REALTIME_TRUSTED; next_state++) {
        info = (mg_realtime_info_t){0};
        assert(mg_clock_realtime_info(&info) == MG_OK);
        assert(last_syscall == 102 && info.state == (u32)next_state);
        if (next_state == MG_REALTIME_TRUSTED) {
            assert(info.time.seconds == 838123456 &&
                   info.resolution_ns == MG_TIME_NANOSECONDS_PER_SEC);
        } else {
            assert(info.time.seconds == 0 && info.time.nanoseconds == 0 &&
                   info.resolution_ns == 0);
        }
    }
    assert(mg_clock_realtime_info((mg_realtime_info_t *)0) ==
           MG_ERR_BAD_ARGUMENT);
    mg_monotonic_info_t monotonic = {0};
    assert(mg_clock_monotonic_info(&monotonic) == MG_OK &&
           last_syscall == 104 && monotonic.time.milliseconds == 123456789 &&
           monotonic.source == MG_CLOCKSOURCE_HPET &&
           monotonic.quality == MG_CLOCK_QUALITY_HARDWARE &&
           monotonic.resolution_ns == 1000000);
    assert(mg_clock_monotonic_info((mg_monotonic_info_t *)0) ==
           MG_ERR_BAD_ARGUMENT);
    mg_mangrove_time_t time = {0};
    assert(mg_clock_realtime(&time) == MG_OK && last_syscall == 62);
    return 0;
}
