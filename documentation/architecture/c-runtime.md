# C runtime, time and text contracts

## Current runtime boundary

loam is Mangrove's native C standard library implementation. It is statically
linked and is not a complete hosted C17 implementation. Native applications compile as freestanding GNU C17 without
builtins, PIC/PIE, red-zone use or host standard headers. The userspace include
path contains the Mangrove C headers and native ABI headers; it does not expose
`kernel/include`.

`libc/crt/crt0.s` receives argc/argv in the native entry registers, aligns the
stack, calls `main`, then calls `exit` so registered normal termination
callbacks run before `process_exit` performs kernel process cleanup.
`kernel/src/process.c::setup_user_stack_args` installs argv strings and a NULL
sentinel; `process_user_thread_entry` enters Ring 3. This is not a Linux initial
stack, environment or auxiliary-vector ABI. Native argv is dynamically captured
and installed in a separate owned mapping under an 8 MiB budget that includes
pointer slots and string terminators. The execution stack remains 32 KiB and
starts 16-byte aligned. The compatibility `process_spawn()` and
`process_spawn_with_output()` command-string interfaces retain their
256-byte input bound; in-tree applications use the argv interfaces. See
[process argument ownership and limits](processes-and-syscalls.md) for
admission, copying and failure contracts. There is no environment vector,
dynamic linker, or C++ constructor/finalizer walk. Native ET_EXEC files may
contain one ELF `PT_TLS` image. Each Ring 3 context receives a private instance
with initialized bytes copied and the remainder zero-filled. The x86-64 FS
thread pointer addresses a self-pointer TCB followed by an empty DTV; static
TLS symbols use the compiler's negative `local-exec` offsets before that TCB.
Native userspace is built with `-ftls-model=local-exec`; general-dynamic,
local-dynamic, initial-exec, dynamic TLS modules, and a dynamic linker are
unsupported. Normal `exit` runs registered callbacks and then
attempts to flush open output streams; `_Exit` does not flush. Static initialized
objects and zero-filled storage are supplied by ELF load segments. GNU
initialization-array attributes are not part of the supported startup contract.

The target uses the x86-64 System V function ABI and little-endian LP64 data
model. Native syscalls use RAX for the number/result and RDI/RSI/RDX for three
arguments (`libc/src/mangrove_syscall.s`). Larger requests use native structs.
Handles are generation-checked `u32`; native results are signed `i64`, with
negative `MG_ERR_*` values. These layouts and error contracts must remain
separate from ISO C typedefs and errno.

User threads have eager FXSAVE/FXRSTOR state (`kernel/src/fpu.c` and
`scheduler.c`); normal user SSE/x87 ABI use is supported. Userspace has an
original Mangrove math primitive library and an x87/SSE floating-point
environment interface. Target `long double` occupies 16 bytes but has a
64-bit significand, not IEEE binary128.
The loader accepts ET_EXEC and maps PT_LOAD segments. It validates and
instantiates at most one `PT_TLS` template using the native static local-exec
model. Dynamic TLS, unwinding and dynamic initialization are unsupported, not
established merely because GCC can emit them.

## Library inventory and known gaps

Compiler-provided or host headers are not searched by the native userspace
build (`-nostdinc`).
Host-only fsstress's POSIX adapter uses the host libc; its additional headers
are not Mangrove facilities.

