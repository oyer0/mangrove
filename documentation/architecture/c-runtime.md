# C runtime, time and text contracts

## Current runtime boundary

Mangrove has a statically linked native C runtime, not a complete hosted C17
implementation. Native applications compile as freestanding GNU C17 without
builtins, PIC/PIE, red-zone use or host standard headers. The userspace include
path contains the Mangrove C headers and native ABI headers; it does not expose
`kernel/include`.

`libc/crt/crt0.s` receives argc/argv in the native entry registers, aligns the
stack, calls `main`, then calls `exit` so registered normal termination
callbacks run before `process_exit` performs kernel process cleanup.
`kernel/src/process.c::setup_user_stack_args` installs argv strings and a NULL
sentinel; `process_user_thread_entry` enters Ring 3. This is not a Linux initial
stack, environment or auxiliary-vector ABI. Both public spawn forms currently
copy into a 256-byte argument budget with at most 16 arguments; the legacy
command-line parser stops at the argument limit, whereas argv requests reject
excess counts. There is no environment vector, TLS, dynamic linker, C++
constructor/finalizer walk or standard stream flushing. Static initialized
objects and zero-filled storage are supplied by ELF load segments. GNU
initialization-array attributes are not part of the supported startup contract.

The target uses the x86-64 System V function ABI and little-endian LP64 data
model. Native syscalls use RAX for the number/result and RDI/RSI/RDX for three
arguments (`libc/src/mangrove_syscall.s`). Larger requests use native structs.
Handles are generation-checked `u32`; native results are signed `i64`, with
negative `MG_ERR_*` values. These layouts and error contracts must remain
separate from ISO C typedefs and errno.

User threads have eager FXSAVE/FXRSTOR state (`kernel/src/fpu.c` and
`scheduler.c`); normal user SSE/x87 ABI use is supported. That does not provide
math functions or a tested floating-point environment library. Target `long
double` occupies 16 bytes but has a 64-bit significand, not IEEE binary128.
The loader accepts ET_EXEC and maps PT_LOAD segments; it does not instantiate
PT_TLS or a dynamic linker. TLS, unwinding and dynamic initialization are
unsupported, not established merely because GCC can emit them.

## Library inventory and known gaps

Compiler-provided or host headers are not searched by the native userspace
build (`-nostdinc`).
Host-only fsstress's POSIX adapter uses the host libc; its additional headers
are not Mangrove facilities.

