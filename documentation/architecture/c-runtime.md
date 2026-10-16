# C runtime, time and text contracts

## Current runtime boundary

Mangrove has a statically linked native C runtime, not a complete hosted C17
implementation. Native applications currently compile as GNU C11, freestanding,
without builtins, PIC/PIE, red-zone use or host standard headers. `USER_CFLAGS`
in the root Makefile currently also exposes `kernel/include`; this is a build
convenience, not a supported application/kernel interface.

`libc/crt/crt0.s` receives the native loader's register arguments, aligns the
stack, calls `main`, and passes its result directly to `process_exit`.
`kernel/src/process.c::setup_user_stack_args` installs argv strings and a NULL
sentinel; `process_user_thread_entry` enters Ring 3. This is not a Linux initial
stack, environment or auxiliary-vector ABI. Both public spawn forms currently
copy into a 256-byte argument budget with at most 16 arguments; the legacy
command-line parser stops at the argument limit, whereas argv requests reject
excess counts. These boundary semantics need explicit validation tests. There is no libc initialization,
`exit`/`atexit`, stream flushing or constructor/finalizer walk. Static initialized
objects and zero-filled storage are supplied by ELF load segments.

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
PT_TLS or a dynamic linker. The linker script does not explicitly manage TLS,
initialization arrays or unwinding. Those execution features are unsupported,
not established merely because GCC can emit them.

## Library inventory and known gaps

Only six ISO-named headers are present in `libc/include`. Compiler-provided or
host headers are not searched by the native userspace build (`-nostdinc`).
Host-only fsstress's POSIX adapter uses the host libc; its additional headers
are not Mangrove facilities.

| Area and authoritative code | Actual behavior and boundary |
| --- | --- |
| `stdint.h`, `stddef.h`, `stdarg.h`; `include/types.h` | Fixed-width integer aliases, size/ptrdiff, NULL and compiler va-list intrinsics. Standard least/fast/max types, limits/literal macros, `offsetof`, `max_align_t`, `wchar_t` and `va_copy` are incomplete/absent. Standard headers import project aliases and bool macros. |
| Target type mismatch | Target compiler `sizeof`/pointer-difference types are `unsigned long`/`long`; current `size_t`/`ptrdiff_t` use `unsigned long long`/`long long`. Width equality does not establish C type compatibility. Native `u64`/`i64` intentionally remain long-long aliases; changing standard headers must not silently change wire types or existing `%llu` arguments. |
| `string.h`, `src/string.c` | Byte strlen/comparison/copy/concatenation and memory routines, including overlapping `memmove`. No general Unicode semantics. Many standard search/token/error/collation functions are absent. `strncat` tests the count before reading the source, including non-NUL-terminated bounded arrays. The historical source read one byte beyond such an array. |
| `stdlib.h`, `src/allocator.c` | malloc/calloc/realloc/free only. First-fit process-local lists, 16-byte payload alignment, split/coalesce and checked allocation-size arithmetic. calloc checks multiplication; failed realloc preserves the original object. Zero-size allocation returns NULL; realloc-to-zero frees. These zero-size choices must be documented rather than mistaken for all allocation failures. |
| Allocator lifetime | Anonymous page mappings are retained until process teardown; free recycles blocks internally. First-fit/free lookup cost scales with block count. No allocator locks, standard aligned allocation, errno or user-thread concurrency contract. Current public process APIs do not create multiple application threads in one address space. Region reclamation and allocator redesign are separate work. |
| `stdio.h`, `src/stdio.c` | putchar/puts/printf/snprintf only; output uses the inherited native stdout object. Formatting has decimal/hex integers, strings, chars, pointers, literal percent, limited width/padding and long-long handling. No FILE, input scanning, precision, full length/flag handling, floating conversion, v-functions, EOF constant or error indicators. Native stdin/stdout exist, but there is no distinct native stderr constant. |
| Demonstrated formatting failures | `%ld` and `%lu` read int-width arguments; a value of 4294967297 prints 1. `%zu` is emitted literally. Unsupported conversions can leave varargs unconsumed. A trailing incomplete format can advance past its terminator; invalid-format robustness is separate from defined-input conformance. Large width/count limits and return overflow need explicit review. |
| Output completion | printf buffers 128 bytes and treats a short native write as an error; snprintf counts truncated output and terminates nonempty buffers. Negative native errors are not a complete C stream/errno contract. `native.c::object_write_all` handles positive prefixes but does not make output atomic or recover an uncertain mutation. |
| Classification, conversion, locale | No ctype/locale/errno headers, general strto*/atoi routines or C multibyte interfaces. Applications have bounded domain-specific parsers. There is no locale selection or normalization service. |
| Other ISO facilities | No assert, float, inttypes, iso646, limits, math, fenv, complex, setjmp, signal, stdalign, stdatomic, stdbool, stdnoreturn, tgmath, threads, time, uchar, wchar or wctype headers. No complete hosted-library claim is possible. Optional facilities and missing required facilities must not be conflated. |
| Toolchain/runtime helpers | Native links use GNU ld and an explicit Mangrove archive, without host CRT/default libraries. Required compiler arithmetic helpers are not supplied by a general runtime archive. `nm -u`/link checks and target ABI tests must accompany new compiler features; do not import a host libgcc or advertise an unsupported TLS image. |

