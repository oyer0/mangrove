# Mangrove timekeeping

Mangrove keeps elapsed time, wall time, and scheduler accounting distinct.

- The monotonic clock is elapsed milliseconds from a boot-local origin. It is
  used for scheduler sleep deadlines, timeouts, and uptime. It does not change
  when realtime is corrected.
- The realtime clock is UTC and is represented by signed seconds plus
  nanoseconds from the **Mangrove Epoch**, `2000-01-01 00:00:00 UTC`. The
  timestamp `0` is therefore that instant; dates before it have negative
  seconds.
- Scheduler tick and process CPU accounting count delivered timer events. They
  are not wall-clock or elapsed-time sources.

The monotonic source is selected after ACPI discovery and permanent MMIO setup:
64-bit HPET, 32-bit ACPI PM timer, narrower HPET, narrower ACPI PM timer, then
the degraded delivered-PIT-tick fallback. HPET is enabled only when selected;
its firmware legacy-route setting is preserved. PM timer GAS reads use the
declared I/O or system-memory location and the FADT's 24/32-bit width. KVM
paravirtual clocks and TSC are not selected: their cross-CPU frequency and
pause/resume behavior have not been qualified on Mangrove's supported targets.
Invalid or unavailable hardware counters fall through without fabricating a
frequency.

The hardware clock is sampled from one origin, converted directly from its
declared femtosecond period, and published through an SMP atomic maximum. Narrow
counters are extended under a shared lock; sampling and extension are ordered
together so a delayed CPU cannot turn an older sample into a false wrap. A
narrow counter can only account for wraps that are observed; the periodic PIT
path samples it even when no caller requests uptime. The status API reports the
source, one-millisecond public resolution, and whether the source is full-range,
wrap-bounded, or degraded. If no qualified source exists, boot continues with
the nominal 1 ms PIT event count and reports the degraded source. That fallback
can drift and cannot recover interrupts that were never delivered.

Hardware-source monotonic time accounts for counter elapsed time rather than
PIT delivery count, so delayed/coalesced PIT interrupts do not permanently lose
elapsed time. PIT/LAPIC interrupts remain scheduler notifications and
preemption inputs. Sleeping threads keep absolute monotonic millisecond
deadlines and the BSP's next PIT notification checks those deadlines; late
notification may delay a wake, but it does not change the deadline. Process CPU
time remains scheduler-accounted and tick-resolution.

The clock is boot-local and does not advance while an external virtual-machine
pause stops the guest's virtual counters. Physical CPU idle time is included
while the selected platform counter continues running. Mangrove has no system
suspend/resume implementation; hardware sleep states that stop/reset the
selected source are not qualified. Counter wrap observation limits for narrow
sources are described above.

CMOS contains no timezone provenance. Realtime is **unavailable by default**:
a deployment must explicitly establish that its RTC contains UTC and build with
`RTC_UTC_TRUST=1`. Physical hardware must not inherit that assertion merely
because QEMU uses UTC. An untrusted, invalid, or unavailable RTC does not
prevent boot or affect monotonic time. The trusted-RTC policy stamp invalidates
kernel objects on incremental policy changes.

`timekeeping_init()` runs after the monotonic source has been initialized. It
uses a boot RTC sample only for boot identity material; it does not anchor
realtime to uptime. Every trusted realtime status/query obtains a fresh stable
CMOS sample. The RTC access lock serializes index/data port transactions across
CPUs. Each sample is validated and returned at one-second resolution; the
kernel does not interpolate subsecond time. If the configured source is
unavailable or invalid at query time, realtime fails closed even if an earlier
sample was valid. Valid backward or forward corrections are exposed as wall
time; they never change monotonic values or scheduler deadlines. `TRUSTED`
means the deployment asserted UTC and the current sample is stable and
calendar-valid; it is not independent proof of RTC accuracy or freshness beyond
the current read. Mangrove has no NTP or other network time synchronization.

Ordinary QEMU development launches use `-rtc base=utc,clock=host`, allowing the
RTC to reflect host wall time across a QEMU process pause or host suspension.
`QEMU_RTC_CLOCK=vm` selects deterministic virtual-time behavior for explicit
tests; `rt` is also available when a test needs QEMU's host monotonic RTC clock.
These RTC options do not select or alter the kernel monotonic source. The
development-image targets build with `RTC_UTC_TRUST=1`; ordinary `make
binaries` and hardware-image builds retain the untrusted default. Changing an
RTC's UTC convention remains an explicit deployment decision.

The CMOS source has **one-second resolution**; nanoseconds remain zero. The
kernel's `timekeeping_realtime_sample()` returns a freshly validated UTC
timestamp and source resolution; failure leaves its output unchanged. RTC I/O
does not occur under the small lock that protects deployment trust and boot
identity. No stale sample is extrapolated using uptime.

`mg_clock_realtime_info()` is the explicit quality interface for security
consumers. One syscall returns a coherent `mg_realtime_info_t` with one of
four states: `MG_REALTIME_UNTRUSTED` (the build/deployment did not assert an
UTC RTC convention and CMOS is not read), `MG_REALTIME_UNAVAILABLE` (UTC was
trusted but no stable RTC sample was available), `MG_REALTIME_INVALID` (a
stable sample failed calendar validation or conversion), or
`MG_REALTIME_TRUSTED`. A timestamp and
resolution are populated only in the trusted state; the other fields are zero.
The status call itself returns `MG_OK` when the query succeeds, so callers can
distinguish a clock-quality result from a syscall failure. Existing
`mg_clock_realtime()` remains strict and returns
`MG_ERR_TIME_UNAVAILABLE` for every non-trusted state. It does not expose an
untrusted calendar.

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

`mg_unix_time_compare()` compares normalized signed Unix seconds and
nanoseconds without conversion to a narrower scalar. A TLS implementation can
convert the trusted kernel sample to Unix time and apply X.509's inclusive
validity rule (`notBefore <= now <= notAfter`) using this comparison. The
clock API does not parse certificates, validate certificate chains, or decide
hostname policy; those remain userspace TLS responsibilities. A monotonic
timestamp or elapsed uptime must never substitute for trusted wall time.

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

The native clock syscalls are 61 (monotonic milliseconds), 62 (strict
validated Mangrove-epoch realtime), 63 (boot identifier), 94 (current-process
CPU ticks), 102 (realtime status plus timestamp), and 104 (monotonic sample,
source, and quality). Their fixed-width request/result structures are in
`include/mg/time.h`; libc converts only at the public C wall-time boundary.

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
