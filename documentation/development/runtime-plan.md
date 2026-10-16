# Runtime implementation and qualification plan

## Acceptance target

Establish a coherent C17 application-runtime baseline over Mangrove's native
interfaces. Preserve working applications and filesystem contracts. Maintain a
header/function/feature ledger of implemented behavior, implementation-defined
choices, tests and unimplemented work. No header, compiler flag or milestone
alone establishes full hosted C17 conformance.

The [architecture inventory](../architecture/c-runtime.md) records the current
implementation and supported subset. Stage 3B establishes the compiler ABI, core
integer formatting/conversion, ASCII C classification, errno mapping and
termination baseline. Required-but-missing hosted facilities remain explicit
runtime work; this bounded profile is not full hosted C17 conformance.

## Ordered implementation slices

| Slice | Bounded deliverable and dependencies | Risks, verification and completion |
| --- | --- | --- |
| **3B — Core C ABI and runtime foundations (implemented)** | Compiler-derived standard types/limits/alignment/varargs headers; separate standard and native ABI types; explicit shared-source membership; corrected supported integer formatting; integer conversion, ASCII C ctype, errno mapping, allocation failure reporting, assertions and bounded termination callbacks. | Strict C17/header probes, differential format/conversion checks, ASan/UBSan allocator/termination fixtures, native fstest runtime smoke and GNU C17 forced target build. Full hosted C17 support is not implied. |
| **3C — Formatting and native streams** | Add reusable v-format entry points and floating/numeric conversions, then FILE/stdio input and output with explicit handle ownership, buffering, mode and error state. Reuse the native object/VFS architecture. Establish any minimal shared open-intent/append and checked console error prerequisites before advertising corresponding modes. Depends on 3B's integer formatter. | Test floating-format edge cases and scanning end positions against independent references. Test short I/O, EOF, cancellation, removal, UNCERTAIN, allocation failure, flush/close failure, stdin/stdout redirection and exit order. Real native files on all three filesystems, permissions, CWD identity and competing opens/appends must preserve ownership. Completion requires accurate statuses/counts and independent persisted-byte checks, not seek/write race emulation or a second file API. |
| **3D — Time and civil conversion** | Fix explicit Unix conversion with checked 946684800-second translation; define signed time_t/native epoch, struct tm, calendar/formatting and available C time interfaces. Keep monotonic native APIs distinct. UTC-only local time, no invented CPU clock or subsecond accuracy. No RTC, filesystem format or log migration. Can follow 3B while 3C is developed. | Independent 1970/2000 known answers, negative fractions, integer extrema, century/leap days, invalid calendars and untouched failure outputs; property/differential calendar tests with an explicit epoch adapter. Test trusted/untrusted/invalid RTC and backwards/overflow monotonic samples. Native whole-second clock/error checks; exFAT metadata regressions confirm existing timestamp publication. Done when interop is independently correct and unavailable/range/precision semantics are documented. |
| **3E — Shared Unicode and multibyte boundaries** | Allocation-free strict UTF-8 decode/encode/validation and bounded streaming/finish contracts; C-locale and explicit UTF-8 multibyte/wide interfaces, correct conversion state and error behavior. Replace duplicated unsafe decoding in small adapters, preserving Reed and terminal policies. Depends on 3B and standard-facing stream integration where wide I/O is exposed. | Exhaustive scalar encode/decode, split/truncated/overlong/surrogate/out-of-range cases, buffer limits, no lost following ASCII and UTF-16 surrogate-pair state for uchar interfaces. Distinguish EILSEQ from incomplete input, bytes from code units/scalars and cells. Native supplementary/combining text and invalid-input controls, exact file-byte preservation and filesystem-specific collision tests. Done when shared primitives and declared interfaces agree; this is not a grapheme-aware UI or shaping rewrite. |
| **3F — Length-aware native paths and enumeration** | Versioned pointer-plus-length path/enumeration requests with bounded required-capacity reporting; migrate relevant libc/application path buffers through existing anchored VFS resolution. Keep legacy ABI, including consumed/deferred NAME_TOO_LONG behavior. Support valid FAT32/exFAT 255-unit imported names without a 255-byte native disadvantage; preserve MGFS's 255-byte on-disk limit. Depends on 3E; metadata publication remains with existing drivers. | Production syscall/copy/cursor/namespace tests, overflow and NUL/invalid-UTF-8 negatives, maximum BMP and supplementary names followed by ordinary entries, sizing/retry serialization, removal and stale mount/CWD identity. New buffer-too-small retry semantics must be explicit and bounded, not silently consume or skip entries. Native two-boot lifecycle and independent checkers/content oracles across filesystems. Done when advertised names are fully accessible without changing matching, formats or recovery guarantees. |

