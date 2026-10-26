/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production clock and RTC decoder; hardware sampling, ticks, entropy and
 * IRQ-safe lock operations are controlled host dependencies. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "../../kernel/src/timekeeping.c"

static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 uptime;
static unsigned rtc_reads;
static rtc_calendar_result_t rtc_status = RTC_CALENDAR_VALID;
static rtc_register_sample_t registers = {
    59, 58, 23, 29, 2, 24, 20, 0x06, true
};

u64 spin_lock_irqsave(spinlock_t *lock)
{
    assert(lock == &clock_lock);
    assert(pthread_mutex_lock(&host_lock) == 0);
    return 0;
}
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    assert(lock == &clock_lock && flags == 0);
    assert(pthread_mutex_unlock(&host_lock) == 0);
}
u64 timer_uptime_ms(void) { return __atomic_load_n(&uptime, __ATOMIC_RELAXED); }
bool entropy_random_u64(u64 *out) { (void)out; return false; }
bool rtc_read_calendar(rtc_calendar_t *out)
{
    rtc_reads++;
    return rtc_status == RTC_CALENDAR_VALID && rtc_decode_sample(&registers, out);
}
rtc_calendar_result_t rtc_read_calendar_status(rtc_calendar_t *out)
{
    rtc_reads++;
    if (!out) return RTC_CALENDAR_INVALID;
    if (rtc_status != RTC_CALENDAR_VALID) return rtc_status;
    return rtc_decode_sample(&registers, out)
        ? RTC_CALENDAR_VALID : RTC_CALENDAR_INVALID;
}
static void *reader(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 10000; i++) {
        timekeeping_realtime_sample_t sample;
        assert(timekeeping_realtime_sample(&sample));
        assert(sample.time.seconds >= 762566339LL &&
               sample.time.seconds <= 762566439LL);
        assert(sample.time.nanoseconds == 0 && sample.time.reserved == 0 &&
               sample.resolution_ns == 1000000000U);
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
    assert(!timekeeping_realtime(&output) && output.seconds == 17 &&
           output.nanoseconds == 23 && output.reserved == 31);
    assert(!timekeeping_realtime(NULL));
    rtc_status = RTC_CALENDAR_UNAVAILABLE;
    timekeeping_init(true);
    assert(!timekeeping_realtime_available());
    assert(timekeeping_realtime_info(&info) &&
           info.state == MG_REALTIME_UNAVAILABLE &&
           info.time.seconds == 0 && info.resolution_ns == 0);
    rtc_status = RTC_CALENDAR_VALID;
    registers.day = 30; /* Invalid leap-February day. */
    timekeeping_init(true);
    assert(!timekeeping_realtime_available());
    assert(timekeeping_realtime_info(&info) &&
           info.state == MG_REALTIME_INVALID &&
           info.time.seconds == 0 && info.resolution_ns == 0);
    registers.day = 29;
    uptime = 123;
    timekeeping_init(true);
    assert(timekeeping_realtime(&output) && output.seconds == 762566339LL);
    assert(timekeeping_realtime_info(&info) &&
           info.state == MG_REALTIME_TRUSTED &&
           info.time.seconds == 762566339LL &&
           info.resolution_ns == MG_TIME_NANOSECONDS_PER_SEC);
    u64 identifier = timekeeping_boot_id();
    uptime = 1122;
    assert(timekeeping_realtime(&output) && output.seconds == 762566339LL &&
           output.nanoseconds == 0);
    uptime = 1123;
    assert(timekeeping_realtime(&output) && output.seconds == 762566340LL);
    assert(identifier == timekeeping_boot_id());
    uptime = 122;
    assert(!timekeeping_realtime_available()); /* Backwards anchor rejected. */
    assert(timekeeping_realtime_info(&info) &&
           info.state == MG_REALTIME_INVALID &&
           info.time.seconds == 0 && info.resolution_ns == 0);
    uptime = 123;
    pthread_t readers[4];
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&readers[i], NULL, reader, NULL));
    for (u64 i = 123; i < 100123; i++) __atomic_store_n(&uptime, i, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(readers[i], NULL));
    /* Exercise arithmetic protection without inventing an RTC year beyond
     * the actual decoder's validated range. */
    realtime_sync.seconds = TIME_I64_MAX;
    uptime = realtime_sync_monotonic_ms + 1000;
    output = (mg_mangrove_time_t){17, 23, 31};
    assert(!timekeeping_realtime(&output) && output.seconds == 17);
    puts("system clock: UTC trust, unavailable/invalid samples, whole seconds, concurrent reads and overflow passed");
    return 0;
}
