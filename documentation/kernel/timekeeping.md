# Mangrove timekeeping

Mangrove has two independent clocks.

- The monotonic clock is elapsed milliseconds from a boot-local origin. It is
  used for scheduler deadlines, sleeps, timeouts, and uptime. It does not
  change when realtime is corrected.
- The realtime clock is UTC and is represented by signed seconds plus
  nanoseconds from the **Mangrove Epoch**, `2000-01-01 00:00:00 UTC`. The
  timestamp `0` is therefore that instant; dates before it have negative
  seconds.

Pith can read and validate the x86 CMOS RTC once during boot and record that
calendar alongside a monotonic synchronization point. CMOS contains no timezone
provenance. Realtime is **unavailable by default**: a deployment must explicitly
establish that its RTC contains UTC and build with `RTC_UTC_TRUST=1`. Physical
hardware must not inherit that assertion merely because QEMU uses UTC. An
untrusted, invalid, or unavailable RTC does not prevent boot or affect monotonic
time.

Use a forced rebuild when changing this deployment setting, for example
`make -B -j4 RTC_UTC_TRUST=1 binaries`. The repository's QEMU launch arguments
explicitly use `-rtc base=utc,clock=vm`, which permits that assertion for those
guests. Run them only with the corresponding trusted build. Changing an RTC's
convention, clock setting, synchronization, and timezone controls are not yet
implemented.

The CMOS source has **one-second resolution**. Realtime advances by complete
elapsed seconds; nanoseconds remain zero. Timer ticks do not establish the RTC
sample's fractional phase, accuracy, or synchronization. The kernel's
`timekeeping_realtime_sample()` returns a coherent validated UTC timestamp and
source resolution; failure leaves its output unchanged. Short IRQ-safe locking
protects the clock synchronization state, and uptime ticks use atomic access.
No device I/O occurs under that lock. Realtime rejects backwards monotonic
observations and arithmetic overflow. The interface leaves room for future
sources and correction without conflating wall-clock time with monotonic time.

`mg_clock_realtime()` returns the native Mangrove timestamp. The explicit
`mangrove_time_to_unix()` and `unix_time_to_mangrove()` helpers exist only for
interoperability; they do not change the kernel's native clock representation.
Mangrove does not currently provide time-zone or daylight-saving policy,
network time synchronization, or a general VFS timestamp interface.

## Interoperability qualification gap

The current Unix conversion helpers incorrectly copy seconds unchanged. Native
UTC 2000 and Unix UTC 1970 differ by 946684800 seconds; the existing round-trip
test duplicates this error. There are no production callers of those helpers.
Do not use them for Unix interchange until checked translation and independent
known answers are implemented. This does not change the native epoch or the
exFAT calendar path. The libc boundary and its time limitations are summarized
in the [C runtime architecture](../architecture/c-runtime.md).