| Area and authoritative code | Actual behavior and boundary |
| --- | --- |
| `stddef.h`, `stdint.h`, `inttypes.h`, `limits.h`, `stdbool.h`, `stdarg.h`, `stdalign.h`, `stdnoreturn.h`, `iso646.h`; `include/types.h` | Standard sizes, pointer integers, fixed/least/fast/max-width integers, limits, format macros, `offsetof`, `max_align_t`, `wchar_t`, bool, alignment/noreturn keyword macros, alternative operator tokens and compiler `va_list`/`va_copy` derive from the x86-64 GCC ABI. Native `u64`/`i64` remain explicit long-long aliases; packed native request layouts do not use standard `size_t`. Header self-containment, strict C17 include order and key type identities are checked. |
| `<stdatomic.h>` | C17 atomic scalar types and operations use GCC `__atomic` intrinsics. Naturally aligned scalar and pointer objects up to 64 bits are lock-free on the native x86-64 target and require no `libatomic`; larger or aggregate atomic objects are not supported. Acquire/release and sequentially consistent operations, compare/exchange, scalar read-modify-write operations, fences and `atomic_flag` are provided. |
| `<mg/sync.h>`, `libc/src/sync.c` | Native process-local 32-bit address wait/wake, atomic fast-path mutexes, and sequence-based condition variables. They can coordinate native workers created through `<mg/thread.h>`. See [userspace synchronization](userspace-synchronization.md) for return codes, memory lifetime, interruption, destruction, and locking rules. |
| `<mg/thread.h>` | `mg_thread_create`, `mg_thread_join`, `mg_thread_detach`, `mg_thread_exit`, and `mg_thread_self` provide native process-local worker lifecycle. Workers are joinable by default; join consumes the generation-tagged thread handle, while detach enables automatic reap. Each worker uses a 32 KiB guarded stack and an independent static TLS instance. Thread IDs are identity only, not handles. This is not `threads.h` or POSIX pthread compatibility. |
| `string.h`, `src/string.c`, `src/strtok.c`, `src/error.c` | Byte-oriented string and memory functions, including search, span, tokenization, collation and transform routines, plus `strerror`. `strcoll`/`strxfrm` use the fixed C-locale byte ordering; `strtok` keeps process-global continuation state. There are no implicit Unicode semantics. `strncat` checks its count before reading bounded source bytes. |
| `stdlib.h`, `src/stdlib.c`, `src/floatconv.c`, `src/allocator.c` | Integer and floating conversion (`strto*`, `ato*`), `div`/absolute-value routines, deterministic `rand`, `bsearch`, allocation-free heapsort `qsort`, `getenv`, `system` and process-local allocation. `aligned_alloc` supports power-of-two alignments; size must be a multiple of alignment and `realloc` preserves that alignment. Integer conversions support bases 2–36 and checked range reporting. Floating conversion supports decimal and hexadecimal input using target `float`, `double` and x87 `long double` formats. `getenv` currently finds no variables; there is no command processor (`system(NULL)==0`, commands fail with ENOSYS). |
| Allocation | First-fit process-local lists, 16-byte `malloc` alignment, over-aligned allocation, split/coalesce and checked allocation-size arithmetic. Each block records allocation extent, user offset and alignment so `free` and `realloc` retain correct ownership; the block header is 64 bytes on this target, 32 bytes larger than before. calloc checks multiplication; failed realloc preserves the original object and sets ENOMEM. `malloc(0)` returns NULL; realloc-to-zero frees and returns NULL. Mappings are retained until process exit; allocator operations have no user-thread concurrency contract. |
| `stdio.h`, `src/stdio.c`, `src/stdio_stream.c`, `src/scanf.c` | Opaque `FILE` streams, standard streams, open/close/reopen, byte and block I/O, buffering, pushback, EOF/error state, seek/position, remove/rename, `perror`, formatted output and formatted input. The three standard streams use static storage; each additional `FILE` is allocated on demand and participates in `fflush(NULL)`. `FOPEN_MAX` remains 16, the public minimum simultaneous-stream guarantee, not a runtime ceiling. `stdin` is unbuffered, `stdout` is line-buffered and `stderr` is unbuffered. Mangrove currently has one inherited output handle, so `stderr` shares `stdout`'s native destination; writing to `stderr` first flushes pending `stdout` data. |
| `math.h`, `libm.a`, `libc/src/math/` | loam supplies original Mangrove implementations for IEEE binary32, binary64 and x87 extended classification and comparison macros, plus `nanf`/`nan`/`nanl` constructors that ignore the tag and return a canonical quiet NaN; absolute value, sign copy, min/max and positive difference; truncation, floor, ceiling, ties-away `round`, rounding-mode-sensitive `rint`/`nearbyint`, and the `lrint`/`llrint`/`lround`/`llround` conversion families; `modf`, `frexp`, `ilogb`, `logb`, `nextafter`, `nexttoward`, square root, exponent scaling (`ldexp`, `scalbn`, `scalbln`), exact binary remainder (`fmod`, `remainder`, `remquo`), fused multiply-add (`fma`), real power (`pow`), error functions (`erf`, `erfc`), gamma (`tgamma`, `lgamma`), cube root, hypotenuse, the exponential/logarithm and hyperbolic/inverse-hyperbolic families, inverse trigonometry, and `sin`/`cos`/`tan` at all three precisions. `complex.h` supplies compiler-compatible complex construction and component macros plus every C17 complex function family at float, double, and x87 extended precision: arithmetic support functions, exponent/logarithm, power, square root, trigonometric, inverse-trigonometric, hyperbolic and inverse-hyperbolic functions. The complex interfaces use the compiler's native `_Complex` ABI; extended operations retain x87 precision. GCC supplies real and complex types here but not the optional imaginary types. Float/double representations use bounded bit operations; x87 `long double` uses its 80-bit significand and 15-bit exponent representation. `sqrtf`/`sqrt` use SSE2 scalar square-root and `sqrtl` uses x87 `FSQRT`. Scaling uses x87 `FSCALE`; remainder uses iterative `FPREM`/`FPREM1`. `remquo` derives signed low quotient bits with bounded integer long division; its worst case performs at most 32,766 exponent steps and allocates no memory. `fma` aligns exact significand products and addends in a fixed-width integer accumulator before one rounding in the active mode; its x87 accumulator occupies at most 8,216 bytes, plus fixed caller/callee frames, and it performs no dynamic allocation. `pow` computes logarithms as two-part x87 values using an 80-term atanh expansion, multiplies with a fused residual, and carries that residual into compensated exponentiation; integral exponents through 2^32-1 use bounded double-part exponentiation; exact powers of two use exponent scaling to avoid spurious inexact flags and preserve directed overflow/underflow results. Gamma uses a near-one Weierstrass series, recurrence to a bounded interval, Stirling's expansion for large positive inputs, and reflection for negative nonintegral inputs; it uses no dynamic allocation. `lgamma` returns `log|Gamma(x)|`; the POSIX `signgam` extension is not provided. Float and double real functions are evaluated through the x87 core before target-format conversion. `erf` uses a compensated Maclaurin series below unit magnitude; `erfc` evaluates a direct complementary-tail continued fraction above that boundary, preserving small tails instead of subtracting from one. The x87 implementation uses bounded, input-dependent recurrence depth and no dynamic allocation. `hypot` normalizes both inputs by an exact power of two before its square sum and has a rounding-mode-aware shortcut for very small ratios. Exponential and logarithm functions use x87 `F2XM1`, `FYL2X`, and `FYL2XP1`, with compensated range reduction for `exp`. Hyperbolic functions use cancellation-aware `expm1`/`log1p` formulas and a bounded `atanh` series near zero. Inverse trigonometric functions use ratio-bounded argument reductions, a compensated Taylor series for `atan`, and stable square-root/`atan2` identities for `asin` and `acos`. Trigonometric functions reduce arguments using a 16,704-bit fixed-point `2/pi` table, then evaluate the reduced values with original Mangrove Taylor/Horner polynomials; neither reduction nor evaluation allocates memory. Complex finite-input calculations use scaled formulas to limit intermediate overflow and cancellation; Annex G special-value combinations are covered by selected vectors, not a complete conformance matrix. Out-of-range integer-rounding conversions return zero and raise `FE_INVALID`; valid `lrint` conversions use the active rounding mode, while `lround` conversions round halfway values away from zero. The 256-bit MPFR host tests compare 63,235 deterministic real records, including gamma and lgamma samples at each precision and four-mode rounding checks, plus 1,592 FMA records using MPFR fused operations at target precision. The MPC/MPFR complex tests compare 27,689 deterministic records across the three target precisions, including subnormal inputs, large exact trigonometric arguments, and long-double branch-point neighborhoods. FMA checks include all four rounding modes, cancellation, reciprocal factors spanning normal exponent ranges, least-subnormal addends, range recovery, overflow and underflow. Complex tests cover all function families, magnitude and phase, branch-axis and signed-zero witnesses, infinity/NaN projections, and near-pole cases; sampled x87 maximum component error was 10.5 ULP for `cacosh` and at most 4.9 ULP for other inverse functions. The real tests enforce measured limits of 1 ULP for `cbrt`, 1.5 ULP for `hypot`, 2 ULP for sampled power, error functions, exponential/logarithm, hyperbolic, inverse-trigonometric and trigonometric results, and 8 ULP for sampled gamma and lgamma (near zero, lgamma uses absolute spacing at one). The complex tests allow up to 8 ULP for direct families and 32 ULP for inverse and power families, using a component-relative scale for severe cancellation; the observed samples stayed below these bounds. Tests include subnormals, large trigonometric arguments, near-singular inputs and selected directed-rounding cases. These bounds are sample-based, not proofs of universal or correctly directed rounding; gamma and complex directed rounding are not comprehensively tested. Math functions report floating-point exceptions for covered conditions; they do not promise `errno` reporting. |
| `fenv.h` and `tgmath.h` | `fenv.h` saves and restores the x87 environment and SSE MXCSR, updates supported exception flags, and synchronizes the four rounding modes across both units. `fegetround` returns -1 if independent code has made the x87 and SSE rounding controls disagree. The kernel saves/restores both units in user-thread context. `tgmath.h` selects float/double/long-double variants for implemented real functions and matching complex variants where defined; its `fabs` macro selects `cabs` for complex arguments. The `carg`, `cimag`, `conj`, `cproj` and `creal` generic macros accept real or complex arguments. `complex.h` provides the `CMPLX*` constructors. |
| Formatted I/O and conversion | Output supports integer, pointer, character, string and floating conversions (`aAeEfFgG`), flags, width, precision, `*`, and `hh/h/l/ll/j/z/t/L` where meaningful. Input supports integer, pointer, floating, character, string, scanset, `%n`, assignment suppression, width, and `hh/h/l/ll/j/z/t/L`; wide and locale-sensitive conversions are unsupported. The float converter uses bounded-precision integer arithmetic and the target's binary32, binary64 and x87 extended formats. Decimal formatting uses about 72 KiB of temporary heap workspace; string-to-float conversion uses about 46 KiB. Allocation failure returns failure with `ENOMEM`. `snprintf` reports the required length, NUL-terminates when capacity is nonzero, and accepts `(NULL, 0)`. Malformed or unsupported format syntax returns failure and sets errno. |
| Stream semantics and limits | `fopen` supports `r`, `w`, `a`, `+`, `b`, and exclusive `x` with `w`. Native files are seekable; standard streams are not. A stream retains handle ownership until close/reopen/process teardown. Additional stream records are allocated per `fopen`; each open file stream allocates a 4096-byte default buffer. Process handle slots grow geometrically from 16 as needed; the existing 32-bit handle encoding permits at most 65,535 indexed handles per process, and practical capacity is lower when memory or other live objects are exhausted. Stream and handle allocation failures are reported without imposing a separate loam stream-table limit. `fflush` transfers buffered output bytes to the native object interface; it is not a filesystem durability barrier. Uncertain writes poison that stream to prevent retries. `rename` returns `EEXIST` when another entry already occupies the destination; replacing an existing destination is not supported. `tmpfile` and `tmpnam` return `ENOSYS`. `fpos_t` is a `long`, so positions beyond `LONG_MAX` are not representable. `%s` input without a field width retains the standard caller-buffer responsibility. Floating `scanf` input items are limited to 65,536 bytes to bound temporary storage; longer items return a matching failure with `EOVERFLOW`. |
| Output/error boundary | Formatted output writes through `FILE`; standard output is buffered and normal `exit` attempts to flush open output streams after `atexit` callbacks. Reads from standard input flush pending line-buffered `stdout`; the shared interactive console-line helper also flushes `stdout` before blocking through the native input API, which bypasses stdio. Flush failures cannot change the exit status. `quick_exit` and `_Exit` do not flush streams. Flush means bytes were accepted by native I/O, not durable media persistence. `strerror` supplies fixed C-locale text for each published errno value and returns `Unknown error` for other values; `perror` writes that text to `stderr` while preserving the reported errno. Native `mg_result_t` remains distinct and `mg_error_to_errno` is an explicit, potentially lossy mapping. EOF is handled before mapping because it is a separate native result. |
| `ctype.h`, `errno.h`, `assert.h`, termination | ctype implements the ASCII C locale; errno is a static thread-local cell, so each worker has its own value. `atexit` and `at_quick_exit` retain up to 32 callbacks each, in reverse registration order; normal initial-thread CRT return runs `exit`, which invokes callbacks and attempts to flush stdio before native process teardown. Worker return uses its private CRT target and does not run process callbacks. `_Exit` skips callbacks and flushing; abort traps because signals are unavailable. Assertions print a diagnostic then abort. The allocator, stdio streams, and other mutable loam state are not yet thread-safe. |
| `libmgtls.a`, `<mg/tls.h>` | `make binaries` builds the wolfSSL 5.9.4-derived mgTLS library with a TLS 1.3 client/X.509 profile and Mangrove secure-random adapter. The current API provides library initialization, cleanup, and secure random bytes; it does not provide a socket transport, handshake, trust-store loading, or certificate/hostname policy. See [mgTLS foundation](../security/mgtls.md). |
| `wchar.h`, `src/wchar.c`, `src/utf8.c` | Strict UTF-8 multibyte conversions (`MB_LEN_MAX == MB_CUR_MAX == 4`), restartable and nonrestartable conversion APIs, `mbstate_t`, compiler `wint_t`, `WCHAR_*`/`WINT_*` limits, `WEOF`, and common wide-string/memory routines. The multibyte encoding is fixed; there is no `setlocale` or locale database. `wchar_t` uses the signed 32-bit compiler ABI. `wcscoll`/`wcsxfrm` use code-unit order; conversion rejects invalid scalars with `EILSEQ`, while wide-string routines do not validate their inputs. Wide I/O, Unicode classification and several other wide/locale interfaces are absent. |
| `time.h`, `src/time_standard.c`, `src/time_convert.c` | `time_t` and `clock_t` are signed 64-bit values. C wall time and `TIME_UTC` use Unix seconds; the native clock remains signed Mangrove-epoch seconds. Checked conversions preserve fractions and reject overflow. UTC breakdown, UTC-only `localtime`/`mktime`, C-locale `strftime`, `asctime`/`ctime`, `difftime`, and `timespec_get` are implemented. Reentrant `_r` forms are Mangrove extensions. `clock()` reports process scheduler CPU ticks at nominal 1 ms resolution, not uptime. Realtime is unavailable unless the deployment explicitly trusts its RTC as UTC; `time()` returns -1 and `timespec_get` returns zero on failure. There is no timezone database, clock setting, DST, or subsecond source precision. |
| Other ISO facilities | `locale.h`, `setjmp.h`, `signal.h`, `threads.h`, `uchar.h` and `wctype.h` are not supplied. Locale remains fixed: byte `ctype` and collation use C-locale rules, while multibyte conversion uses Mangrove's fixed UTF-8 policy. There is no ISO C `threads.h` or POSIX thread API; Mangrove workers are documented separately in `<mg/thread.h>`. Signals, nonlocal jumps, Unicode classification and UTF-16/UTF-32 conversion have no public library contract. |
| Toolchain/runtime helpers | Native links use GNU ld and explicit Mangrove archives, without host CRT/default libraries. Programs using math link objects, `libm.a`, `libc.a`, then the target compiler's `libgcc.a` for compiler-generated helpers. Programs not using math do not pull math objects into their image. The kernel shares only explicitly listed freestanding `string.c`, `units.c`, and `utf8.c`, so userspace math and runtime objects cannot enter the kernel build through discovery. Native ET_EXEC images may carry one static local-exec `PT_TLS` template; dynamic TLS models remain unsupported. This is distinct from the mgTLS network-security library. |

