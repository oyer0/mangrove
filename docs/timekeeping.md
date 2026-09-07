# Mangrove timekeeping

Mangrove has two independent clocks.

- The monotonic clock is elapsed milliseconds from a boot-local origin. It is
  used for scheduler deadlines, sleeps, timeouts, and uptime. It does not
  change when realtime is corrected.
- The realtime clock is UTC and is represented by signed seconds plus
  nanoseconds from the **Mangrove Epoch**, `2000-01-01 00:00:00 UTC`. The
  timestamp `0` is therefore that instant; dates before it have negative
  seconds.

Pith reads and validates the x86 CMOS RTC once during boot and records that
  value alongside the monotonic synchronization point. Realtime queries then
  advance from the monotonic clock. The RTC is treated as UTC, and an invalid
  or unavailable RTC leaves realtime unavailable without preventing boot or
  affecting monotonic time.

`mg_clock_realtime()` returns the native Mangrove timestamp. The explicit
`mangrove_time_to_unix()` and `unix_time_to_mangrove()` helpers exist only for
interoperability; they do not change the kernel's native clock representation.
Time zones, daylight-saving rules, network synchronization, and filesystem
timestamps are outside this stage.