| Area and authoritative code | Actual behavior and boundary |
| --- | --- |
| `stddef.h`, `stdint.h`, `inttypes.h`, `limits.h`, `stdbool.h`, `stdarg.h`; `include/types.h` | Standard sizes, pointer integers, fixed/least/fast/max-width integers, limits, format macros, `offsetof`, `max_align_t`, `wchar_t`, bool and compiler `va_list`/`va_copy` derive from the x86-64 GCC ABI. Native `u64`/`i64` remain explicit long-long aliases; packed native request layouts do not use standard `size_t`. Strict C17 probes cover include order and key type identities. |
| `string.h`, `src/string.c` | Byte-oriented string and memory routines, including overlapping `memmove`; `strerror` is also provided. There are no implicit Unicode semantics, and many standard search/token/collation functions are absent. `strncat` checks its count before reading bounded source bytes. |
| `stdlib.h`, `src/stdlib.c`, `src/floatconv.c`, `src/allocator.c` | Integer and floating conversion (`strto*`, `ato*`), `div`/absolute-value routines, deterministic `rand`, `bsearch`, allocation-free heapsort `qsort`, `getenv`, `system` and process-local allocation. Integer conversions support bases 2–36 and checked range reporting. Floating conversion supports decimal and hexadecimal input using target `float`, `double` and x87 `long double` formats. `getenv` currently finds no variables; there is no command processor (`system(NULL)==0`, commands fail with ENOSYS). |
| Allocation | First-fit process-local lists, 16-byte payload alignment, split/coalesce and checked allocation-size arithmetic. calloc checks multiplication; failed realloc preserves the original object and sets ENOMEM. `malloc(0)` returns NULL; realloc-to-zero frees and returns NULL. Mappings are retained until process exit; allocator operations have no user-thread concurrency contract. |
| `stdio.h`, `src/stdio.c`, `src/stdio_stream.c`, `src/scanf.c` | Opaque `FILE` streams, standard streams, open/close/reopen, byte and block I/O, buffering, pushback, EOF/error state, seek/position, remove/rename, formatted output and formatted input. The fixed stream table has `FOPEN_MAX` entries; file buffers are allocated per stream, while standard streams use static storage. `stdin` is unbuffered, `stdout` is line-buffered and `stderr` is unbuffered. Mangrove currently has one inherited output handle, so `stderr` shares `stdout`'s native destination; writing to `stderr` first flushes pending `stdout` data. |
| Formatted I/O and conversion | Output supports integer, pointer, character, string and floating conversions (`aAeEfFgG`), flags, width, precision, `*`, and `hh/h/l/ll/j/z/t/L` where meaningful. Input supports integer, pointer, floating, character, string, scanset, `%n`, assignment suppression, width, and `hh/h/l/ll/j/z/t/L`; wide and locale-sensitive conversions are unsupported. The float converter uses bounded-precision integer arithmetic and the target's binary32, binary64 and x87 extended formats. Decimal formatting uses about 72 KiB of temporary heap workspace; string-to-float conversion uses about 46 KiB. Allocation failure returns failure with `ENOMEM`. `snprintf` reports the required length, NUL-terminates when capacity is nonzero, and accepts `(NULL, 0)`. Malformed or unsupported format syntax returns failure and sets errno. |
| Stream semantics and limits | `fopen` supports `r`, `w`, `a`, `+`, `b`, and exclusive `x` with `w`. Native files are seekable; standard streams are not. A stream retains handle ownership until close/reopen/process teardown. The 16-entry `FOPEN_MAX` table includes the three standard streams; each open file stream allocates a 4096-byte default buffer. `fflush` transfers buffered output bytes to the native object interface; it is not a filesystem durability barrier. Uncertain writes poison that stream to prevent retries. `rename` returns `EEXIST` when another entry already occupies the destination; replacing an existing destination is not supported. `tmpfile` and `tmpnam` return `ENOSYS`. `fpos_t` is a `long`, so positions beyond `LONG_MAX` are not representable. `%s` input without a field width retains the standard caller-buffer responsibility. Floating `scanf` input items are limited to 65,536 bytes to bound temporary storage; longer items return a matching failure with `EOVERFLOW`. |
| Output/error boundary | Formatted output writes through `FILE`; standard output is buffered and normal `exit` attempts to flush open output streams after `atexit` callbacks. Flush failures cannot change the exit status. `quick_exit` and `_Exit` do not flush streams. Flush means bytes were accepted by native I/O, not durable media persistence. `errno` is set at libc boundaries; native `mg_result_t` remains distinct and `mg_error_to_errno` is an explicit, potentially lossy mapping. EOF is handled before mapping because it is a separate native result. |
| `ctype.h`, `errno.h`, `assert.h`, termination | ctype implements the ASCII C locale; errno is one process cell because user processes currently have one user thread. `atexit` and `at_quick_exit` retain up to 32 callbacks each, in reverse registration order; normal CRT return runs `exit`, which invokes callbacks and attempts to flush stdio before native process teardown. `_Exit` skips callbacks and flushing; abort traps because signals are unavailable. Assertions print a diagnostic then abort. There is no TLS or multi-thread errno contract. |
| Locale and conversion | Only C-locale classification/conversion is implemented; `MB_LEN_MAX` is 1. There is no locale selection, Unicode ctype, C multibyte API, normalization or collation service. |
| Other ISO facilities | Math, fenv, complex, setjmp, signal, stdalign, stdatomic, stdnoreturn, tgmath, threads, time, uchar, wchar and wctype facilities remain absent. No complete hosted-library claim is possible. |
| Toolchain/runtime helpers | Native links use GNU ld and the explicit Mangrove archive, without host CRT/default libraries. The kernel shares only explicitly listed freestanding `string.c` and `units.c`, so userspace runtime objects cannot enter the kernel build through discovery. Required compiler arithmetic helpers are not supplied by a general runtime archive; TLS images are unsupported. |

The kernel separately compiles shared string/units code and calendar
conversion. The Makefile lists the kernel-shared libc sources explicitly,
preserving the kernel's no-SSE compilation and preventing userspace-only runtime
objects from entering the kernel archive.

`make test-libc` runs the host-side runtime probes under ASan/UBSan. The stream,
format and scan probes compile production libc sources against a controlled
native-I/O adapter and compare supported results with the host C library.
`make test-guest-mgfs` also builds and launches the real Ring 3
`fstest stdio RUN [TARGET]` phase in QEMU, then checks the disposable MGFS image
with `mgfsck` and exact expected bytes. These are host and QEMU checks; they do
not establish physical-hardware, SMP, or power-loss behavior.

## Time: current guarantees and defects

See [timekeeping](../kernel/timekeeping.md) for the source contract. Native
realtime is signed seconds plus a canonical nanosecond fraction from UTC
**2000-01-01**. Monotonic time is boot-local milliseconds. The kernel samples a
validated CMOS calendar only under explicit RTC-as-UTC deployment trust;
untrusted or invalid clocks remain unavailable. Whole-second advancement does
not manufacture subsecond precision or prove clock accuracy. There is no clock
setting, timezone, DST database or synchronization service. Timer-derived elapsed
time is not process CPU time.

`libc/src/time_convert.c` supplies proleptic Gregorian conversions, including
negative dates, leap-year rules and range checks. It rejects leap-second input
(second 60); there is no leap-second/TAI model. Calendar years are signed 32-bit;
not every signed-64-bit second value can be converted to that calendar.

**Known interoperability defect:** `mangrove_time_to_unix` and its inverse copy
seconds unchanged. Unix 1970 and Mangrove 2000 differ by **946684800 seconds**.
For 2000-01-01 the current helper returns Unix 0 instead of 946684800. Existing
`tests/timekeeping_test.c` repeats the same incorrect identity expectation;
round-trip success is insufficient evidence. There are currently no production
callers of these two conversion helpers. Do not use them for interoperability
until checked addition/subtraction and independent epoch vectors are implemented.
The native clock, log records and filesystem representations must not be migrated
as a side effect of that correction.

