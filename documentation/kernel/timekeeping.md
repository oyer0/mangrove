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
They add or subtract exactly 946684800 seconds with checked signed arithmetic,
preserving nanoseconds and leaving the output unchanged on failure. ISO C
leaves `time_t` encoding implementation-defined; Mangrove selects signed
64-bit seconds from the conventional POSIX UTC epoch, 1970-01-01, without
leap-second counting. The C library's calendar conversion supports the range
representable by its signed 32-bit `struct tm.tm_year`; values outside that
range fail with `EOVERFLOW`.

The native `<time.h>` provides `time`, `difftime`, `gmtime`, `mktime`,
`asctime`, `ctime`, `strftime`, and `timespec_get(TIME_UTC)`. Reentrant `_r`
calendar and text functions are Mangrove extensions. `localtime` and `mktime`
use UTC because there is no timezone database or configuration; this is an
explicit UTC-only policy, not general local civil time. `strftime` uses fixed
C-locale English names and UTC `%z`/`%Z`. Unsupported conversions return zero
and set `errno`; the implementation does not provide locale-sensitive or wide
time formatting. `asctime` supports years 1000 through 9999.

`time()` and `timespec_get(TIME_UTC)` fail when native realtime is unavailable;
`time()` returns `(time_t)-1`, while `timespec_get` returns zero and leaves its
output unchanged. Native result codes remain available through `mg_clock_*`;
the C boundary maps unavailable realtime to `ENODATA`. The current clock has
whole-second precision, so `timespec_get` reports `tv_nsec == 0`.

`clock()` returns scheduler-accounted CPU milliseconds for the calling
process, using the kernel's nominal 1 ms scheduler tick and
`CLOCKS_PER_SEC == 1000`. It returns `(clock_t)-1` if the kernel query fails or
the value cannot fit. This accounting is tick-resolution process runtime, not
wall time, uptime, or sub-tick CPU measurement. The monotonic `mg_clock_*`
clock remains the source for elapsed durations.

The native clock syscalls are 61 (monotonic milliseconds), 62 (validated
Mangrove-epoch realtime), 63 (boot identifier), and 94 (current-process CPU
ticks). Their fixed-width request/result structures are in `include/mg/time.h`;
libc converts only at the public C wall-time boundary.

Filesystem timestamps retain their format-specific meanings. exFAT mutations
convert the native UTC calendar directly and write a valid zero UTC offset;
the Unix conversion helpers do not participate. FAT timestamps encode local
civil fields without an offset, and Mangrove does not infer UTC for imported
values or currently populate ordinary FAT32 mutation timestamps. MGFS has no
per-file timestamp API; its formatter stores the caller-supplied
`format_time_ns` value without assigning it a Unix epoch. These boundaries are
described in the [C runtime architecture](../architecture/c-runtime.md) and
[exFAT metadata contract](../filesystem/exfat/README.md#timestamps-and-allocation-percentage).

The C interface follows the applicable C17 time-function contracts; Mangrove's
signed Unix `time_t`, `TIME_UTC` epoch, UTC-only local-time policy, scheduler
tick resolution and error mappings are implementation choices. See the
[WG14 C draft](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf)
for the standard time interfaces and the
[POSIX `time()` specification](https://pubs.opengroup.org/onlinepubs/9699919799/functions/time.html)
for the seconds-since-Epoch convention. See the
[exFAT specification](https://learn.microsoft.com/en-us/windows/win32/fileio/exfat-specification)
for timestamp and UTC-offset fields. Mangrove does not currently provide clock
setting, timezone/DST configuration, network time synchronization, or a general
VFS timestamp interface.
