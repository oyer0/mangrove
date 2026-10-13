/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <timekeeping.h>

#include <entropy.h>
#include <rtc.h>
#include <timer.h>
#include <spinlock.h>
#if defined(RTC_DEBUG)
#include <kprint.h>
#endif

#define TIME_I64_MAX ((i64)0x7fffffffffffffffLL)

static mg_mangrove_time_t realtime_sync;
static u64 realtime_sync_monotonic_ms;
static bool realtime_available;
static u64 boot_id;
static spinlock_t clock_lock;

static u64 timekeeping_read_tsc(void)
{
    u32 low;
    u32 high;

    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((u64)high << 32) | low;
}

static u64 timekeeping_boot_identifier(const rtc_calendar_t *calendar)
{
    u64 identifier;
    u64 entropy = 0;

    identifier = timekeeping_read_tsc();
    identifier ^= timekeeping_read_tsc() << 17;
    identifier ^= timer_uptime_ms() * 0x9e3779b97f4a7c15ULL;
    if (calendar) {
        identifier ^= (u64)(u32)calendar->year * 0x100000001b3ULL;
        identifier ^= (u64)calendar->month << 48;
        identifier ^= (u64)calendar->day << 40;
        identifier ^= (u64)calendar->hour << 32;
        identifier ^= (u64)calendar->minute << 24;
        identifier ^= (u64)calendar->second << 16;
    }
    /* RDRAND/RDSEED makes repeated deterministic emulator boots extremely
     * unlikely to reuse a retained boot identifier.  The timing/RTC mix is
     * retained as a non-cryptographic fallback for older x86 machines. */
    if (entropy_random_u64(&entropy)) identifier ^= entropy;
    identifier ^= identifier >> 30;
    identifier *= 0xbf58476d1ce4e5b9ULL;
    identifier ^= identifier >> 27;
    identifier *= 0x94d049bb133111ebULL;
    identifier ^= identifier >> 31;
    return identifier ? identifier : 1U;
}

void timekeeping_init(bool rtc_utc_trusted)
{
    rtc_calendar_t calendar;

    mg_mangrove_time_t sync = {0};
    bool available = false;

    if (rtc_utc_trusted && rtc_read_calendar(&calendar)) {
        mg_calendar_time_t public_calendar = {
            calendar.year, calendar.month, calendar.day,
            calendar.hour, calendar.minute, calendar.second, 0
        };
        /* The kernel performs the same bounded calendar conversion as libc;
         * this keeps the public representation canonical without a second
         * epoch implementation in the RTC driver. */
        mg_result_t result = mangrove_time_from_calendar(
            &public_calendar, &sync);
#if defined(RTC_DEBUG)
        kprint("[RTC] calendar=%d-%02u-%02u %02u:%02u:%02u convert=%lld\n",
               public_calendar.year, public_calendar.month,
               public_calendar.day, public_calendar.hour,
               public_calendar.minute, public_calendar.second,
               (long long)result);
#endif
        if (result == MG_OK)
            available = true;
    }
    /* The identifier is generated once by the kernel, not by logd, so a
     * daemon restart cannot split one boot into multiple logical boots.  It
     * is boot-local rather than a globally persistent UUID. */
    u64 anchor = timer_uptime_ms();
    u64 identifier = timekeeping_boot_identifier(available ? &calendar :
                                                 (const rtc_calendar_t *)0);
    u64 flags = spin_lock_irqsave(&clock_lock);
    realtime_sync = sync;
    realtime_sync_monotonic_ms = anchor;
    realtime_available = available;
    boot_id = identifier;
    spin_unlock_irqrestore(&clock_lock, flags);
}

u64 timekeeping_monotonic_ms(void)
{
    return timer_uptime_ms();
}

bool timekeeping_realtime_sample(timekeeping_realtime_sample_t *out)
{
    mg_mangrove_time_t sync;
    u64 anchor;
    u64 now;
    bool available;
    if (!out) return false;

    /* Snapshot both the synchronization point and its monotonic observation.
     * No RTC I/O or calendar conversion occurs under this short lock. */
    u64 flags = spin_lock_irqsave(&clock_lock);
    sync = realtime_sync;
    anchor = realtime_sync_monotonic_ms;
    available = realtime_available;
    now = timer_uptime_ms();
    spin_unlock_irqrestore(&clock_lock, flags);
    if (!available || now < anchor) return false;

    u64 elapsed_seconds = (now - anchor) / 1000U;
    if (elapsed_seconds > (u64)TIME_I64_MAX ||
        sync.seconds > TIME_I64_MAX - (i64)elapsed_seconds)
        return false;
    /* A CMOS sample has whole-second precision. Timer advancement does not
     * establish its fractional phase or justify subsecond realtime output. */
    *out = (timekeeping_realtime_sample_t){
        .time = {sync.seconds + (i64)elapsed_seconds, 0, 0},
        .resolution_ns = MG_TIME_NANOSECONDS_PER_SEC
    };
    return true;
}

bool timekeeping_realtime(mg_mangrove_time_t *out)
{
    timekeeping_realtime_sample_t sample;
    if (!out || !timekeeping_realtime_sample(&sample)) return false;
    *out = sample.time;
    return true;
}

bool timekeeping_realtime_available(void)
{
    timekeeping_realtime_sample_t sample;
    return timekeeping_realtime_sample(&sample);
}

u64 timekeeping_boot_id(void)
{
    u64 flags = spin_lock_irqsave(&clock_lock);
    u64 identifier = boot_id;
    spin_unlock_irqrestore(&clock_lock, flags);
    return identifier;
}