The kernel separately compiles shared string, units, and UTF-8 codec sources;
calendar conversion remains in its own kernel source. The Makefile lists the
kernel-shared sources explicitly, preserving the kernel's no-SSE compilation
and preventing userspace-only runtime objects from entering the kernel archive.

`make test-unicode` exercises the production UTF-8 and multibyte/wide conversion
sources, Shoot line editor, and terminal table text metrics under ASan/UBSan.
`make test-libc` includes that target, runs the host-side runtime probes, and
builds a target-ABI link probe against the actual static libc and math archives.
The probes compile production
libc sources against a controlled native-I/O adapter and compare supported
format, scan and conversion results with the host C library. The math group
checks 63,235 deterministic float, double and long-double results against
256-bit MPFR, plus 1,592 exact single-round FMA cases at target precision,
including all four rounding modes. It uses seeded inputs across the x87
exponent range and explicit subnormal, `log1p`, error-function recurrence and tail-boundary vectors, then repeats the production
math sources under ASan/UBSan. The tested `cbrt` inputs are within 1 ULP,
`hypot` within 1.5 ULP, and sampled exponential/logarithm, hyperbolic,
inverse-trigonometric and trigonometric inputs within 2 ULP, including selected
directed-rounding inputs; these tests do not prove universal
or correctly directed rounding. Corrupted-result,
quotient, unsupported-symbol and incomplete-trace controls are included. The
test requires an x86-64 host and the MPFR shared library. The complex numerical
probe additionally requires host MPC. The target link probe checks declarations
and symbols from both archives; a deliberately missing symbol verifies that the
link-control test rejects unresolved references.
`make test-guest-mgfs` also builds and launches the real Ring 3
`fstest stdio RUN [TARGET]`, time, and math phases in QEMU. The math phase
checks x87 precision, scaling, remainder, cube-root, hypotenuse and selected
hyperbolic, inverse-trigonometric and trigonometric results at all three input precisions, representative exponential
and logarithm results at all three precisions, supported primitive edge cases, and fenv state across
a syscall and scheduled context switch. It includes underflow and inexact flags when
`nextafter` steps from zero to the least subnormal and invalid flags for
`ilogb` exceptional inputs. The runner checks child completion and independently
verifies all recorded square-root, scaling, remainder, cube-root, hypotenuse,
exponential, logarithm, hyperbolic, inverse-trigonometric, trigonometric and
fused multiply-add values against MPFR. It also records six gamma results and
fifteen complex results across float, double and x87 long double, which the host
runner independently checks against MPFR and MPC. A guest PASS requires native
child exit evidence and a complete transcript; its persistent root image is
checked independently after QEMU stops.
These are host and QEMU checks; they do not establish physical-hardware, SMP,
or power-loss behavior.