The kernel separately compiles shared string/units code and calendar conversion.
The Makefile currently discovers libc source files for the kernel using an
exclusion list. Adding a userspace-only libc source could therefore accidentally
pull it into the kernel. Explicit shared-source membership is a prerequisite
for substantial library growth, preserving the kernel's no-SSE compilation.

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

Shared codecs should distinguish invalid input, incomplete input and output
capacity, with explicit consumption and EOF behavior. Strict identity paths
must reject rather than replace; presentation may choose bounded U+FFFD recovery.
Valid unassigned scalars/noncharacters are not automatically malformed Unicode;
format-specific exclusions remain with their filesystem. Escaping control bytes
for a listing is presentation policy, not permission to change stored names.

## Recommended runtime contracts

- Keep standard headers free of native aliases; derive C types/limits from the
  target compiler. Keep native fixed-width ABI structs and `MG_ERR_*` interfaces
  independently stable. Standard and native headers need one definition of
  `uintptr_t`, with all include orders tested. Move userspace/runtime to GNU C17
  after correction, test standard-facing headers in strict C17, and preserve
  the separate kernel/boot compilation policies.
- Use explicit shared, allocation-free calendar/codec primitives; compile them
  for kernel and userspace with their respective flags. No kernel dependency on
  FILE, locale state, userspace allocation or syscall wrappers.
- Start in a documented ASCII C locale. Provide explicit UTF-8 codecs
  independently; a small explicit `C.UTF-8` locale may enable standard multibyte
  conversion without changing numeric punctuation or filesystem matching.
  Native OS text remains UTF-8 regardless of C conversion locale; byte I/O is
  not transcoded. Unsupported locale names must fail, not silently select another
  locale.
- Use an errno accessor scoped to the supported execution context, leaving room
  for future TLS. Do not claim general TLS/threads or introduce them implicitly.
  Native APIs keep their detailed statuses; C adapters must retain uncertainty
  diagnostics and never automatically retry an ambiguous mutation.
- FILE owns or explicitly borrows a native handle. Distinguish EOF, confirmed
  prefixes and errors; serialize its position/buffer state. Standard open modes
  need a sound identity-bound create/truncate/append contract. Separate seeks
  must not be advertised as atomic append. Define stderr routing/ownership and
  keep text/binary file bytes unchanged, without implicit Unicode or newline
  normalization. Console admission failure returning
  zero in `console_read_bytes` also needs a checked adapter before being treated
  as stream EOF. Neither fflush nor process exit means global disk quiescence.
- A proposed signed-64-bit `time_t` should retain the native epoch, with UTC-only
  local civil time initially. `timespec_get(TIME_UTC)` must honor availability and
  real precision. `clock()` must report unavailable until process CPU accounting
  exists, never substitute uptime. Unix interoperability stays explicitly typed.
- Use bounded buffers, checked arithmetic, initialization-before-publication and
  failure-preserving realloc/stream/codec state. Keep ordinary streams distinct
  from the existing 2 MiB snapshot/replacement API and runtime recovery owners.

## Standards references and claim discipline

[WG14 project status](https://www.open-std.org/jtc1/sc22/wg14/www/projects.html)
identifies C17 as ISO/IEC 9899:2018. The public
[N1570 baseline](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf)
provides reference clauses for retained C library interfaces; it is a C11 draft,
not a substitute for the C17 corrections and required conformance ledger.
The [C11/C17 issue log](https://www.open-std.org/JTC1/SC22/WG14/issues/c11c17/log.html)
is needed when resolving disputed semantics. The final C17 draft was not
available during this review, so this is not formal certification.
[Unicode conformance](https://www.unicode.org/versions/Unicode17.0.0/core-spec/chapter-3/)
distinguishes encoding validity from casing, normalization and text segmentation.
Consulting that reference does not update Mangrove's pinned filesystem tables.

The [runtime implementation plan](../development/runtime-plan.md) specifies
bounded work and qualification gates. A documented application-runtime profile
is not a claim that every required hosted C17 facility exists.
