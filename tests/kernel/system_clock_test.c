/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production clock policy and RTC decoder with controlled hardware sampling. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "../../kernel/src/timekeeping.c"

static pthread_mutex_t host_clock_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t host_rtc_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 uptime;
static unsigned rtc_reads;
static rtc_calendar_result_t rtc_status = RTC_CALENDAR_VALID;
static rtc_register_sample_t registers = {
    59, 58, 23, 29, 2, 24, 20, 0x06, true
};

u64 spin_lock_irqsave(spinlock_t *lock)
{
    assert(lock == &clock_lock);
    assert(pthread_mutex_lock(&host_clock_lock) == 0);
    return 0;
}
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    assert(lock == &clock_lock && flags == 0);
    assert(pthread_mutex_unlock(&host_clock_lock) == 0);
}
u64 timer_uptime_ms(void) { return __atomic_load_n(&uptime, __ATOMIC_RELAXED); }
bool entropy_random_u64(u64 *out) { (void)out; return false; }
bool rtc_read_calendar(rtc_calendar_t *out)
{
    return rtc_read_calendar_status(out) == RTC_CALENDAR_VALID;
}
rtc_calendar_result_t rtc_read_calendar_status(rtc_calendar_t *out)
{
    rtc_calendar_result_t result;
    pthread_mutex_lock(&host_rtc_lock);
    __atomic_add_fetch(&rtc_reads, 1U, __ATOMIC_RELAXED);
    if (!out)
        result = RTC_CALENDAR_INVALID;
    else if (rtc_status != RTC_CALENDAR_VALID)
        result = rtc_status;
    else
        result = rtc_decode_sample(&registers, out)
            ? RTC_CALENDAR_VALID : RTC_CALENDAR_INVALID;
    pthread_mutex_unlock(&host_rtc_lock);
    return result;
}

static void *reader(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 10000; i++) {
        mg_realtime_info_t info;
        assert(timekeeping_realtime_info(&info));
        assert(info.state == MG_REALTIME_TRUSTED);
        assert(info.time.seconds == 762566280LL);
        assert(info.time.nanoseconds == 0 && info.time.reserved == 0 &&
               info.resolution_ns == MG_TIME_NANOSECONDS_PER_SEC);
        assert(timekeeping_boot_id());
    }
    return NULL;
}

int main(void)
{
    mg_mangrove_time_t output = {17, 23, 31};
    mg_realtime_info_t info = {99, {17, 23, 31}, 41, 43};
    timekeeping_init(false);
    assert(!rtc_reads && !timekeeping_realtime_available());
    assert(timekeeping_realtime_info(&info) &&
           info.state == MG_REALTIME_UNTRUSTED &&
           info.time.seconds == 0 && info.time.nanoseconds == 0 &&
           info.resolution_ns == 0 && info.reserved == 0);
    assert(!rtc_reads);
    assert(!timekeeping_realtime(&output) && output.seconds == 17 &&
           output.nanoseconds == 23 && output.reserved == 31);
    assert(!timekeeping_realtime(NULL));

    rtc_status = RTC_CALENDAR_UNAVAILABLE;
    timekeeping_init(true);
    assert(!timekeeping_realtime_available());
    assert(timekeeping_realtime_info(&info) &&
           info.state == MG_REALTIME_UNAVAILABLE &&
           info.time.seconds == 0 && info.resolution_ns == 0);

    /* Once the deployment establishes UTC trust, each query samples the RTC
     * again. A temporarily unavailable or invalid sample fails closed, while
     * recovery does not require a kernel reboot or stale monotonic anchor. */
    rtc_status = RTC_CALENDAR_VALID;
    registers.day = 31; /* Invalid leap-February day. */
    timekeeping_init(true);
    assert(!timekeeping_realtime_available());
    assert(timekeeping_realtime_info(&info) &&
           info.state == MG_REALTIME_INVALID && info.time.seconds == 0);
    registers.day = 29;
    uptime = 123;
    timekeeping_init(true);
    assert(timekeeping_realtime(&output) && output.seconds == 762566339LL);
    assert(timekeeping_realtime_info(&info) &&
           info.state == MG_REALTIME_TRUSTED &&
           info.time.seconds == 762566339LL &&
           info.resolution_ns == MG_TIME_NANOSECONDS_PER_SEC);
    u64 identifier = timekeeping_boot_id();

    /* Elapsed uptime alone does not advance civil time. A fresh source sample
     * does; valid backward corrections are exposed rather than hidden behind
     * a monotonic wall-clock clamp. */
    uptime = 1123;
    assert(timekeeping_realtime(&output) && output.seconds == 762566339LL);
    registers = (rtc_register_sample_t){0, 0, 0, 1, 3, 24, 20, 0x06, true};
    assert(timekeeping_realtime(&output) && output.seconds == 762566400LL);
    registers = (rtc_register_sample_t){0, 58, 23, 29, 2, 24, 20, 0x06, true};
    assert(timekeeping_realtime(&output) && output.seconds == 762566280LL);
    assert(identifier == timekeeping_boot_id());

    pthread_t readers[4];
    for (unsigned i = 0; i < 4; i++)
        assert(!pthread_create(&readers[i], NULL, reader, NULL));
    for (unsigned i = 0; i < 4; i++)
        assert(!pthread_join(readers[i], NULL));

    rtc_status = RTC_CALENDAR_UNAVAILABLE;
    assert(!timekeeping_realtime_available());
    rtc_status = RTC_CALENDAR_VALID;
    assert(timekeeping_realtime_available());
    printf("system clock: UTC trust, fresh RTC reads, fail-closed states, wall corrections and concurrent samples passed (%u RTC reads)\n",
           rtc_reads);
    return 0;
}