## Time: current contracts and limits

See [timekeeping](../kernel/timekeeping.md) for the source contract. Native
realtime is signed seconds plus a canonical nanosecond fraction from UTC
**2000-01-01**. Monotonic time is boot-local milliseconds from the selected
hardware clocksource (with a reported degraded PIT-count fallback). The kernel
samples a validated CMOS calendar only under explicit RTC-as-UTC deployment
trust, and reads it afresh for each realtime query; untrusted, unavailable, or
invalid clocks remain unavailable. Whole-second resolution does not prove RTC
accuracy. The monotonic clock supports elapsed durations and excludes time
while an external VM pause stops guest counters. A separate syscall returns
scheduler-accounted CPU ticks for the calling process; this is the source for C
`clock()`, not uptime. There is no clock setting, timezone, DST database or
network time synchronization service.
Security-sensitive users can query `mg_clock_realtime_info` to distinguish an
untrusted deployment, unavailable RTC, invalid sample, and trusted sample.
Its timestamp is usable only in the trusted state; monotonic time is never a
wall-clock substitute. The generic `mg_unix_time_compare` helper compares
normalized second/nanosecond timestamps for certificate validity boundaries,
but certificate parsing, chain validation, and hostname matching remain
userspace TLS responsibilities.

`libc/src/time_convert.c` supplies proleptic Gregorian conversions, including
negative dates, leap-year rules and range checks. It rejects leap-second input
(second 60); there is no leap-second/TAI model. Calendar years are signed 32-bit;
not every signed-64-bit second value can be converted to that calendar.

