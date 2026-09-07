#include <timekeeping.h>

#include <entropy.h>
#include <rtc.h>
#include <timer.h>
#if defined(RTC_DEBUG)
#include <kprint.h>
#endif

#define TIME_I64_MAX ((i64)0x7fffffffffffffffLL)

static mg_mangrove_time_t realtime_sync;
static u64 realtime_sync_monotonic_ms;
static bool realtime_available;
static u64 boot_id;

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

void timekeeping_init(void)
{
    rtc_calendar_t calendar;

    realtime_available = false;
    realtime_sync = (mg_mangrove_time_t){0};
    realtime_sync_monotonic_ms = timer_uptime_ms();
    if (rtc_read_calendar(&calendar)) {
        mg_calendar_time_t public_calendar = {
            calendar.year, calendar.month, calendar.day,
            calendar.hour, calendar.minute, calendar.second, 0
        };
        /* The kernel performs the same bounded calendar conversion as libc;
         * this keeps the public representation canonical without a second
         * epoch implementation in the RTC driver. */
        mg_result_t result = mangrove_time_from_calendar(
            &public_calendar, &realtime_sync);
#if defined(RTC_DEBUG)
        kprint("[RTC] calendar=%d-%02u-%02u %02u:%02u:%02u convert=%lld\n",
               public_calendar.year, public_calendar.month,
               public_calendar.day, public_calendar.hour,
               public_calendar.minute, public_calendar.second,
               (long long)result);
#endif
        if (result == MG_OK)
            realtime_available = true;
    }
    /* The identifier is generated once by the kernel, not by logd, so a
     * daemon restart cannot split one boot into multiple logical boots.  It
     * is boot-local rather than a globally persistent UUID. */
    boot_id = timekeeping_boot_identifier(realtime_available ? &calendar :
                                           (const rtc_calendar_t *)0);
}

u64 timekeeping_monotonic_ms(void)
{
    return timer_uptime_ms();
}

bool timekeeping_realtime(mg_mangrove_time_t *out)
{
    u64 elapsed_ms;
    u64 elapsed_seconds;
    u32 elapsed_nanoseconds;
    i64 seconds;
    u32 nanoseconds;

    if (!out || !realtime_available)
        return false;

    elapsed_ms = timer_uptime_ms() - realtime_sync_monotonic_ms;
    elapsed_seconds = elapsed_ms / 1000U;
    elapsed_nanoseconds = (u32)((elapsed_ms % 1000U) * 1000000U);
    if (elapsed_seconds > (u64)TIME_I64_MAX)
        return false;
    if (realtime_sync.seconds > TIME_I64_MAX - (i64)elapsed_seconds)
        return false;

    seconds = realtime_sync.seconds + (i64)elapsed_seconds;
    nanoseconds = realtime_sync.nanoseconds + elapsed_nanoseconds;
    if (nanoseconds >= MG_TIME_NANOSECONDS_PER_SEC) {
        nanoseconds -= MG_TIME_NANOSECONDS_PER_SEC;
        if (seconds == TIME_I64_MAX)
            return false;
        seconds++;
    }
    out->seconds = seconds;
    out->nanoseconds = nanoseconds;
    out->reserved = 0;
    return true;
}

bool timekeeping_realtime_available(void)
{
    return realtime_available;
}

u64 timekeeping_boot_id(void)
{
    return boot_id;
}