These are implementation boundaries, not permission to start them during an
audit. Each requires its own reviewed contracts and applicable qualification.
A final baseline acceptance review must account for the remaining hosted-library
ledger rather than silently declaring complete C17. Additional required library
work stays tracked as runtime work before any full-hosted claim; this plan does
not place it in Stage 4 allocator redesign or Stage 6 SMP.

### Next implementation

Stage 3B is the current implemented baseline. The next slice is **3C**, which
extends formatting and defines native stream ownership, buffering, status and
EOF behavior. Preserve native u64/i64 aliases and packed request layouts as the
standard-facing library grows.

Do not wait for 3D to fix epoch conversion if a real interoperability consumer
is introduced earlier: implement its independent vectors and checked arithmetic
first. The current round-trip test incorrectly assumes identical epochs and
must be replaced for that claim. Native calendar/clock evidence remains useful.

## Verification architecture and reuse

Use MTS and existing tools, not a parallel qualification engine:

- `make test-libc` covers production string bounds, compiler/header ABI, integer
  formatting and conversion, allocator failures, errno, ASCII ctype and
  termination semantics with focused host fixtures. It is not a general libc
  conformance suite.
- `make test-time` covers native calendar/RTC/clock behavior, including controlled
  concurrent clock reads; it does not independently validate Unix conversion.
  `test-terminal` covers the kernel replacement decoder and overlay contracts;
  Reed tests cover strict buffer/save behavior. None establishes standard locale,
  FILE, scanf, floating-format or C multibyte conformance.
- `make test-heap` exercises kernel heap/process mapping behavior, not a broad
  userspace allocator conformance suite. Native application success does not
  replace direct malloc/calloc/realloc failure and alignment fixtures.
- Host tests must compile the actual libc sources with private symbol names when
  necessary; host libc interposition must not substitute for the implementation.
  Use independent expected bytes/statuses and reference comparisons only for
  defined inputs and declared implementation-defined policies. Do not encode a
  current bug into the oracle or certify undefined-input behavior accidentally.
- `test-guest-safe` executes an fstest runtime scenario through Ring 3 for
  integer formats, errno, allocation, floating-point arguments/returns and CRT
  `atexit` behavior. It reuses the production volume/application runner and is
  not broad runtime testing across every native application. Host adapters do
  not count as Ring 3 evidence; fsstress and MFQS are filesystem oracles, not
  libc standards oracles.
- Reuse retained integration reports after source/binary/configuration checks.
  Select new guest coverage when affected paths change; do not rerun every host
  or guest campaign merely for documentation. Comprehensive MTS execution remains
  unqualified end to end; manual hardware/network tools remain explicit.

The runtime tests compile the actual library sources under renamed symbols to
prevent host-libc substitution. They compare defined integer formats and
conversions against host references, and exercise allocation, errno and callback
behavior with controlled native dependencies. Existing string bounds coverage
retains its independent ASan/UBSan oracle and historical failing control.

## Scope and resource boundaries

Keep interfaces and recovery ownership intact. The allocator's first-fit
throughput, region reclamation and broad hardening belong to Stage 4. Full
terminal layout, grapheme movement, wide-cell rendering and richer editing
belong to Stage 5; keep byte-safe scalar boundaries now. User-thread/SMP
qualification belongs to Stage 6, hardware to Stage 7 and persistent recovery
to Stage 9. Time setting, timezone databases, NTP and broad internationalization
are separate prerequisites, not implicit C wrapper behavior. GUI text shaping
is not part of this runtime baseline.

New codecs should use constant small state and caller-owned buffers. Standard
streams need a documented per-stream buffer/object budget and failure-preserving
allocation, not a whole-file copy. Length-aware enumeration may retain one
bounded pending entry per cursor, not a whole-directory inventory. Preserve
2 MiB safe-file limits and the existing filesystem proof/transaction budgets.
Measure requested heap, syscall/I/O counts, native elapsed time and artifacts
for each implementation. A one-cell terminal does not become Unicode-width
correct by counting UTF-8 lead bytes, and a nanosecond field does not create
nanosecond resolution.