`mangrove_time_to_unix` and its inverse perform checked translation by
**946684800 seconds** and preserve nanoseconds. Calendar conversion uses the
proleptic Gregorian calendar, rejects leap-second input, and reports values
outside the supported `struct tm` year range. No native clock, log, or on-disk
representation is implicitly migrated by the C Unix-time boundary.

exFAT uses native calendar conversion, not the Unix helper, with an explicit
1980–2107 admission range, UTC fields and whole-second increment encoding in
retained transactions. Ordinary reads preserve access timestamps. FAT32 stores
local civil date/time without a timezone field; the driver preserves imported
fields but does not currently populate ordinary mutation timestamps. MGFS has
no per-file timestamp API, and its formatter stores the supplied
`format_time_ns` value without assigning it a Unix epoch. Native
`mg_path_info_t` exposes no per-file timestamps; adding `<time.h>` does not
create that facility or authorize rewriting imported filesystem dates.

## Text: current guarantees and boundaries

| Layer | Actual behavior |
| --- | --- |
| Standard byte strings | Lengths and offsets are bytes; no implicit decoding, casing or normalization. |
| Shared codec: `include/mg/utf8.h`, `libc/src/utf8.c` | Allocation-free strict UTF-8 encoder, bounded decoder, validator and incremental state machine used by kernel and userspace. It accepts only Unicode scalar values, rejects overlong forms, surrogates, invalid leads and values above U+10FFFF, and distinguishes structurally incomplete input from invalid input. Explicit-length decoding accepts U+0000; string APIs stop at NUL. Strict decoding reports the malformed start byte; presentation decoding groups a malformed prefix as one U+FFFD unit. Streaming callers can retry a byte that interrupts an incomplete sequence. It performs no normalization, case mapping or display-width calculation. |
| C multibyte and wide strings | The fixed encoding is UTF-8, with no locale selection or stateful encodings. `mbrtowc`/`mbrlen` retain split input in `mbstate_t`, return `(size_t)-2` for incomplete input, and return `(size_t)-1` with `EILSEQ` for malformed input. Restartable string conversions preserve source position when output capacity stops conversion; the nonrestartable functions use separate internal states. `wchar_t` is the compiler's signed 32-bit type; conversion accepts Unicode scalar values. `btowc`/`wctob` handle ASCII single-byte characters and return `WEOF`/`EOF` for other byte values. Wide-string comparison is code-unit order, not Unicode collation. |
| Kernel `utf8.c` | Terminal adapter over the shared incremental codec. It applies a replacement-character policy to malformed input, preserves an interrupting byte as a possible next character, and retains incomplete sequences. There is no finish/EOF operation; terminal presentation behavior is not a pathname validation policy. |
| Reed `buffer.c`/`editor.c` | Strict scalar validation, shared UTF-8 encoding and codepoint-boundary navigation. File loading rejects embedded NUL and malformed UTF-8. Buffer offsets and file positions remain byte counts; cursor movement and edits occur only at scalar boundaries. Movement is not grapheme-cluster editing. |
| Shoot completion/line editor | Completion parses command syntax as bytes and uses the shared strict decoder for display counts. The line editor accepts complete valid UTF-8 sequences, drops malformed bytes, and moves/deletes at scalar boundaries. Display accounting remains one cell per scalar. |
| Console/terminal/font | The scalar console-input helper encodes each valid scalar as one complete UTF-8 sequence before queueing bytes. The current USB keyboard map supplies ASCII to ordinary line editing; raw key events carry codepoints. PSF2 Unicode-table parsing uses the shared strict decoder. The renderer uses a fixed one-cell-per-codepoint grid with font fallback; it has no general grapheme, combining-mark, East Asian width, wide-cell, or shaping model. |
| Storage and log presentation | `volumed` and `mkmgfs` reject invalid UTF-8 labels through the shared codec. `logv` and common table display preserve malformed bytes for diagnostics but measure each malformed prefix as one replacement cell; `lsdsk` truncates at valid scalar or replacement boundaries. The independent `mgfsck` checker retains its own decoder so image checking does not depend on the production codec. |
| MGFS | Strict UTF-8, case-sensitive byte comparison, no normalization, on-disk 255-byte name limit. That format limit remains even after a native ABI extension. |
| FAT32 | Validated UTF-16 LFNs, pinned Unicode 15.1 simple uppercase and conservative OEM alias policy; no universal full folding or normalization. See [names](../filesystem/fat32/names.md). |
| exFAT | Validated UTF-16 entry sets and the volume's validated upcase table/hash. A maximum 255-unit name needs at most 765 UTF-8 bytes. Internal parsing includes those names independently of native representability. |
| Native paths/enumeration | NUL-terminated paths allow at most 65,535 bytes before NUL, including separators. FAT32/exFAT components and extended directory results support up to 765 UTF-8 bytes (255 UTF-16 code units); MGFS retains its separate 255-byte on-disk name limit. Legacy directory-read ABI records remain 256 bytes; new programs use `directory_read_ex()` / `directory_read_batch_ex()`. Kernel length-aware VFS helpers reject embedded NUL and do not read beyond the supplied length. Invalid or overlong input is never truncated. Checked enumeration distinguishes errors from EOF, consumes an unrepresentable entry and defers errors after completed batch entries. Anchored CWD/mount identity remains authoritative. |