exFAT uses native calendar conversion, not the Unix helper, with an explicit
1980–2107 admission range, UTC fields and whole-second increment encoding in
retained transactions. Ordinary reads preserve access timestamps. FAT32 date
fields are not backed by a shared clock-population path. MGFS's supplied
superblock nanosecond fields are not a general VFS file-time API. Native
`mg_path_info_t` exposes no per-file timestamps; adding `<time.h>` does not
create that facility or authorize rewriting imported filesystem dates.

## Text: current guarantees and boundaries

| Layer | Actual behavior |
| --- | --- |
| Standard byte strings | Lengths and offsets are bytes; no implicit decoding, casing or normalization. |
| Kernel `utf8.c` | Streaming replacement decoder, at most two output scalars per byte. It rejects overlong, surrogate and out-of-range completed sequences, preserves a following ASCII byte, and retains partial sequences. It has no finish/EOF operation; it is not a strict pathname validator or C multibyte API. |
| Reed `buffer.c`/`editor.c` | Strict scalar validation, UTF-8 encoding and codepoint-oriented navigation; duplicated buffer/frame decoders. File loading rejects embedded NUL and malformed UTF-8. Codepoint movement is not grapheme-cluster editing. |
| Shoot completion/line editor | Completion has separate width and decode helpers; the decoder accepts overlong encodings and surrogates. Line-editor cursor/delete operations use byte positions while display accounting counts non-continuation bytes. These helpers must not be reused for identity or strict validation. |
| Console/terminal/font | Raw key events carry codepoints; the legacy input path casts values up to 255 to single bytes, not UTF-8. Output decodes UTF-8 into a fixed one-cell-per-codepoint grid with font fallback. No general combining, wide-cell, grapheme or shaping model is established. |
| MGFS | Strict UTF-8, case-sensitive byte comparison, no normalization, on-disk 255-byte name limit. That format limit remains even after a native ABI extension. |
| FAT32 | Validated UTF-16 LFNs, pinned Unicode 15.1 simple uppercase and conservative OEM alias policy; no universal full folding or normalization. See [names](../filesystem/fat32/names.md). |
| exFAT | Validated UTF-16 entry sets and the volume's validated upcase table/hash. A maximum 255-unit name needs at most 765 UTF-8 bytes. Internal parsing includes those names independently of native representability. |
| Native paths/enumeration | Components and returned names currently allow 255 UTF-8 bytes. Checked enumeration distinguishes errors from EOF, consumes an unrepresentable entry and defers errors after completed batch entries. Anchored CWD/mount identity remains authoritative. |

The libc has no shared Unicode codec or multibyte conversion interface. The
path-specific decoding, validation, and presentation behavior above remains in
its owning subsystem; standard byte-string functions do not validate text.

## Runtime architecture boundaries

- Compiler-derived ISO types and Mangrove-native fixed-width aliases are
  intentionally separate. Native syscall results and structures keep their
  existing ABI; C errno conversion is explicit and lossy.
- Userspace builds as GNU C17, while kernel and boot code retain their own
  freestanding flags. Only byte strings and units are shared with the kernel;
  allocation, errno, formatting and process wrappers remain userspace-only.
- The supported character locale is ASCII C. Native text and paths remain
  byte-preserving UTF-8 at their existing API boundaries; no locale selection,
  normalization, Unicode ctype or multibyte conversion service is implied.
- The errno cell is process-local for the current single-user-thread process
  model. This does not establish TLS or concurrent user-thread semantics.
- Normal `exit` runs registered callbacks, attempts to flush open writable streams, then
  delegates to native process teardown. This does not provide global VFS sync,
  disk quiescence or crash durability. `quick_exit` and `_Exit` skip stream
  flushing.
- `FILE` wraps native object handles and preserves their EOF, confirmed-count,
  error and uncertainty contracts. It does not retry an uncertain output. Time
  and filesystem-specific text rules are documented at their owning interfaces.
- Mangrove supplies a useful C library subset, not a complete hosted C17 or
  POSIX environment. Stream rename replacement, temporary files, wide I/O,
  locale selection, and several other standard facilities are unsupported as
  described above.

## Standards references and claim discipline

[WG14 project status](https://www.open-std.org/jtc1/sc22/wg14/www/projects.html)
identifies C17 as ISO/IEC 9899:2018. The public
[N1570 draft](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf)
provides reference clauses for many retained library interfaces but is a C11
draft, not formal C17 conformance evidence. This document describes a bounded
implemented subset; Mangrove has not completed a formal hosted C17 conformance
assessment. The [C11/C17 issue log](https://www.open-std.org/JTC1/SC22/WG14/issues/c11c17/log.html)
records later clarifications. The
[Unicode conformance](https://www.unicode.org/versions/Unicode17.0.0/core-spec/chapter-3/)
distinguishes encoding validity from casing, normalization and text segmentation.
Consulting that reference does not update Mangrove's pinned filesystem tables.