Filesystem drivers use the shared strict codec at their UTF-8 boundaries but
retain their existing name identity rules: MGFS compares valid UTF-8 bytes
case-sensitively without normalization; FAT32 uses its pinned Unicode 15.1
simple-uppercase table; exFAT uses each volume's validated upcase table.
Neither the codec nor terminal presentation changes those on-disk rules.
Native complete pathnames are bounded at 65,535 UTF-8 bytes, while supported
FAT32/exFAT components can require up to 765 UTF-8 bytes for 255 UTF-16 code
units. MGFS remains limited to 255 UTF-8 bytes on disk. These are byte and
format limits, not Unicode scalar counts;
filesystem-specific validation rejects malformed UTF-8 before name mutation.
Ordinary byte-string functions continue to operate on bytes without decoding.
The [terminal contract](terminal.md) describes the framebuffer grid and
Mangrove's canonical `ESC{...}` output-control stream.

## Runtime architecture boundaries

- Compiler-derived ISO types and Mangrove-native fixed-width aliases are
  intentionally separate. Native syscall results and structures keep their
  existing ABI; C errno conversion is explicit and lossy.
- Userspace builds as GNU C17, while kernel and boot code retain their own
  freestanding flags. Byte strings, units and the strict UTF-8 codec are shared
  with the kernel; allocation, errno, formatting and process wrappers remain
  userspace-only.
- `ctype` remains ASCII C-locale classification, while multibyte conversion
  has a separate fixed UTF-8 encoding policy. There is no `setlocale`, locale
  database, Unicode `ctype`, normalization, or locale collation service.
  This fixed conversion policy does not provide general locale support.
- The supported executable TLS model is x86-64 static `local-exec`; errno
  uses that TLS. Each native worker gets a separate PT_TLS instance and FS base.
  C17 atomics, native wait/wake, mutexes, conditions, and worker lifecycle are
  available; allocator, stdio, and other loam operations do not yet have a
  general concurrent-use contract.
- Normal `exit` runs registered callbacks, attempts to flush open writable streams, then
  delegates to native process teardown. This does not provide global VFS sync,
  disk quiescence or crash durability. `quick_exit` and `_Exit` skip stream
  flushing.
- `FILE` wraps native object handles and preserves their EOF, confirmed-count,
  error and uncertainty contracts. It does not retry an uncertain output. Time
  and filesystem-specific text rules are documented at their owning interfaces.
- Mangrove supplies a useful C library subset, not a complete hosted C17 or
  POSIX environment. Stream rename replacement, temporary files, wide I/O,
  locale selection, signals, nonlocal jumps and the ISO `threads.h`/POSIX
  thread interfaces are unsupported as described above. Mangrove's native
  worker API is documented separately. Real and complex math provide the families
  in the inventory above. Numerical bounds are based on deterministic samples;
  they do not establish universal correct rounding or complete IEC 60559 Annex
  G special-value conformance.

## Standards references and claim discipline

[WG14 project status](https://www.open-std.org/jtc1/sc22/wg14/www/projects.html)
identifies C17 as ISO/IEC 9899:2018. The public
[C17 draft N2310](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2310.pdf)
provides the C17 library clauses used for the math and type-generic interfaces.
[N1570 draft](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf)
provides reference clauses for many retained library interfaces but is a C11
draft, not formal C17 conformance evidence. This document describes a bounded
implemented subset; Mangrove has not completed a formal hosted C17 conformance
assessment. The [NIST Digital Library of Mathematical Functions, logarithm and power series](https://dlmf.nist.gov/4.6), [error-function continued fraction](https://dlmf.nist.gov/7.9), and [incomplete-gamma series](https://dlmf.nist.gov/8.7) provide mathematical identities used to derive the original implementations; no external source code is incorporated into loam. The [C11/C17 issue log](https://www.open-std.org/JTC1/SC22/WG14/issues/c11c17/log.html)
records later clarifications. The
[Unicode conformance](https://www.unicode.org/versions/Unicode17.0.0/core-spec/chapter-3/)
distinguishes encoding validity from casing, normalization and text segmentation.
Consulting that reference does not update Mangrove's pinned filesystem tables.
