# Focused test coverage

## Native terminal controls and `say` escapes

`make test-terminal` compiles the production terminal renderer under
ASan/UBSan and covers incremental ESC command parsing, malformed and
incomplete sequence recovery, per-process style state, literal writes,
terminal ownership, cursor operations, and clearing. `make test-say` tests the
production byte-escape translator, including `\\`, `\n`, `\r`, `\t`, `\e`,
unknown escapes, trailing backslashes, and UTF-8 preservation.

The disposable `guest_mgfs_test.py --shoot-v1` workflow exercises `say -e`
through the native console and checks framebuffer text and palette pixels.
The image is built under the selected build directory and independently
checked after shutdown. The terminal parser treats actual ESC bytes as native
commands only on ordinary console writes; `say` without `-e` and the explicit
literal-write API preserve displayed bytes. See the
[terminal behavior contract](../architecture/terminal.md).

## System CA root-store packaging

`make test-ca-store` validates the checked-in Mozilla-derived PEM snapshot and
its source/license manifest, rejects missing, empty, malformed, and digest-
mismatched stores, and exercises fresh plus incremental MGFS image creation.
It compares packaged bytes, ownership, permissions, idempotence, and
independent `mgfsck` results. The fixture uses disposable MGFS media.

`make test-guest-ca-store` builds an explicitly UTC-trusted test kernel and
boots a disposable Q35/OVMF two-vCPU guest. A native process reads each store
file, confirms ordinary writes are denied, then performs a reboot. A second
native boot repeats the checks; the stopped image is independently checked
and its exact trust-store bytes are compared with the reviewed source.

These tests establish packaging, access control, and persistence. They do not
exercise TLS certificate parsing, chain or hostname validation, source
authentication, or real hardware time/RTC behavior. See the [CA trust-store
contract](../security/ca-trust-store.md).

## mgTLS foundation

`make test-mgtls` runs wolfCrypt SHA-256, AES-GCM, and X25519 known-answer
checks under ASan/UBSan, exercises the Mangrove random-byte adapter including
partial-output clearing on entropy failure, and compiles a native TLS 1.3
client API probe. It verifies that the static archive contains the TLS 1.3
client method and omits the TLS 1.2 client method. Deterministic local TLS
peers exercise ECDSA and RSA server certificates, multiple trusted roots,
DNS/IP SANs, a TLS_AES_128_GCM_SHA256-only peer, fragmented records, a
3 MiB transfer, chain and certificate failures, untrusted time, timeout,
interruption, reset, and cleanup. Leak detection is disabled
because LeakSanitizer does not initialize reliably in this host environment.

`make test-guest-mgtls` builds a source-matched disposable Q35/OVMF guest with
two vCPUs (KVM by default on Linux, TCG on other supported hosts), runs the
test-only native crypto program against
`mg_random_bytes()`, and checks the stopped MGFS image independently. This
qualifies the selected operations and entropy wiring in that virtual machine;
it is not a cryptographic certification or a TLS handshake test. This target
does not establish certificate-chain or hostname validation, transport
callbacks, physical-hardware entropy quality, or production HTTPS readiness.
See the [mgTLS foundation contract](../security/mgtls.md).

`make test-guest-mgtls-tls` performs a native authenticated TLS 1.3 exchange
with `example.com`. `make test-guest-fetch-https` builds with
`RTC_UTC_TRUST=1`, downloads the site through both native HTTP and HTTPS
`fetch` paths, also checks HTTPS origins at `www.gnu.org` and `letsencrypt.org`,
follows `5x40.org`'s HTTP-to-HTTPS redirect, verifies content markers and
downloaded bytes, and independently checks the stopped MGFS image. Both use
disposable two-vCPU Q35/OVMF guests and require current public
DNS, network access, and the remote certificate chain; neither qualifies
physical hardware or provides cryptographic certification.

`make test-x86-cache` (also included in `test-vmm`) executes the production
PAT/MTRR policy with adapters for privileged instructions. ASan/UBSan cover
actual PAT selectors, loader 4 KiB/huge-page use of the reserved slot, AP
consistency and rejection, strict UC fallback, MTRR masks and overlaps,
partial boundaries, arithmetic limits and readback failures. Hardware cache
flushes, MSR instructions and effective nested-page attributes require native
verification. Leak detection is disabled; LeakSanitizer is not qualified.

`make test-framebuffer-mapping` (included in `test-vmm`) executes the production
boot GOP constructor, physical alias walker and read-only PCI eligibility
probe. It covers 32/64-bit BARs, disabled/register/non-display resources,
multifunction discovery, image arithmetic/alignment, inactive-root/CPU/IRQ
restrictions, 4 KiB/2 MiB/1 GiB aliases, later UC alias refusal and partial
allocation failure. The main VMM fixture also tests canonical framebuffer
cache-selector enforcement and user-alias rejection in the actual leaf mappers.
The framebuffer-copy fixture checks SFENCE completion once per WC region, no
UC fences, and MFENCE ordering before direct VRAM scrolling and initial
backbuffer reads using a pending-store adapter. Native cache attributes,
performance, actual fonts and framebuffer pixels remain separate evidence.

The [Mangrove Test Suite (MTS) guide](test-suite.md) is the inventory and
aggregate execution/reporting reference. This page details focused production
paths and fixture boundaries. `make test` retains the host suite; `test-guest`
and `test-all` explicitly select additional guest qualification.

Linux guest tests default to KVM. Select TCG explicitly with `MTS_ACCEL=tcg`
for MTS, the relevant `*_ACCEL=tcg` Make variable, or a direct runner's
`--accel tcg` option; non-Linux hosts retain their existing TCG default. KVM
requests fail with an actionable diagnostic if QEMU lacks KVM support or
`/dev/kvm` is absent or inaccessible; there is no TCG fallback.

On Linux, `make test` runs the host suite; on macOS, use `gmake test`.
The normal macOS suite uses native Homebrew GNU GCC without temporary source
or compiler wrappers. Test-only linker selection uses Darwin `-dead_strip`
instead of GNU `--gc-sections`. Linux keeps its GNU flags and Reed malloc
wrapping. Darwin's Reed fixture injects a header into all participating
translation units to intercept the same malloc calls, including allocation
failure injection; target Reed code has no test allocator dependency.
The shared uintptr_t definition uses the SDK-compatible compiler type only
for native Darwin builds; cross-target and Linux types remain unchanged.
ARM Darwin boot-parser fixtures use explicit native callbacks; Linux/Intel
fixtures retain the x86-64 Microsoft ABI. Neither native parser fixtures nor
binary compilation substitute for a UEFI guest boot. The VFS recovery fixture
uses actual pthread workers with a cyclic mutex/condition barrier on Darwin,
and native pthread barriers on Linux; its reuse is tested for 1,000 rounds.
No sanitizer or failure assertion is removed for portability.

`make test-pager` also connects the production shared pager to the production
terminal and a pixel framebuffer fixture. It checks one-row scrolling work,
unchanged-view output, rapid ordered j/k movement, page/document movement,
narrow and wrapped UTF-8/tab layouts, search highlighting, partial output
failure/retry, and normal-screen restoration. The same pager is linked into
both `view` and `info`; their focused command tests remain separate. The
157-column, 43-row numbered-line fixture emits fewer than 200 bytes and glyphs
for a one-row scroll, rather than rewriting more than 6 KiB of complete rows.

The ordinary write/truncate pin fixture executes production VFS dispatch,
post-unlock handoff, file-object translation, and native syscall result
translation with controlled filesystem callbacks and mutexes. It covers
confirmed partial writes, uncertain outcomes, failed pin finishing, device
detachment, and pin exhaustion; the controlled VFS fixture does not itself execute a filesystem metadata commit
or a guest device.

It covers timekeeping, terminal, Reed, MGFS version admission and fresh/update
image contents (including every installed quick-help record and application
manual), VMM, heap, the slab core, history, synchronization, storage,
development-image guard, and password tests.

`make test-mgfs-version` also runs `mgfs_checker_records_test.py`. The production
VFS/MGFS and formatter generate a 210-slot fixture, fill its table, reject
ordinary and conditional creation without changing its bytes, then delete and
reuse slots with new IDs beyond table capacity. Remount and enumeration verify
those identities. Both table exhaustion and exhaustion of assignable 64-bit
IDs must return `NO_SPACE` without publishing a node or changing the image.
Legal identity-gap fixtures exercise IDs above 32 and 63 bits
and the final assignable 64-bit ID. Sanitized checker negative controls retain
checksum-valid dangling references, duplicate IDs, out-of-range identities,
cycles, shared references and freed backing; unreachable owned files remain
warnings. Both driver and checker run under ASan/UBSan with leak detection
disabled. These are host tests, not power-loss or native SMP qualification.

A bounded two-vCPU Q35/OVMF KVM check also exercised a full 1,071-slot
native image: file and directory creation returned `NO_SPACE`, deleting one
file allowed creation with a fresh ID above slot capacity, and the child exited
zero before orderly shutdown. The independently checked stopped image had all
1,071 Records reachable with zero errors or warnings. This does not establish
physical-hardware or power-loss behavior.

`make test-mgfs-reconcile` exercises the production image updater against a
populated MGFS image. It verifies that a stale free Record whose data extent
has a unique live Record owner can be cleared without changing the live block,
that an already released extent is reconciled, and that an allocated extent
with no live owner remains a hard error with no in-memory image mutation.
The resulting safe fixtures are independently checked with `mgfsck`.
It also checks full updates containing a large-ID user file, missing manuals
and a missing manual directory after small IDs have been consumed. File bytes,
identity and ownership survive updates; invalid ID watermarks and exhausted
64-bit identities fail without publishing changes. Repeated updates are
byte-stable. These updater tests run on disposable host images.

`make test-help` runs the production quick-help parser and renderer against
synthetic section combinations and every checked-in `/share/help` record under
ASan/UBSan. It verifies optional-section omission, narrow-terminal wrapping,
and legacy records containing only usage and options. It is a focused host
fixture; it does not boot the packaged image. A source-record audit checks
section classification, the declared option set for all 44 commands, and the
interactive controls documented for the pager, Reed, TMon, and disk utility.

| Target | Production code exercised | Test boundary |
| --- | --- | --- |
| `test-pmm` | Production PMM frame accounting and explicit generation-owned frame operations | ASan/UBSan checks zeroed acquisition, physical exhaustion, raw-free and reservation rejection, borrowed/reserved/forged/stale handle rejection, mapping pins, double transitions, frame reuse, and two concurrent releases of copied handles. It also fills all 2,048 ownership records while ordinary frames remain available and verifies clean registry-exhaustion failure and restored accounting. LeakSanitizer is disabled for this fixture and is not qualified. |
| `test-vmm` | User mapping/path allocation, production kernel page-table subtree construction, owned kernel virtual-range leases, bootstrap-map teardown, unmap state transitions, the production TLB shootdown sender/handler, and AP startup-failure membership handling | ASan/UBSan fixtures check mapping ownership, owned-map rollback, bootstrap teardown preserving its reserved leaf while reclaiming empty page-table frames, rejection of a mismatched leaf without mutation, lease bounds/generations, validation-before-clear, concurrent map/release rejection during unmap, owned-frame pin retention until the simulated shootdown returns, fail-stop behavior on missing acknowledgment, IRQ-disabled and spinlock-held refusal, online-target selection, per-page invalidation before acknowledgment, a remote CPU using a different CR3, malformed-request rejection, and that a BSP startup timeout does not withdraw a possibly executing AP. The sender/handler protocol is simulated on the host; it is not hardware SMP evidence. LeakSanitizer is disabled for these fixtures and is not qualified. |
| `test-idt` | Production IDT/TSS setup and CPU-register frame helpers | ASan/UBSan checks IST/TSS ranges, all vector values, kernel/user hardware SS:RSP selection, interrupted-stack alignment, and complete long-mode frame offsets; it does not emulate a hardware interrupt or scheduler restore. |
| `test-heap` | Production heap initialization, allocation, free, bin lookup, coalescing, accounting, four-page growth, and bounded tail reclamation | ASan/UBSan with pthread-backed locks, simulated PMM/VMM mapping, failure injection, randomized ownership and concurrent workloads. Checks frame release on initial PMM/leaf-map failures; includes pre-scheduler stack-sized allocation, bootstrap exhaustion, partial PMM batch release, and zero/insufficient/sufficient partial VMM growth. Deterministic growth interleavings verify that old capacity remains usable before publication, competing growers serialize across a sufficient partial prefix, and published page addresses do not overlap. Reclamation fixtures keep live tail groups mapped, withdraw only a complete owned tail, hold the withdrawn region unavailable during simulated shootdown, validate rollback on VMM failure, verify PMM release and frame reuse on recommit, and exercise concurrent free to the left of a pending trim. Additional pressure checks cover unavailable generation-owned backing with raw-PMM fallback, lease-reservation failure after frame acquisition, preservation of live data on PMM exhaustion, and recovery after frame supply returns. Isolated child-process controls require fatal handling of duplicate insertion, cyclic/broken bins, bitmap and membership damage, physical-chain/header corruption, accounting mismatch, and invalid frees. The independent model checks payload accounting, physical coverage and live-range overlap. A seeded 20,000-operation mixed-size churn workload preserves long-lived anchors, checks live payload sentinels, and independently reconstructs ownership and accounting throughout. The fixtures use pthread adapters; they are not kernel SMP qualification. |
| `test-slab` | Production `kernel/src/slab.c` operations, sparse classifier, shared slabs, per-CPU caches, and owned backing lifecycle in both validation modes | ASan/UBSan with pthread lock/CPU-index adapters and PMM/VMM backing mocks. Covers all twenty shared and local classes; cache hits, refill contention, overflow/drain, cross-CPU frees, classifier publication/retirement, generation reuse, frame and lease reuse, acquisition/map failure rollback, injected metadata failure, simulated CPU migration, and ownership reconstruction. It injects an unmap failure during final-object retirement, verifies backing ownership remains pending, checks reclamation refusal in a spinlock-held context, and then retries successfully. The mocks verify resource ownership transitions but do not establish Pith SMP/TLB behavior. |
| `test-allocator-routing` | Public `kmalloc`/`kfree` dispatch in the production heap/slab implementation | Builds indexed-only and hybrid-256 host fixtures, checks every supported class, pre-initialization heap ownership, indexed large-request routing, alignment, zero/overflow behavior, free ownership, invalid/double/interior-free negative controls, and hybrid bounded pressure recovery. The hybrid fixture also forces PMM exhaustion, verifies a clean small-request slab miss falls back to heap ownership and is freed there, then restores PMM supply and verifies slab routing resumes. It does not test a kernel `krealloc` API because none exists. |
| `test-heap-profile` | Production diagnostic profile accounting in `kernel/src/heap_profile.c` and independent snapshot parser | ASan/UBSan checks exact size buckets, failed requests, address reuse, outstanding objects, cross-CPU frees, success/failure/free execution-context totals, phases, class estimates, deterministic admission-versus-commit interleaving, in-flight freeze, bounded-table loss, counter saturation, and pthread workers. Parser controls reject incomplete, malformed, or internally inconsistent snapshots. This validates instrumentation logic, not slab allocation. |
| `test-history` | Shoot history/ring logic and production `shell_exit()` cleanup; VFS and MGFS merge/snapshot; MGFS write/truncate, spanning tombstones, record-ID/create lifecycle, and 2 MiB private replacement and creation staging | Shutdown fixture verifies pending shell history is appended by the same exit path used for normal exit and reports write failure. The history fixture models old-only migration on first write, both-path conflict, empty-home behavior, malformed legacy contents, and read failures; persistence is mocked here, so native media behavior still requires a guest workflow. Filesystem fixtures cover controlled hardware, not crash recovery. |
| `test-shoot-config` (included in `test-shoot`) | Production Shoot config parsing, path selection, defaults, and legacy copy | ASan/UBSan host fixture verifies optional defaults, canonical and whitespace-tolerant assignments, all-or-nothing validation for first/middle/last errors, duplicate and unknown keys, value bounds, dynamic files beyond 2 KiB, allocation/open/read/close failures, new-path precedence, non-overwrite migration, and preservation of valid legacy settings on migration failure. Filesystem behavior is modeled; native MGFS installation is not covered by the host fixture. LeakSanitizer is disabled and is not qualified. |
| `test-app-directory` | Production per-user application-directory path and explicit creation helper | Host adapter verifies all four directory kinds, read-only path lookup, explicit creation, existing-directory reuse, invalid names/kinds, bounded paths, system identity rejection, and authenticated home matching. Native filesystem ownership depends on the process identity and directory permissions. |
| `test-plant` | Production `plant` argument handling and per-file creation result handling | Host filesystem adapter covers one/multiple targets, duplicate names, a failure between successful targets, invalid operands, unknown options, missing operands, and quick help without creating files. Native filesystem behavior is not covered by this fixture. |
| `test-mkdir` | Production `mkdir` argument handling and per-directory creation result handling | Host filesystem adapter covers one/multiple targets, duplicate names, a failure between successful targets, invalid operands, unknown options, missing operands, and quick help. Native filesystem behavior is not covered by this fixture. |
| `test-shoot` | Production Shoot tokenizer, variable/condition evaluator, script loader/interpreter, built-in arity dispatcher, external command launcher, and redirection path | ASan/UBSan host fixture executes `tests/userspace/shoot_v1.shoot` and covers quotes/empty args, expansion, comparisons, block execution, child status and spawn failures, redirection, script stop-on-error, 6,000 statements, a 16 KiB source line, and allocation-failure cleanup. Process and filesystem calls are controlled adapters; native execution is covered by `guest_mgfs_test.py --shoot-v1`. LeakSanitizer is disabled and is not qualified. |
| `test-list` | Production `list` filename formatter and column layout | ASan/UBSan host fixture checks plain names, spaces, punctuation, quoting and escaping, Unicode whitespace, malformed UTF-8, directory suffix placement, and a terminal-width boundary where quote bytes change the selected column count. Filesystem and terminal output are controlled adapters; LeakSanitizer is disabled and is not qualified. |
| `test-cli-options` | Shared native command-line option parser | ASan/UBSan checks grouped boolean short options, attached and separated value arguments, long `--name=value` and `--name value` forms, `--`, option values beginning with `-`, unknown options, missing values, and malformed boolean values. `test-list` depends on this target and separately verifies equivalent `list -lt`, `list -tl`, and `list -l -t` output through the production command parser. LeakSanitizer is disabled and is not qualified. |
| `test-process-interrupt` (included in `test-sync`) | Production foreground-process interrupt selection | ASan/UBSan fixture executes the production session/parent wait-chain selection and verifies deepest-child targeting, one-shot consumption, rejection of ambiguous sessions, and cross-session protection. `test-terminal` exercises the production HID-to-console Ctrl+C route for Shoot's raw line editor, foreground raw applications, cooked input isolation, and interruptible console reads. These host fixtures do not establish real USB scheduling behavior. |
| `test-net` | E1000 context/TX ownership, IPv4 route/ARP, ICMP state, TCP stream send/receive/close, userspace network object lifecycle, and production ping argument/scheduling path | ASan/UBSan host fixtures cover TCP multi-segment ACK gating, delayed and missing ACKs, retransmission exhaustion, duplicate receive data, advertised-window updates, half-close, FIN/TIME_WAIT reuse, reset, timeout, interruption, device-generation invalidation, partial-write/error mapping, and close-versus-active-operation ownership. They also cover ping's options and monotonic scheduling. LeakSanitizer is disabled and is not qualified; native network behavior requires a guest workflow. |
| `test-fetch` (included in `test-net`) | Production HTTP/HTTPS URL parsing and redirect resolution, CLI parsing, HTTP framing, transport selection, lifecycle/progress formatting, and bounded response write coalescing | ASan/UBSan checks bare and explicit HTTP/HTTPS URLs, default ports, relative and protocol-relative redirects, HTTP-to-HTTPS upgrades, HTTPS downgrade rejection, URL-list/output/quiet/timing option forms, `--`, fragmented HTTP headers and bodies, Content-Length completion without an extra receive, chunked and close-delimited bodies, interim responses, non-2xx status, multi-megabyte streaming, malformed/truncated responses, incomplete bodies, sink failure, and TLS transport selection/partial I/O/fail-closed errors without plaintext fallback. Display fixtures check stage ordering, authenticated-TLS wording, metadata, known/unknown-length progress, quiet mode, and clean progress/error transitions. A 3 MiB sink fixture verifies that fragmented data is coalesced into bounded 32 KiB writes and that failed chunks remain uncommitted. The two-vCPU `guest_mgfs_test.py --network-ping-smoke` workflow retains HTTP compatibility checks; `make test-guest-fetch-https` verifies normal lifecycle output, captures an in-place progress update during a multi-megabyte authenticated HTTPS download, verifies downloaded bytes against fixed reference hashes, and independently checks the stopped MGFS image. |
| `test-fsstress` | Portable C workload/model, native trace format and POSIX adapter | ASan/UBSan, separate Python byte/namespace oracle, replay and controlled transport/parser failures; actual native guest acceptance is opt-in `test-guest-stress`. |
| `test-mfqs` | Complete VFS and MGFS/FAT32/exFAT units: mount, creation/recovery handoff, ordinary write/read/resize, snapshot/conditional replacement, nonempty exclusive creation, nested namespace lifecycles and real close/unmount/remount | Independent Python byte model, evidence-gated quick/extended campaigns, exact replay, disposable independently checked images, poisoned slack/free storage and historical negative controls; controlled single-threaded hardware/context/clock dependencies. Broader qualification remains incomplete; missing tools report INCOMPLETE. |
| `test-sync` | Spinlock held-depth accounting and scheduler preemption deferral; scheduler handoff publication/selection, migration-safe current-thread snapshots and block/sleep/exit rollback, termination admission, process collection after final thread exit, session retirement, retained handle lookup, shutdown application/service ordering and bounded timeout, VFS sync-hook/device-flush lifecycle and power-transition gate, pathname copy and exact-length VFS parsing, trailing-separator mutation rules, 8 MiB file-replacement and exclusive-create syscall copying and libc ABI, streamed VFS snapshot expectation and coherent node metadata, mutex wake/ownership, recursive block gate, dynamically allocated IPC request records and FIFO queues beyond the former 32-request/eight-entry bounds, endpoint registration beyond eight entries, ordered request/event merging across sequence wrap, client and endpoint exit cleanup, cancellation unlinking, and explicit event-ring overflow | Deterministic interleavings and pthread workers; shutdown fixtures execute production lifecycle functions with scheduler/storage adapters. They establish ordering and rollback, not hardware ACPI, real guest scheduling, or physical durability. LeakSanitizer is disabled in the shutdown fixture; ASan/UBSan remain enabled. |
| `test-storage` | Block retirement, GPT backing cleanup, mutation admission; AHCI completion/recovery, xHCI register layout and teardown, BOT retry, cache/read-ahead and USB batching | Pthread/IRQ fixtures, simulated MMIO, timing, DMA, command and device failures. xHCI MMIO register structures compile with packed-member warnings as errors and check the operational register alignment. These are production-path host tests, not hardware integration or physical recovery verification. |
| `test-unicode` | Shared strict UTF-8 codec, C restartable/nonrestartable multibyte conversions, wide strings, Shoot line-editor scalar edits and history draft navigation, terminal text metrics | Production sources under ASan/UBSan; history Up/Down traversal and cursor-preserving draft restoration, exhaustive scalar round trips, fixed malformed vectors, split-buffer cases, seeded malformed inputs, and display replacement-span checks. |
| `test-terminal` | Shared UTF-8 decoder adapter; production terminal geometry, 512-row scrolling, tabs, wrap cursor, canonical ESC command parsing and literal rejection of unsupported ANSI/VT sequences, UTF-8 write boundaries, startup cursor ownership, alternate-screen restoration, overlay/input positioning; process-output ownership serialization; console queue, waiter wakeup, raw-event and keyboard-repeat synchronization, including production HID repeat cancellation through the production raw queue; production loam line-editor incremental rendering and atomic prompt update | The framebuffer fixture runs the production line editor against the production terminal cell renderer, records glyph renders, dirty-row flushes, and copied bytes, and covers append, cursor-only movement, middle insertion, backspace tail clearing, history/draft replacement, completion replacement, and wrapped input. It confirms startup writes and clears leave the cursor disabled until the terminal API enables it. In the same 640x240 fixture, six appended characters fell from 123 to 18 glyph renders (85% fewer); both runs made six flushes; the current horizontal dirty spans copy 6,912 bytes instead of 276,480 bytes for full-row presentation. Unchanged text/color writes produce no glyph renders or pixel copies with the cursor disabled. Color changes and cursor restoration still invalidate their exact cells. A production rectangle-copy fixture checks clipping, zero and overflowing dimensions, and pitch padding through a host adapter for REP MOVSD. Each text update reasserts the cursor before presentation. Frame snapshots verify the cursor glyph at the expected cell after typing from both visible and hidden blink phases, after idle blinking, backspace and retyping, wrapped input, and cursor movement. A separate line-editor presentation fixture rejects the intermediate cursor flash at the prompt. The renderer fixture also maintains separate pixel backbuffer and display arrays, checks every glyph pixel against the text cells and the single allowed cursor overlay, and schedules a due blink between a deferred scroll and its presentation. A production loam command-submit, diagnostic, and next-prompt sequence reproduces two deferred scrolls without timer injection; the original source leaves cursor blocks in the prior prompt while its cells remain intact. It covers visible, blink-hidden and disabled cursors, repeated and batched colored scrolls, cursor APIs inside transactions, later editing/output, clear, fullscreen restoration, and login-style input. Its deterministic glyph model tests foreground/background restoration without qualifying PSF font lookup. Pthread-backed production-source interleavings run with ASan/UBSan; host fixtures do not establish native SMP qualification. The terminal API fixture checks that public output helpers encode canonical ESC bytes, use literal payload writes, and do not emit control bytes to redirected stdout. `make test-guest-reed` additionally drives held H and release through QMP USB HID in the two-vCPU Reed workflow and checks that the rendered cursor stops moving after release. It sends key-up before framebuffer capture so synchronous screenshot latency does not extend the hold. The new `line_editor_key_event_test.c` fixture executes the production line editor with HID identities and modifiers, covering nonmodal editing, word boundaries, history, completion, Ctrl controls, and cooked type-ahead. QEMU remains functional, not physical-hardware, qualification. |
| `test-clipboard` | Production clipboard daemon policy and public client API | ASan/UBSan; immutable read snapshots, atomic replacement, session/PID isolation, UTF-8 chunk boundaries, 8 MiB transfer, cancellation, abandoned-transfer expiry, allocation/transport failures and restart. Controlled IPC/session/mapping adapters establish host behavior, not native throughput. Fresh/incremental image tests preserve occupied Record IDs and run independent checks. LeakSanitizer is disabled. |
| `test-reed` | Reed buffer edits, multilingual byte/scalar boundaries and production snapshot/load, conditional-save, exclusive-create, Save As, stale-generation conflict prompt, and saved-baseline paths; production editor keymap, scalar/paragraph navigation, open-line insertion/failure rollback, selection/delete/undo, incremental search, wrapped visual-row layout, shared horizontal viewport, tab expansion, and command parsing/completion; configuration defaults, parsing, path resolution, exact-size reads, actionable diagnostics, and invalid-value fallback | The interaction fixture includes `userspace/reed/editor.c` and drives its production dispatch and frame-layout helpers; the configuration fixture calls the production parser and loader against controlled VFS/identity adapters. It checks invalid first/middle/last settings without partial application, duplicate/unknown keys, whitespace, large input, missing/unreadable files, short reads, allocation failure, and path/line diagnostics. Clipboard tests link the real client and daemon policy through controlled transport/session/mapping adapters, covering selection copy/cut/paste across simulated PIDs, publisher destruction, prepared-edit allocation failure, rejected NUL data and undo/redo. `test-reed-range-edit` injects failure at gap growth, undo-array allocation and delta creation before a single allocation-free commit. `fullscreen_render_test.c`, built with `TEST_REED_RENDER`, connects the actual editor to the production terminal renderer and checks cells and framebuffer pixels, changed output, cursor restoration, ordered navigation followed by insertion, search, page movement, and fullscreen exit. Work counters bound scalar decoding and line-bound lookup on a 2,000-line document; unchanged redraws emit no text, and a one-row numbered-text scroll stays below 200 output bytes without a full redraw. Narrow 1/4/17-column layouts cover wrapping, horizontal scrolling, tabs, UTF-8, selection, EOF navigation, and a 100,000-byte logical line. These deterministic work bounds do not establish native latency or physical keyboard release timing. Native guest execution and persistent configuration-media integration are not covered by this target. ASan/UBSan run with leak detection disabled; LeakSanitizer is not qualified. |
| `test-dev-tools` | Test-artifact lifecycle, guest evidence validators, and development-image update guard | Synthetic owned workspaces cover successful cleanup after verification, failed-run diagnostics, bounded failure retention, timeout process reaping, explicit retention, symlink/path isolation, and concurrent runs. Image fixtures prove synchronization precedes QEMU launch, sync failures prevent launch, and cleanup cannot precede independent verification. Image-update controls use disposable images and missing/busy/idle tool fixtures. |
| `test-password` | SHA-256, HMAC, PBKDF2 and authentication | Host compiler/entropy fixture; comparison with Python references. |
| `test-random` | Production SHA-256 HMAC_DRBG, entropy-source selection, CSPRNG service, syscall range validation, and libc API | ASan/UBSan fixtures cover the NIST SP 800-90A known-answer case, unsupported and busy sources, reseed failure, concurrent callers, and multi-megabyte requests. Leak detection remains disabled in this host environment. |
| `test-guest-random` | Native `mg_random_status()` and 2 MiB `mg_random_bytes()` from four concurrently launched processes | Disposable two-vCPU Q35/OVMF KVM image with independent stopped-image MGFS checking; requires `/dev/kvm`. |

## Graphical keyboard navigation diagnostic

Run `make diagnose-reed-input` from a Linux graphical desktop to capture real
keyboard holds in Reed. It uses `make run`'s Q35/KVM, CPU, display, and USB
device arguments with private generated media, automatically opens a populated
`/temp/navigation.txt`, and waits until setup input has drained. After `READY`,
hold and release h, j, k, and l for about one second each, pausing between
keys. No login or manual guest commands are needed. The session ends after
all four matched holds or 90 seconds and returns through Shoot before shutdown
and an independent stopped-image MGFS check.

The optional diagnostic kernel has a bounded, allocation-free navigation trace;
its hooks and storage are absent from normal builds. The runner reads already
accessible host input devices without grabbing them and retains only H/J/K/L
events. It records QEMU frontend/input events, guest HID reports, repeat
generation/cancellation, raw queue operations, framebuffer copies, and cursor
presentation. Evdev uses host monotonic time; QEMU's realtime timestamps are
correlated with host clock samples, and guest HPET timestamps are calibrated
through bracketed QMP counter reads. PIT uptime is recorded separately and is
not treated as a host clock. Reports include timing uncertainty and lost-event
counts; missing permissions or evidence leave the capture incomplete.

Concise evidence is retained through the existing bounded diagnostic-bundle
lifecycle; disposable images and builds are removed after owned processes are
reaped. `CAPTURED` describes collection, not a correctness or performance PASS.
`REED_INPUT_REPLAY=path/to/report.json` replays the last two captured physical
holds for controlled comparisons and labels that input as QMP replay. It does
not qualify a new physical-keyboard session. Screen-cursor coordinates alone
cannot measure document movement when viewport scrolling keeps the cursor at
an edge, nor do they measure compositor/monitor scanout.

The production-source dirty-row regression in `make test-terminal` rejects
copying clean rows between a cursor and a distant status update. The clock
and evidence negative controls run in `make test-dev-tools`.

## Scheduler current-thread migration

`tests/kernel/scheduler_current_test.py` compiles the production scheduler CPU-record wrapper,
block/sleep/terminate, dispatch preparation, first-entry and public thread
snapshot functions under ASan/UBSan. A deterministic fixture migrates the
caller between CPU-record lookup and dereference, with another thread running
on the old CPU. It checks dispatch success and rollback, unchanged remote
ownership, entry callback identity, and IRQ-state restoration. Linux x86-64
also executes the production GS-relative load against fixture CPU records;
other hosts leave that hardware check to the target build and guest.

The same fixture accepts an old `scheduler.c` pathname as its optional argument.
The pre-correction source fails all five cases (block, sleep, terminate,
snapshot and entry); the corrected source passes. These are controlled
interleavings, not a hardware interrupt emulator. `scheduler_handoff_test.py`,
IDT/IST, spinlock and termination-lifecycle fixtures cover the adjacent
contracts separately. Both scheduler fixtures are included in `make test-sync`.

`tests/kernel/process_reap_test.py` executes the production wait, poll,
session-retirement, destructor, and scheduler exit-completion functions. Its
controlled child publishes process termination while its exit syscall remains
ready or blocked, wakes its parent, and completes a remote stack handoff only
after the parent has resumed. Collection must retain ownership throughout
those states. It also checks cancellation by shutdown, refusal to advance a
busy session, and the destructor's fail-closed thread-destruction check.
`--process-source PATH --scheduler-source PATH` runs the same assertions on
older source; `107c328` fails at premature process-owner release. The shutdown
fixture additionally keeps terminated-but-suspended cleanup in the pending
work set. These host interleavings do not execute CR3 writes or hardware SMP;
the bounded `test-guest-shutdown` workflow supplies separate native evidence.

## Kernel heap and slab tests

Run the production heap and slab-core host fixtures with:

```sh
make test-heap
make test-slab
make test-allocator-routing
make test-slab-benchmark
make test-slab-guest
make test-hybrid-guest
make test-vmm-range-guest
```

`test-slab` compiles `kernel/src/slab.c` directly with ASan/UBSan once for each
validation policy. `SLAB_DEEP_VALIDATION=0` keeps mandatory ownership, pointer,
descriptor, touched-word, double-free, transition, and retirement checks while
deferring scans of unrelated bitmap words and neighbors until a transition or
explicit verifier. `=1` also scans bitmap population and checks adjacent list
descriptors on each touched operation and after mutation. The tests include
negative controls for corruption first detected later in normal mode and
immediately in deep mode. `slab_verify()` reconstructs full class/list and
accounting state, cache entries, in-flight refill ownership, cache-CPU
ownership, and duplicate cache references in both modes. The per-CPU fixture deliberately changes its
logical CPU ID after cache selection but before the host IRQ-save adapter
acquires the lock, then checks the allocator retries against the new CPU's
record. It also suspends one same-CPU refill in the backing adapter and checks
that a competing call takes the shared path instead of reserving a second
batch. These are source-level interleaving controls, not actual scheduler or
interrupt behavior. The PMM-owned-frame provider, VMM lease/map/unmap calls,
and IRQ-safe locks are host adapters for slab lifecycle tests;
`test-heap` separately exercises production page-extent allocation and release,
including overflow, undersized-release rejection, failed growth, and handle
consumption. These tests do not use a toy slab implementation and do not
establish Pith SMP correctness. The heap fixture also exercises
page-aligned extent backing through growth and PMM failure, then verifies that
releasing the extent returns capacity to ordinary heap use. It also checks
that extent-owned blocks cannot be released through the raw heap interface,
and that a copied stale handle cannot release a later allocation at the same
address. The slab host adapter currently provides one-page leases, matching
the production slab format. The public allocator API fixture keeps that leased
range separate from the indexed heap arena and shares a bounded synthetic
physical-frame pool between slab backing and heap growth. Its pressure check
therefore verifies that a retired slab frame can supply later heap growth;
this is host ownership-model coverage, while native guests verify the real PMM
and SMP shootdown path.

The slab fixture additionally exercises `slab_reclaim_bounded()` while two
host workers continue cache allocation/free churn. Deterministic controls hold
a cache lock and a class lock in turn and verify that the reclaim pass skips
each after one try rather than waiting. It checks record/object budgets,
refill reporting, held-spinlock rejection, recovery after an injected backing
failure, and ownership reconstruction after concurrent draining. Compile-time
host variants cover equal cache/refill capacities 2, 4, 8, and 16, as well as
separate 2/4 and 4/2 settings. These are measurement and bounds experiments;
the diagnostic default remains 2/2.

```sh
python3 tests/memory/slab_test.py gcc --cache-capacity 2 --refill-batch 2
python3 tests/memory/slab_test.py gcc --cache-capacity 4 --refill-batch 4
python3 tests/memory/slab_test.py gcc --cache-capacity 8 --refill-batch 8
python3 tests/memory/slab_test.py gcc --cache-capacity 16 --refill-batch 16
python3 tests/memory/slab_test.py gcc --cache-capacity 2 --refill-batch 4
python3 tests/memory/slab_test.py gcc --cache-capacity 4 --refill-batch 2
```

`test-slab-benchmark` compiles the production heap and slab sources into one
unsanitized `-O2` fixture with the same host arena and PMM/VMM/spinlock
adapters. Heap growth uses a pthread mutex adapter so concurrent growth keeps
the production serialization invariant; this adapter does not model scheduler
parking or remote TLB work. It compares indexed heap, shared slab, and per-CPU slab API paths
with identical 16-, 72-, 192-, and 384-byte allocation/free pairs. Every
measured allocation is touched at its first and last payload byte. The
single-thread phase keeps one anchor live, performs and validates 64
allocation/touch/free warm-up pairs per allocator and size, then records 128
samples of 1,000 pairs per size. Reported p50/p95/p99 are percentiles across
those batch means, not per-operation tail latencies. It does not grow its
16 KiB starting heap.

On Linux, the four-worker phase pins each pthread to a distinct CPU allowed by
the process affinity mask and verifies its CPU at worker entry and exit. A
separate 2,048-pair witness phase records the maximum number of overlapping
allocator-call brackets; overlap instrumentation is excluded from the timed
20,000-pair-per-worker phase. It measures same-class 16- and 72-byte traffic
and a cyclic mixed 16/72/192/384-byte workload. Missing workers, duplicate
CPUs, wrong counts, absent overlap, or a missing payload-derived checksum
invalidate the multicore result. Where the host cannot pin four CPUs, the
report marks multicore evidence incomplete. This is a host adapter, not Pith
SMP qualification.

The same fixture runs a live-population case with 1,024 simultaneously live
objects across 16, 32, 72, 128, 192, 256, 384, and 385-byte requests. It frees
and replaces alternating objects while preserving the others, independently
checks payload sentinels and pairwise live-range overlap, and sends 385-byte
requests through the indexed heap for both slab modes. After each untimed
phase it walks the production heap block chain to reconstruct free payload,
largest free block, and free-block count, then checks the sum against heap
statistics. Slab runs also report reusable slot capacity, CPU-cached slot
capacity, and slab backing pages separately; those free slots are not part of
the heap's free-payload statistic. The runner checks these quantities for
alignment and plausible ownership bounds. It also records mapped virtual heap
bytes, classifier retention, and memory before and after cache drain. Mapped
virtual extent is committed heap mapping in this fixture, not host RSS or a
claim of PMM reclamation.

The report includes raw samples, source hashes, cache and class counters,
bitmap work, lock acquisitions, backing counts, rounding waste, and cached
backing pages. A generated `-O2` host assembly file is retained beside each
JSON report. Capacity and refill batch can be varied independently:

```sh
python3 tests/memory/slab_benchmark.py gcc --cache-capacity 2 --refill-batch 2 \
    --output build/slab-cache-cap2.json
python3 tests/memory/slab_benchmark.py gcc --cache-capacity 4 --refill-batch 4 \
    --output build/slab-cache-cap4.json
python3 tests/memory/slab_benchmark.py gcc --cache-capacity 8 --refill-batch 8 \
    --output build/slab-cache-cap8.json
python3 tests/memory/slab_benchmark.py gcc --cache-capacity 16 --refill-batch 16 \
    --output build/slab-cache-cap16.json
```

Each command refuses to overwrite its JSON or assembly artifact. It separately
compares production token lookups over a deterministic, varying set of 600
live slab pages crossing two classifier leaves. A direct-root experiment and
an atomic-byte load are lower-bound references, not equivalent ownership
checks. Per-sample checksums keep results observable and reject mismatched
classification. A paired selector phase compares the production path against
the previous four-word bit-by-bit scan for the 16-, 80-, and 384-byte classes,
using matched non-inlined call boundaries and controlled bitmap occupancies.
The production path uses count-trailing-zeros for classes above 16 slots and a
bounded short scan for smaller classes. The parser has deliberate checksum
negative controls. Separated allocation/free batches and empty-class
refill/retirement samples expose costs hidden by the pair benchmark. Reports
record the timer, compiler flags, CPU affinity, source hashes, and raw samples
under the chosen `build/` output. These measurements are host-adapter evidence,
not Pith or hardware timings; host worker throughput is especially sensitive
to affinity and scheduling.

The earlier capacity sweeps in `build/stage4_5_v2_cap*.json` used four logical
cache IDs but did not pin those workers or witness overlapping allocator
calls. Their old four-worker throughput ratios are therefore not valid
multicore comparisons. They remain useful for the single-worker measurements
and for the finding that larger capacities did not consistently improve the
tested pair workload; the default remains two entries with a two-object
refill.

For the Stage 4.6 host comparison, use five independent invocations with
distinct output and assembly names, for example:

```sh
python3 tests/memory/slab_benchmark.py gcc --cache-capacity 2 --refill-batch 2 \
    --output build/stage4_6-mutex-trial1.json
```

The five-process mutex-adapter set used GCC 14.2.0 `-O2`, Debian 13 x86-64 on
a Ryzen 5 5600H, four pinned host CPUs, 64 warm-up pairs per allocator/request,
and independent fresh processes. Median-of-process single-worker pair p50s
for heap/shared-slab/per-CPU-slab were 122.4/120.9/110.7 ns at 16 bytes,
119.3/121.6/117.3 ns at 72, 119.4/124.5/120.3 ns at 192, and 117.8/126.5/122.0
ns at 384. This run set shows approximate heap/per-CPU parity, with no stable
single-worker advantage across sizes. The variation between retained five-run
sets also makes small differences inconclusive. Median four-worker rates were
4.24M/3.98M/4.80M pairs/s (heap/shared/per-CPU) at 16 bytes,
4.13M/4.11M/4.83M at 72 bytes, and 3.82M/1.49M/10.85M for balanced mixed-size
churn. The per-CPU rate was about 13–17% higher at the fixed sizes and 2.84x in
the mixed adapter. The mixed result is a synthetic local-cache reuse workload,
not a profile-weighted kernel trace. Workers were pinned to distinct allowed
CPUs, each completed 20,000 timed pairs, and a separate witness observed all
four allocator-call brackets active simultaneously. Host frequency and
unrelated work were not controlled, so the throughput spread is not a
hardware-independent scaling claim.

The per-process percentiles below are computed over 128 batch means of 1,000
pairs; they are not individual-operation latency tails. Values shown are the
median of each percentile across that five-process set.

| Request | Heap p50/p95/p99 (ns/pair) | Shared-slab p50 | Per-CPU-slab p50/p95/p99 (ns/pair) |
| ---: | ---: | ---: | ---: |
| 16 | 122.4 / 171.9 / 215.7 | 120.9 | 110.7 / 122.9 / 143.5 |
| 72 | 119.3 / 160.1 / 194.2 | 121.6 | 117.3 / 138.6 / 154.3 |
| 192 | 119.4 / 156.1 / 186.7 | 124.5 | 120.3 / 155.5 / 208.5 |
| 384 | 117.8 / 149.8 / 176.8 | 126.5 | 122.0 / 146.0 / 172.9 |

The runner also measures separate allocation and free batches for all three
paths while a high-occupancy set remains live. With the two-object per-CPU
cache, those batches include refills and overflow/drain work; they are not
local-hit-only measurements. Across five processes, 16-byte allocation/free
batch p50s were 174.6/91.6 ns for heap, 91.7/152.8 ns for shared slabs, and
178.9/152.8 ns for per-CPU slabs. At 72 bytes they were 162.9/162.9 ns,
155.2/162.9 ns, and 271.6/163.0 ns. At 192 bytes, allocation and free medians
are 349.2 ns for all three paths; 384-byte alloc/free medians are 698.5 ns for all
paths. These near-identical, coarse values do not resolve allocator cost.
Use the combined pair samples for steady reuse and the
population phases for refill/lifetime behavior; do not add the separate
allocation/free medians to estimate a pair.

The host run pinned workers but did not isolate the machine from unrelated
host work or fix its CPU frequency policy. Boost was enabled and frequency
scaling remained active. Independent processes and affinity checks make this
comparison useful for broad workload trends, but absolute latency and
throughput remain host-specific.

The 1,024-object phase requested 187,520 live bytes. Peak heap payload
accounting was about 233,750 bytes for both slab variants versus 190,528 bytes
for the heap; mapped heap extent was 344,064 versus 245,760 bytes. At the
alternating-free phase, the heap had 110,592 bytes of free payload split among
512 blocks, with a largest block of 6,480 bytes (5.9% of free payload). The
shared-slab and per-CPU runs had 98,304 and 110,592 bytes of slab backing,
respectively, with 5,840 and 16,432 bytes of reusable object slots; the
per-CPU run additionally held 832 bytes of object capacity in local caches.
These slot capacities are separate from heap free payload and cannot be added
to it without considering slab descriptors and object alignment. After all
caller objects were released, the per-CPU path retained 14 cached objects
(2,176 bytes of class capacity) over seven partial slab pages (28,672 bytes)
until the explicit drain. Draining returned all 53 slab backing blocks to the
indexed heap. The classifier retained 8,224 payload bytes, and four host CPU
records used 10,240 bytes. Returning backing blocks only makes them reusable
by the indexed heap; it does not unmap those heap pages or return physical
frames to the PMM.

Median phase times over the five host processes were:

| Phase | Indexed heap (µs) | Shared slab (µs) | Per-CPU slab (µs) |
| --- | ---: | ---: | ---: |
| Fill 1,024 live objects | 99.9 | 104.1 | 205.3 |
| Free alternating half | 20.4 | 44.4 | 45.5 |
| Replace that half | 54.3 | 50.6 | 88.7 |
| Release all objects | 52.9 | 87.9 | 91.5 |

Fresh one- and two-vCPU QEMU TCG diagnostic guest runs completed their native
child and independent stopped-image MGFS checks. The two-vCPU run recorded
both CPUs online, overlapping operations, and cross-CPU frees. For its 512-pair
worker phase, serialized TSC ticks were higher for the per-CPU path than for
the heap in same-size and mixed workloads; the coarse millisecond timer
reported zero for these short phases, so no native throughput rate is
claimed. TSC values are diagnostic QEMU ticks, not calibrated hardware cycles
or time. The original no-dedicated-warm-up host samples remain preserved as
`build/stage4_6-comparison-trial*.json`; the first warm-up-corrected run set
is in `build/stage4_6-warm-trial*.json`. Five final trials that also serialize
concurrent heap growth, check payload-derived checksums, reconstruct heap
free-block shape, validate slab slot capacity, and measure separate
allocation/free batches for all three paths are in
`build/stage4_6-mutex-trial*.json` and their matching `.s` files. The failed
no-op-mutex attempt is preserved as
`build/stage4_6-final-qual-trial4.failure-slab.txt`. That fixture let two
heap growers pass through its mutex adapter concurrently and hit the heap's
concurrent-growth invariant; the corrected adapter serializes growth with a
pthread mutex. The retained failure is benchmark-adapter evidence, not a
production allocator failure. A sixth, independent validation process is
retained as `build/stage4_6-final-validation.json` with matching `.s` assembly;
it confirmed four-worker overlap and a similar rate pattern, but is not folded
into the five-process summaries above. Guest reports live under
`build/stage4_6-native-*/report.json`.

Bitmap words carry separate integrity values. The tests corrupt a word not
touched by a free, confirm that free remains safe, and require detection before
that word is used or a slab is retired. The diagnostic full scan independently
checks the whole bitmap population.

`tests/memory/slab_test.py gcc --empty-reserve` compiles the production source with a
diagnostic one-empty-page-per-class candidate and tests same-page reuse,
duplicate-free rejection while retained, explicit draining, failed backing
allocation followed by reserve draining and one retry, and concurrent
drain/allocation/free. This policy is off by default.
`tests/memory/slab_lifecycle_policy.py` compares 32 repeated final-free cycles for
16-, 72-, and 384-byte requests over five fresh host processes, using the same
`-O2` production-source adapters for immediate retirement and the reserve
candidate. It validates backing allocation/release, retirement, reuse,
classifier payload, and final drain counts; raw samples and source/compiler
hashes go to disk-backed output files:

```sh
python3 tests/memory/slab_lifecycle_policy.py gcc \
    --output build/slab-lifecycle-<unique-run>-immediate.json
python3 tests/memory/slab_lifecycle_policy.py gcc --reserve \
    --output build/slab-lifecycle-<unique-run>-reserve.json
```

Replace `<unique-run>` with a new identifier for each invocation; the script
refuses to overwrite an existing evidence file.

This fixture isolates one-object churn. Its single-operation host timing
brackets are quantized at the measured scale, so it establishes fewer backing
operations but no stable latency reduction. The candidate holds at most 20
slab pages (80 KiB of backing plus heap metadata); immediate retirement remains
the default.

To force isolated ordinary and diagnostic target links, use unique ignored
build directories:

```sh
make -B -j4 BUILD_DIR=build/unified-default-build binaries
make -B -j4 BUILD_DIR=build/indexed-only-build \
    KERNEL_HYBRID_ALLOCATOR=0 binaries
make -B -j4 BUILD_DIR=build/slab-core-build \
    KERNEL_HYBRID_ALLOCATOR=0 SLAB_CORE_DIAGNOSTICS=1 binaries
```

The ordinary build defaults to unified hybrid-256 routing. Set
`KERNEL_HYBRID_ALLOCATOR=0` to build the indexed-only comparison/recovery
kernel; it omits `kernel/src/slab.c`. The diagnostic core target links the
slab implementation without initializing it or routing public allocations
through it. Neither configuration reserves BSS proportional to the 512 GiB
heap region. These commands check target ABI/linkage; guest tests exercise
allocator behavior in Pith.

`test-slab-guest` forces a separate kernel with `SLAB_CORE_GUEST_TEST=1`,
`SLAB_CORE_DIAGNOSTICS=1`, and `VMM_KERNEL_RANGE_GUEST_TEST=1`, then runs a
focused native MGFS boot under QEMU TCG with one and two vCPUs and the normal
e1000 NIC. A test-only kernel hook, called after SMP startup, exercises all
shared and per-CPU classes, payload-preserving local reuse, concurrent cache
allocation/free churn, cross-CPU frees, cache draining, bounded
`slab_reclaim_bounded()` calls during concurrent frees, page retirement, and
reuse by ordinary heap allocation. A following VMM phase retires a PMM-backed
slab page while CPU 1 uses a different CR3 and verifies both CPUs acknowledged
the shootdown before PMM returns the same physical frame for reuse. The runner
also exercises a reclaim pass with local interrupts disabled and verifies the
saved IF state remains disabled through the return. It requires reclaim
progress and, on two vCPUs, reclaim activity on both CPUs overlapping
allocator operations. Its negative controls reject missing slab,
bounded-reclaim, IRQ-disabled-reclaim, or PMM/VMM frame-reuse evidence. A
complete `slab_cpu_drain_all()` still requires allocator users to be
quiescent; the runtime reclaim API reports partial progress and never claims a
global drain. The guest runner independently checks the diagnostic kernel's
persisted boot-log witness, native Shoot process completion, clean MGFS images
before and after guest execution, online CPU count, and cross-CPU activity in
the two-vCPU case. It checks that heap-used growth covers retained classifier
metadata; slab backing itself is no longer heap committed extent. Temporary
images are removed after both independent image checks. Supply
`SLAB_GUEST_OUTPUT` or `HYBRID_GUEST_OUTPUT` to retain the full output tree, or
use `--retain-artifacts` for a default temporary workspace. This is focused
guest evidence, not production `kmalloc` routing or complete Stage 4
qualification.

The same guest hook also exercises the production indexed-heap tail reclaimer:
it forces heap growth through public `kmalloc()`, frees the allocation, trims
one complete owned four-page tail group, checks the mapped-prefix and PMM free
count changes, reacquires the released physical frame, and recommits the heap
through `kmalloc()`. The `HEAP_RECLAIM_GUEST` witness records group/frame/byte
counts and the VMM shootdown acknowledgment mask. This distinguishes physical
frame return from ordinary reuse of an already mapped heap free block. The
indexed-only and hybrid guest targets both compile this hook; two-vCPU runs
require acknowledgments from both online CPUs. A passing witness does not
qualify arbitrary interior heap-page reclamation: only complete owned tail
groups are eligible.

`test-vmm-range-guest` builds the bounded VMM-only guest hook and runs one- and
two-vCPU native boots. The hook acquires a generation-owned PMM frame, maps it
through the exact leased kernel-range handle, confirms release is rejected
while mapped, then unmaps and waits for shootdown completion before releasing
the frame. It immediately allocates again and requires the same physical frame
to be returned. In the two-vCPU case, a worker is pinned on CPU 1 with a
different CR3 while it reads the shared mapping; the evidence requires
acknowledgments from both CPUs and confirmation that CPU 1 invalidated under
that other CR3 before frame reuse. The runner requires native child completion
and independent clean MGFS checks before removing temporary images. Its
test-only witness also records whether the requested hybrid slab backend was
actually initialized; allocator readiness is no longer inferred from normal
boot presentation. For the
indexed-only comparison, set `KERNEL_HYBRID_ALLOCATOR=0` and
`VMM_RANGE_GUEST_BUILD=build/vmm-range-indexed`. QEMU TCG confirms execution
and synchronization behavior, not physical-hardware performance.

For a deep guest run, set both `SLAB_CORE_DIAGNOSTICS=1` and
`SLAB_DEEP_VALIDATION=1` on the outer `make test-slab-guest` invocation; the
top-level Makefile checks this dependency before the target's recursive build.
The guest runner requires the kernel's mode witness to match the requested
build configuration. The default unified build includes slab routing; deep
validation is selected for diagnostic or explicit hybrid test builds.

Historical source-matched runs at revision `4eb5b7a5656ee76f4b65f5edbf5e504db7c2fea8`
passed this contract in normal and deep modes on one and two vCPUs. All four
native Shoot children exited zero and independent `mgfsck` checks were clean
before and after execution. Both two-vCPU reports observed online mask `0x3`,
cross-CPU frees, and reclaim overlapping allocator work; the normal/deep
overlap counts were 84/116. The IRQ-disabled reclaim probe preserved IF state,
and all guest-evidence negative controls rejected missing witnesses. Reports,
logs, and images are retained under `build/stage4_6-guest-{normal,deep}-*/`.
These runs qualify functionality and reclamation behavior, not allocator
throughput or network receive traffic. Those runs used heap-backed slabs and
do not qualify PMM frame reclamation.

`test-hybrid-guest` forces `KERNEL_HYBRID_ALLOCATOR=1` with the public-routing
test hook, then runs one- and two-vCPU guests. The hook allocates each of the
twenty classes through public `kmalloc()` and frees through public `kfree()`,
checks the greater-than-384-byte heap path and zero/unrepresentable-size
behavior, then verifies reclamation and ownership. Four concurrent workers
allocate and free 192 class-80 objects through the public API while bounded
reclamation can overlap. The scheduler uses a shared migratable run queue, so
freeing a neighboring worker's objects does not by itself guarantee a remote
CPU free. In two-vCPU mode the guest therefore also performs one explicit
remote-free probe: it allocates a public 72-byte object, waits with a bounded
yield loop for the worker to run on another CPU, and keeps interrupts disabled
only across that CPU check and the single `kfree()`. The probe and class
counters must confirm the additional operation; the validator requires both
CPUs, at least one cross-CPU free, and overlapping allocator activity. The
public witness reports 192 operations on one CPU and 193 on two CPUs. It is
checked separately from direct slab-core activity and includes
missing-witness negative controls. The VMM witness also requires a PMM-backed
slab page to be retired while CPU 1 runs on another CR3, all online CPUs to
acknowledge invalidation, and the physical frame to be reacquired. MGFS is
independently checked before and after each guest. The target can also be run with both
`SLAB_CORE_DIAGNOSTICS=1` and `SLAB_DEEP_VALIDATION=1`. This is focused
routing/correctness evidence for the hybrid path. The ordinary build now
defaults to hybrid-256; `KERNEL_HYBRID_ALLOCATOR=0` remains the indexed-only
comparison/recovery configuration. This focused hook does not exercise
network receive traffic.

To exercise native e1000 receive under SMP, run the isolated MGFS guest with
its QEMU user-network gateway echo workload enabled:

```sh
build_dir=build/hybrid256
output=build/network-rx-2vcpu-run1
python3 tests/guest/guest_mgfs_test.py \
  --build-dir "$build_dir" \
  --output "$output" \
  --accel tcg --cpus 2 --repetitions 2 \
  --network-receive-smoke --negative-control
```

Use the indexed-only build for the baseline and a separate output path for
each configuration; each output path must be new.

The runner requires a 16-reply native ICMP burst and persisted e1000 RX/TX
counter deltas of at least 16, in addition to native child completion,
stopped-image MGFS verification, and its wrong-content negative control. The
network child runs in its own boot phase, separate from `fstest time`, so a
clock-test failure cannot prevent or be mistaken for RX execution. Each echo
opens and closes a 48-byte kernel ICMP object through public interfaces, so
hybrid-256 exercises the slab route for these objects. Fresh forced-build
two-vCPU campaigns passed with indexed-only (run
`3e29b8f1c67157d1`) and hybrid-256 (`59be9a6f8b578c43`); each received 16
36-byte replies from `10.0.2.2`, with native interface RX/TX counters advancing
from 0/1 to 17/18. Both reports prove two CPUs online, native child completion,
independent clean MGFS checks, and the wrong-content negative control. The
evidence validator also rejected missing/short bursts, wrong gateway, short
RX/TX deltas, absent interface RX, and malformed phase markers. This is
functional QEMU TCG evidence: it does not establish which CPU serviced each
device interrupt, simultaneous network/allocator execution on both CPUs, or
physical-NIC throughput. The packet path is exercised by this separate test,
not by `test-hybrid-guest` or the heap-profile guest. The earlier combined
time-and-network attempt exited 43 without a valid time/network transcript;
its precise failing subcheck is unresolved. Later, four Stage 5 Slice 1 MGFS
guest attempts at one and two vCPUs also exited 43 in `fstest time`. The
embedded kernels were built with `RTC_UTC_TRUST=0`, although QEMU used a UTC
virtual RTC. With that build policy, `timekeeping_init(false)` leaves realtime
unavailable and the first check in `userspace/fstest/time.c` returns 43 when
`mg_clock_realtime()` fails with `MG_ERR_TIME_UNAVAILABLE`. A time-only guest
run from revision `5ce4604` with `RTC_UTC_TRUST=1` passed the native transcript
checks and independent MGFS verification. These four failures were build
configuration errors, not evidence against the input synchronization change.
The older combined time-and-network attempt has no preserved build setting, so
whether it had the same cause remains unknown. The historical
`volume-safe-create` status-43 failure below is a separate, unresolved
filesystem incident. Earlier indexed-only boot/stack failures remain
separately recorded under the scheduler/system qualification notes.

The previous MGFS `volume-safe-create` child-status-43 failure has no
established root cause. Passing current create/verify runs and the intentional
wrong-content negative control do not prove that historical failure was
fixed. A matched two-vCPU trusted-clock safe-file campaign completed under
both indexed-only (run `75f6e7d78fcc6b33`) and hybrid-256 (run
`1ee3c38adbcbbd88`): create, verify, and read-only children exited zero, each
stopped MGFS image passed the independent checker, and the intentional
wrong-content control was rejected with child status 43. These runs qualify
the current workflow in both allocator configurations; they do not establish
the cause of the earlier failure.

Historical public-routing guest runs used hybrid-256 in normal and deep
validation, each at one and two vCPUs. Those reports passed native child
completion, independent MGFS checking, and the then-current evidence negative
controls. The two-vCPU reports recorded both CPUs online, overlapping
allocator activity, and a verified cross-CPU free; their reclaim probes
returned cached objects and released heap-backed slabs to indexed-heap
capacity while preserving the caller's IRQ state. They do not qualify physical
frame reclamation. A failed earlier deep-mode witness was traced to the
test assuming a worker-ID ordering implied a remote free; the dedicated probe
above replaces that assumption. This establishes the test correction, not a
change to production allocator behavior.

`test-hybrid-public-benchmark` checks the public benchmark validator's sample
normalization and percentile controls. To run the host comparison against the
actual production heap/slab sources and the exact small-request size histogram
from an accepted Pith profile, use a new ignored output path:

```sh
python3 tests/memory/hybrid_public_benchmark.py gcc \
    --profile-summary build/stage4_6-profile5-guest2-rerun/mgfs-0/create/heap-profile/summary.json \
    --output build/stage4_6-hybrid-public-<run>.json
```

The runner builds indexed-only and public-hybrid binaries with GCC `-O2`,
then also calls the shared and per-CPU slab APIs directly for mechanism
comparison. Five independent processes take 128 samples of 1,000 checked
allocation/free pairs for 16-, 72-, 192-, and 384-byte requests. Four-worker
cases pin workers to distinct available host CPUs and require overlap,
participation, checksums, payload validation, and exact operation counts. A
separate replay preserves the profile's exact request-size multiset up to 384
bytes but pairs each allocation immediately with its free; it does not
reconstruct the profile's object lifetimes, contexts, or ordering. A 1,024-live
object test measures mixed-size population and backing extent, and an
exhaustion fixture checks bounded reclaim and recovery. Invalid, interior, and
double-free controls must fail, as must a missing timing sample. The process
uses `CLOCK_MONOTONIC` wall time; host adapters do not reproduce Pith scheduling,
interrupts, or remote TLB behavior. The latest corrected source-matched
summary and raw files are retained under ignored
`build/stage4_6-hybrid-public-v4-final.json` and
`build/stage4_6-hybrid-public-v4-final-build/`.

`test-slab-native-benchmark` builds isolated diagnostic kernels. The
comparison build sets `SLAB_CORE_GUEST_BENCHMARK=1` and
`SLAB_CORE_GUEST_FREE_PROFILE=0`; it compiles the free-path probes out while
comparing direct production indexed-heap calls, the shared slab API, and the
per-CPU slab API. Its
steady samples use serialized same-CPU TSC deltas, exclude scheduler-tick
intervals, and report TSC ticks without converting them to time. They remain
QEMU TCG diagnostics, not hardware measurements. In the four-worker phase,
each allocator call is bracketed with local interrupts disabled to keep CPU
identity stable; cross-free owner capture uses the same bounded wrapper. These
calls do not represent an ordinary interrupt-enabled caller.

The comparison records per-CPU allocation/free counts, worker completion,
error status, CPU participation, cross-CPU frees, and overlapping allocator
call brackets. Per-CPU cache-line-separated active flags use sequentially
consistent publication and observation; a two-vCPU result is rejected unless
both requested CPUs execute allocator calls and a positive overlap is observed.
Per-CPU counts must reconcile with each workload's aggregate operation totals.
An empty-bracket calibration records the overlap-tracker cost; it is reported
but not subtracted. Substantial run-to-run TCG variation can make parallel
elapsed ticks inconclusive even when overlap evidence is valid.

A second build sets `SLAB_CORE_GUEST_FREE_PROFILE=1` and adds stage probes to
`slab_try_free`. It measures nonfinal frees, full-to-partial transitions, and
final-object retirement for 16-, 72-, 192-, and 384-byte requests. Each case
runs three trials of 1,024 operations and retains 64 sampled attempts per
trial. The uninstrumented steady-state comparison retains 31 batches of 16
operations per allocator, size, and phase, after two warm-up cycles. This is
more useful for comparing medians than the earlier seven-batch run, but does
not support tail-latency claims. Zero-tick profile deltas are counted as lost
observations; unexplained loss or truncated samples fail validation. This
profile build is for path
decomposition only: its instrumented steady and parallel timings are not used
to compare allocators. Its diagnostic boot-log ring is 512 KiB so the full
bounded profile remains available; the unprofiled comparison ring is 192 KiB.
Component intervals are independent samples and must not be summed as if they
were one free operation.

Both configurations verify native child completion and independently check
the stopped MGFS image. The benchmark runs one- and two-vCPU comparison
guests, then a one-vCPU free-profile guest. It preserves reports, extracted
boot logs, source and kernel hashes, and disk-backed images under ignored
`build/` paths. e1000 remains configured, but these workloads send no network
traffic. Use fresh build and output paths, for example:

```sh
make test-slab-native-benchmark \
    SLAB_NATIVE_BENCH_BUILD=build/slab-native-build-<run> \
    SLAB_NATIVE_BENCH_OUTPUT=build/slab-native-<run> \
    SLAB_NATIVE_PROFILE_BUILD=build/slab-profile-build-<run> \
    SLAB_NATIVE_PROFILE_OUTPUT=build/slab-profile-<run>
```

The latest source-matched campaign includes a passing one-vCPU run, a passing
two-vCPU run, and a passing one-vCPU free-profile run; three earlier corrected
two-vCPU repeats also passed. A preceding two-vCPU run completed both CPUs'
overlapping allocator calls, then failed its mixed-class phase after the BSP
released a cross-free anchor before it had completed its own work. The
allocator calls were interrupt-disabled, so growth was correctly unavailable.
The benchmark now retains that anchor until the BSP finishes. An earlier
tracker rejection also came from checking CPU identity after restoring
interrupts; the benchmark now performs that check inside the protected
interval. These changes correct the test-harness causes, and current reports
still require successful child completion, per-CPU count reconciliation,
overlap, cross-CPU frees, and payload ownership checks. Parallel elapsed TSC
values varied by several times between runs, so they do not support a stable
throughput conclusion. Per-operation medians are diagnostic TCG evidence, not
a hardware performance claim.

This target does not change ordinary
`kmalloc`/`kfree` routing, and benchmark-kernel timings do not establish
production or physical-hardware performance.

`test-launch` remains a compatibility alias for VMM, heap and history tests.
The separate host-only first-fit/indexed allocator comparison is reproducible
with:

```sh
python3 tests/memory/heap_benchmark.py --output build/heap-benchmark-<unique-name>
```

Use `python3 tests/memory/heap_benchmark.py --self-test` to check its percentile and
measurement negative controls without running a comparison.

The output path must be new and under ignored `build/`; existing evidence is
never overwritten. The default run uses seven trials and 256 timed
allocation/free samples per scenario and revision. It saves the extracted
production sources, their hashes, generated benchmark binaries, exact
invocation and compile/run commands in `commands.json`, raw JSONL samples, workload/search-step
counters, timer overhead, host/compiler details, and a summary. The harness
uses GCC `-O0`, one shared PMM/VMM and pthread
adapter, no sanitizers, and no full allocator verifier. Workloads include
fragmentation, undersized same-bin candidates, mixed lifetimes, large and
boundary sizes, allocation failure/recovery, growth, and four-worker churn.
Search counters are gathered by separate instrumented binaries and provide a
negative control for the expected fragmented-search ordering. Latencies use
serialized RDTSCP deltas calibrated over 100 ms against
`CLOCK_MONOTONIC_RAW`; measured timer-pair overhead is recorded and not
subtracted. Reported p50/p95/p99 values are medians of the per-trial
percentiles; pooled samples and per-trial ranges are retained too. The tool
requires Linux x86-64 with RDTSCP and constant, nonstop or
invariant TSC flags. The concurrent pthread workload is pinned to the available
host CPUs, but does not reproduce Pith locks, interrupts, TLB shootdowns, or
scheduling. Heap-growth comparisons are descriptive only because the compared
revisions have different growth implementations. These measurements have no
pass threshold and are not guest or hardware performance claims.

The diagnostic-only target heap profile is separate from allocator benchmarks:

```sh
make test-heap-profile
make test-heap-profile-guest
```

The host target builds the production profile source with ASan/UBSan in both
indexed-only format 5 and hybrid format 6 modes, then runs snapshot-parser
negative controls. The indexed-only guest target forces an isolated
`HEAP_PROFILE_DIAGNOSTICS=1` build and runs one- and two-vCPU QEMU TCG MGFS
workloads. Override `HEAP_PROFILE_BUILD`, `HEAP_PROFILE_OUTPUT`, and
`HEAP_PROFILE_ACCEL` to use unique ignored `build/` paths and the selected QEMU
acceleration. The one-vCPU run includes a wrong-content negative control. The
runner keeps e1000 attached, extracts a frozen snapshot from stopped media in
bounded chunks, rejects any loss/inflight/overflow flags, checks the binary
profile structure independently, and runs the MGFS checker. Results and raw
snapshots are retained when a new explicit output directory is selected or
`--retain-artifacts` is used; otherwise the temporary guest workspace is
removed after validation. Never point a run at an existing evidence directory.

Format 5 records indexed-heap request sizes, outcomes, mutually exclusive
normal/interrupts-disabled/tracked-spinlock-held context totals, CPU identity,
immediate caller text offsets, object IDs, and allocation/free events. A caller
offset is a 32-bit RVA from `__kernel_text_virt_start`; `0xffffffff` means the
return address was outside the linked kernel text. This records one allocator
call site, not a stack trace or semantic object type. The optional event trace
is bounded to 65,536 24-byte records (1.5 MiB). It is allocated once from the raw heap
during diagnostic initialization before recording starts; allocator
instrumentation itself does not allocate. The trace array is not static BSS,
but it increases diagnostic heap use and changes the profiled memory state.
If allocation fails or events overflow, the trace is explicitly
unavailable/incomplete and the evidence validator rejects it. Snapshot export
contains a versioned `PITH-ALLOC-TRACE-2` CSV with a digest and exact event
count. Format 4 snapshots remain readable and are reported with an unknown
caller site. Event publication happens under the existing heap lock after the
record is complete, so ordering is heap commit order rather than call-entry
order, wall-clock order, or proof of overlapping CPU execution. Reused
addresses receive new profile-local object IDs. The trace preserves request
size, allocation/free relationship, caller RVA, phase, CPU at commit, and
cross-CPU frees for offline replay; lifetime distance is an operation count,
not elapsed time.

Format 6 is available only through the explicitly enabled
`HYBRID_RUNTIME_DIAGNOSTICS=1` build option, which requires
`KERNEL_HYBRID_ALLOCATOR=1`. It appends a heap-backed extension containing
public `kmalloc`/`kfree` routing totals, pressure-reclaim attempts and results;
the existing per-class and per-CPU slab/cache counters; classifier storage;
heap extent/payload counters; CPU count; current and peak slab-page occupancy;
diagnostic-storage size; and the maximum number of simultaneous per-CPU
cache critical sections. The latter
is updated while the IRQ-saving local cache lock is held, where migration and
local interrupt reentry are excluded. It establishes allocator-section
overlap, not throughput. The extension is allocated through the raw indexed
heap before recording begins, not in static BSS. At the current layout it
occupies 496,232 bytes in extension version 2. Version 1 remains readable by
the analyzer for earlier captures. The existing 65,536-record event buffer
remains 1.5 MiB. The diagnostic `fstest` export includes both payloads; the analyzer
rejects a truncated extension. These allocations change diagnostic heap use
and are reported so footprint values can be interpreted. No ordinary build
has runtime instrumentation cost.

Format-6 route counters count admitted public calls from profile initialization
until freeze. Per-class and per-CPU records are sampled one lock-protected
record at a time; they are not one global stop-the-world instant and may
include activity adjacent to the freeze. Freeze closes route admission and
waits a bounded interval for already admitted calls. If one remains active,
the profile is marked incomplete and export is rejected. Counter overflow,
uninitialized slabs, CPU-count mismatch, or unavailable rows also invalidate
the export. The ordered heap event trace is not a complete public hybrid event
trace: it records heap allocator operations, including raw allocator use,
while successful slab object lifetimes are represented by aggregate routing,
class, and CPU-cache counters. Do not replay a format-6 heap trace as the full
hybrid workload. The extension supplies event frequencies, pressure-reclaim
counts, transient backing-allocation failures, and ownership state, not
per-object slab lifetimes or timing
attribution. Reclaim-attempt counters include bounded reclaim invoked after
an indexed allocation failure and after slab backing allocation failure;
they do not count independent diagnostic drain calls.

Build and run an isolated native hybrid counter profile with unique ignored
paths:

```sh
make -B -j4 BUILD_DIR=build/hybrid-profile-<run> \
  KERNEL_HYBRID_ALLOCATOR=1 KERNEL_HYBRID_ALLOCATOR_CUTOFF=384 \
  HYBRID_RUNTIME_DIAGNOSTICS=1 RTC_UTC_TRUST=1 \
  binaries fstest mkmgfs mgfsck
python3 tests/guest/guest_safe_test.py \
  --build-dir build/hybrid-profile-<run> \
  --output build/hybrid-profile-<run>-guest-2vcpu \
  --accel tcg --backend mgfs --repetitions 1 --readonly \
  --heap-profile --cpus 2
```

`make test-hybrid-runtime-profile-guest` runs the host controls, forces this
diagnostic configuration, and executes fresh one- and two-vCPU profiles. Set
`HYBRID_RUNTIME_PROFILE_CUTOFF=256` to collect the existing memory-conscious
policy as a separate source-matched build. The guest profile suite covers its
boot/service, Shoot, Reed edit/save, MGFS create/verify/read-only paths; it does
not reproduce complete system pressure. QEMU TCG operation counts are
functional/state evidence, not hardware latency or throughput measurements.

Format-4/5 indexed heap request profiles retain caller-attributed ordered
event history. Convert an RVA to symbols with `addr2line` using the matching
Pith ELF and address
`__kernel_text_virt_start + caller_text_offset`; source identity comes from the
guest report/build artifacts, not the RVA itself. The event replay accepts
format-4/trace-1 records with an unknown caller and format-5/trace-2 records
with caller RVAs. The runner's Reed interaction exercises editing and saving
but skips the external UDP stale-conflict rendezvous. Current profile guest
evidence uses MGFS only and is not a replacement for FAT32/exFAT or broader
system qualification. The independent `--network-receive-smoke` procedure
above is required for a native RX claim; merely attaching e1000 is insufficient.
Offline ideal-packed slab demand is a lower-bound model from the trace, not
observed slab occupancy or class-lock contention. Profiling memory and
instrumentation mean these runs are not production latency or footprint
measurements.

To replay a complete profile event sequence against the indexed heap, shared
slab core, per-CPU slab core, and public hybrid cutoffs, provide both the
validator-produced summary and its matching CSV trace:

```sh
python3 tests/memory/hybrid_public_benchmark.py gcc \
  --profile-summary build/<profile>/mgfs-0/create/heap-profile/summary-event-ready.json \
  --profile-events build/<profile>/mgfs-0/create/heap-profile/allocation-events.csv \
  --trials 5 --output build/<new-benchmark-output>.json
```

The replay runs serially on the host. It preserves captured event order,
object lifetime, request size, phase, and logical free CPU, but does not
reproduce guest concurrent intervals, migration, IRQ state, or timer cost. Do
not present it as multicore throughput. The separate host four-worker fixture
pins workers to distinct host CPUs and validates overlap; its synthetic
immediate-pair workloads are not a substitute for the trace replay.

The opt-in [native MGFS guest test](guest-filesystem.md) boots real processes
and verifies persisted media independently; it is separate from `make test`.
The [filesystem qualification](filesystem-qualification.md) documents
MFQS protocol, exact coverage, replay, negative controls and resource bounds.
The oversized harness has been split into subsystem files. Temporary C programs
are extracted from production definitions; fixtures duplicate only the required
surrounding ABI. These tests do not establish whole-kernel integration or ABI
compatibility by themselves. Build and guest tests are still required.

The mgfs_device_identity_test.py fixture executes production block admission,
FAT32 mounted read/write, FAT-entry helpers and sync, exFAT mounted I/O and sync,
plus MGFS identity-bound paths. Pthread-backed waiters and
device callbacks inject removal, slot reuse, failed reads, partial writes,
multi-copy metadata failures, and flush failures. The fixture does not model
the full filesystem namespace, hardware DMA, or transaction recovery.
`fat_exfat_mount_identity_test.py` compiles the production FAT32 and exFAT
drivers into controlled mounted-media fixtures. It exercises repeated lookup,
empty-file identity, ordinary mutation generations, rename, slot reuse,
coherent snapshot bytes, cached size updates, ambiguous writes, and saved
device identity. VFS node registration and size publication, block callbacks,
and heap allocation are controlled dependencies. It does not prove full
guest concurrency or power-loss recovery.
The `fat32_rename_test.c` fixture executes the production pinned rename,
shared create placement/growth and LFN retirement mechanisms. It covers short,
case-only, maximum ASCII and Unicode names, shared sectors, cluster boundaries,
root/non-root directory moves, ancestry rejection, retained identities, slot
reuse, files larger than 2 MiB, and 64 KiB clusters. Faults cover each observed
write/flush/allocation boundary, read-time removal and registration replacement,
failed pin finishing, and forced-detach node retargeting. Controlled block,
heap, node-registration and recovery-registry dependencies remain host fixtures.
The production VFS fixture verifies same-identity destination admission and
post-unlock pin finishing. `path_move_syscall_test.py` extracts the real native
move case and authorized helper, testing exclusive destinations, same-identity
aliases, authorization revalidation and uncertain error translation.
`path_remove_syscall_test.py` checks that a trailing separator cannot turn a
file removal into a successful unlink. `pathname_copy_test.py` executes the
production syscall path copier with exact-limit, unterminated, inaccessible,
and address-overflow inputs. `vfs_location_test.py` exercises exact-length
VFS path parsing, embedded-NUL rejection, a maximum-byte multibyte name,
trailing separators, and the existing directory-identity lifecycle.

Set `FAT32_RENAME_IMAGE` to a disposable, independently formatted image containing
`SOURCE`, `DEST`, `KEEP.TXT`, a 255-byte ASCII `a` filename under `SOURCE`, and
`SOURCE/Imported long directory name/CHILD.TXT`, with the files containing
`external-bytes\n`. Fill DEST's first cluster to exercise growth. The optional
fixture moves the imported file and directory, reads existing bytes, creates and
moves a child, reuses the retired slot, and validates allocation ownership.
Verify resulting names/readback separately with mtools and run `fsck.fat -n`.
Use disposable images only; these checks are not guest, hardware, reboot or
power-loss verification. On restricted Linux hosts use
`ASAN_OPTIONS=detect_leaks=0`; this disables leak detection.

The `fat32_ordinary_test.c` fixture executes production pinned writes and internal
resizing, including empty-file growth, holes, fragmented allocation, confirmed
partial writes, truncate grow/shrink, extra allocation past EOF, tombstones and
slot reuse. It injects definite, torn and ambiguously completed writes at every
data, FAT-copy, directory and FSInfo boundary, plus flush failure, removal,
re-registration and failed cancellation. Preparation allocation failure, pin
exhaustion, ownership conflicts, mirrored-FAT disagreement, proof invalidation,
and cached-tail reuse are asserted. Generated backing media exercises a
fragmented 128 MiB file and repeated append/truncate without proportional
transaction allocations. VFS dispatch tests separately verify the internal
resize hook, unchanged offsets, permissions, legacy fallback and post-unlock
pin finishing. Device/pin callbacks remain controlled dependencies; no guest
or physical-device behavior is implied.

For independent tooling, format a disposable FAT32 image with `mkfs.fat -F 32`,
populate an empty `TARGET.BIN` with mtools, and set `FAT32_ORDINARY_IMAGE` when
running `fat_exfat_mount_identity_test.py`. Its production driver writes more
than 3 MiB, appends, grows and shrinks, and reads the contents back. Verify the
result independently with `mcopy`/`mtype` and `fsck.fat -n`. On Debian, include
`/usr/sbin` in PATH for dosfstools. The expected output is `(i * 7) & 255` for
`3 * 1024 * 1024 + 17` bytes, followed by `TAIL`. Images must be disposable;
no persistent development disk is used. Current resource measurements appear in
[FAT metadata performance](fat-metadata-performance.md#fat32-ordinary-write-and-resize-resources).

The `fat32_create_test.c` fixture executes production ordinary create/mkdir hooks
and their shared safe-create transaction. It covers short, Unicode and maximum
255-byte names; sector/cluster boundaries; fragmented parent directories, growth
and earlier end markers; dot entries and nested directories; empty-file growth;
slot reuse, supported short-name rename and removal. Every admission allocation
is failed before mutation. Faults cover definite, torn and ambiguously completed
writes, each FAT copy, FSInfo, flushes, removal and device re-registration, plus
cancellation/cleanup failure and retained pins. The current layouts exercise 386
write/removal/re-registration/flush fault cases. Legacy callback refusal is tested.
Block I/O, heap services, node registration and pin callbacks are controlled
fixtures; this is not guest concurrency or persistent recovery evidence.

`vfs_read_only_test.py` combines production VFS write, truncate, unlink and
rmdir dispatch with both production FAT32 and exFAT drivers on controlled writable
media logically mounted read-only. It also calls pinned creation, mkdir,
replacement, rename and retirement hooks directly and checks mounted low-level
write/allocation defenses. Media bytes, filesystem/node state, generations,
offsets, heap/pin activity and stale-handle errors are asserted. The write and
namespace pin fixtures execute the production retained-superblock policy helper
for ordinary, trusted, identity-checked, pinned and legacy dispatch. Recovery-pin
admission rejects read-only superblocks without allocating. MGFS production
callbacks have equivalent denial regressions. Independent image fixtures check
read-only mount admission and rejection as well as unchanged readable contents.
Block, heap, lifetime and lock dependencies remain controlled; these tests do
not establish guest SMP behavior, hardware durability, or crash recovery.

`vfs_create_pin_test.py` executes production ordinary and trusted owned
create/mkdir dispatch, node registration and pin finishing using pthread locks
and controlled driver hooks. It also executes the production native create-path
helper and VFS error translator with controlled pathname/lookup dependencies;
this tests dispatch and result propagation, not syscall entry assembly or user
page copying. Owned callbacks retain explicit authorization while enforcing
read-only mounts, and pinned callbacks take precedence over legacy callbacks.
It checks authorization after lock waiting, legacy compatibility, return-node
allocation failure, cancellation, pin exhaustion, uncertainty, finish retry,
detach and reference ownership. In particular, a node made public by completed
handoff is preserved after late removal, because another lookup may retain it.
The exclusive-create fixture checks that same lifetime rule for its prepared
handle and verifies failed private return cleanup separately.

Optionally set `FAT32_CREATE_IMAGE` to a disposable, independently formatted
FAT32 image with a full root directory when running the combined FAT fixture.
The production driver creates a 255-byte directory name (`d` repeated 250 times,
then `édir`), writes `ordinary large file.txt` within it, and exercises nested
creation/removal and supported short-name rename/unlink. Independent `mcopy`
readback must equal `(i * 13) & 255` for `3 * 1024 * 1024 + 17` bytes; check the
result with `fsck.fat -n` and verify any preexisting neighbor separately. This
mode modifies only the explicitly supplied disposable image. Resource figures
are in [FAT metadata performance](fat-metadata-performance.md#fat32-ordinary-create-and-mkdir-resources).

The separate `fat32_namespace_test.c` fixture reuses these controlled dependencies
and executes the actual FAT32 comparison, LFN validation, lookup, enumeration and
namespace mutation admission functions. Independently specified Unicode/UTF-16
vectors cover case-mapping categories, supplementary scalars, absence of full
casefolding/normalization, malformed encodings, boundary names, long imported
names, cross-namespace collisions and unknown OEM aliases. It verifies refusal
before writes or recovery pins, preserved confidence, zero comparison allocations,
and retryable read/allocation failures. The committed ASCII-only comparator fails
the `é`/`É` assertion. The pinned mapping generator is checked by the normal runner.
See [FAT32 filename policy](../filesystem/fat32/names.md) for limits and provenance.

`FAT32_NAMESPACE_IMAGE` optionally supplies a **read-only disposable FAT32 image**
containing `Résumé long.txt`, `Long report.txt`, and `PLAIN.TXT`, each holding
`fixture` followed by a newline. Production mount, uppercase accented lookup,
content reads and conservative OEM-alias refusal are exercised. A reproducible
external-tool fixture is:

```sh
testdir=$(mktemp -d)
python3 -c 'import sys; f=open(sys.argv[1], "wb"); f.truncate(64*1024*1024); f.close()' "$testdir/names.img"
mkfs.fat -F 32 "$testdir/names.img"
printf 'fixture\n' > "$testdir/contents.txt"
for name in 'Résumé long.txt' 'Long report.txt' 'PLAIN.TXT'; do
    mcopy -i "$testdir/names.img" "$testdir/contents.txt" "::$name"
done
mdir -i "$testdir/names.img" ::
mtype -i "$testdir/names.img" '::RÉSUMÉ LONG.TXT'
fsck.fat -n "$testdir/names.img"
FAT32_NAMESPACE_IMAGE="$testdir/names.img" python3 tests/filesystems/exfat/fat_exfat_mount_identity_test.py gcc-16
```

This requires UTF-8 host filename support and an mtools codepage that produces
a non-ASCII alias for the accented file (the fixture asserts that condition).
It does not infer that codepage in Mangrove. Linux uses its native GCC command;
macOS uses the Homebrew versioned GCC. Independent formatting/listing/checking
is not a QEMU integration test or broad Windows interoperability certification.
These production-function fixtures also exercise operation-local metadata
views, fragmented traversal and large-directory read budgets.
Paired FAT-write cases additionally check complete recovery sector images,
reserved nibbles, mirror failures, torn two-entry updates, cancellation and
full-size write budgets.
See [FAT metadata measurements](fat-metadata-performance.md) for the profiling
command, baseline comparison, cache lifetime and verification limitations.
The FAT32 fixture also executes the bounded full-directory walker and private
namespace ownership preflight against controlled FAT sectors and directory
clusters. It covers multi-cluster lookup and mutation, nested directories,
shared file/directory clusters, malformed chains and long-filename metadata,
read/allocation failure, and a byte-counted heap peak. It also executes the
private 0–2 MiB conditional replacement hook, including fragmented staging,
mirrored FAT writes, torn publication, partial cleanup, and pin handoff.
Device callbacks, VFS admission, and recovery pins are controlled host
dependencies; this does not test a guest mount or out-of-band media changes.
The FAT32 ownership scaling fixture reuses the production driver with a generated
read-only block backend. It covers the exact 128 MiB rejection regression,
fragmented 512 MiB and maximum-size files, claim collisions across bitmap chunks,
12,000 nested directories, 70,000 directory children, and chains exceeding the
ordinary directory-walk bound. Allocation and read failures, removal, mirror
conflicts, cyclic ownership and the shared memory-budget failure are checked
without media writes or retained heap. The generated backend supplies disk
bytes; it does not implement the ownership algorithm. An optional disposable
`mkfs.fat`/mtools image exercises production mount and ownership validation.

`fat32_mutation_boundary_test.py` checks the production object's complete public
symbol inventory, absence of raw allocation/extension/free declarations, and
optional boot call sites. Its ASan/UBSan fixture executes nine production
mutation hooks against two live entries sharing a cluster: every operation must
reject without writes, allocation reclamation or recovery-pin acquisition.
Read-only FAT inspection covers bounds, heap/read failure, logical read-only
mounts, removal and reused device slots. Heap, block and VFS registry dependencies
are controlled; transaction failure phases are covered by the existing mutation
fixtures. When dosfstools and mtools are available, a separately formatted
disposable image exercises production retirement of short and spanning-LFN
files larger than 2 MiB, `fsck.fat` checks and unrelated-file readback. The source
image remains unchanged. Compiling optional boot diagnostics is distinct from
running them in a guest; these checks do not establish hardware or crash recovery.

The exFAT fixture executes its private namespace ownership
preflight with FAT-linked and contiguous mappings, metadata streams, nested
and multi-cluster directories, shared and free-marked clusters, entry-set
checksum faults, read/allocation failure, and device identity loss. Its block
device and heap remain controlled fixtures; it does not establish external
exFAT-tool interoperability or guest concurrency.
The fixture also executes production ordinary write and truncate hooks on
same-sector and split-sector entry sets. It checks checksums, readback,
NoFatChain conversion, FAT-linked extension, pin exhaustion, staged-allocation
and data faults, torn stream and primary writes, failed flushes, device loss,
and post-truncation reclamation uncertainty. The VFS pin registry is mocked;
the separate VFS recovery-pin tests exercise its production ownership rules.
With `EXFAT_ORDINARY_SPLIT_IMAGE` set to a disposable 12,000-sector image
formatted by `mkfs.exfat -c 4K`, the fixture imports a valid split-sector
entry set, writes and reads it through the production driver, and writes the
image back. Run `fsck.exfat -n` separately for an independent consistency
check. Neither fixture establishes power-loss recovery or guest concurrency.
It also executes the private 0–2 MiB replacement hook with contiguous and
fragmented fresh storage, a 2 MiB old mapping, exact-length source failures,
same-sector and split-sector publication, unsupported TexFAT rejection, torn bitmap and
data/publication writes, free-space exhaustion, failed reclamation, flush
failure, and recovery-pin
handoff. The VFS pin registry and block callbacks are fixture dependencies;
the test does not prove hardware sector atomicity, crash durability, or guest
concurrency.
Split replacement cases cover sector and fragmented cluster boundaries,
255-byte names with unchanged filename sectors, generation conflicts,
snapshot versions, repeated saves and subsequent ordinary write/truncate,
compact old NoFatChain inventories, and maximum old/new FAT-linked ledgers.
Every write and flush boundary injects failure, including writes that partially
or fully changed media before returning an error, cancellation failures,
post-staging metadata read failure, and device replacement. Assertions preserve
old allocations until confirmed/flushed publication and retain recovery state
after uncertainty. With `EXFAT_REPLACE_SPLIT_IMAGE` set to a freshly formatted
disposable image of the same geometry as above, the runner persists the split
file first, then reloads it in a separate fixture invocation for conditional
replacement, repeated saves, and production readback. For independent checks
before and after replacement, compile the fixture with the runner's flags and
invoke `--ordinary-split-image IMAGE`, `fsck.exfat -n IMAGE`,
`--replace-split-image IMAGE`, and `fsck.exfat -n IMAGE` in that order.
The fixture also executes production pinned unlink and empty-directory removal
for same-sector and imported two/three-sector entry sets, including fragmented
cluster boundaries, maximum names, Unicode names, both allocation modes,
unopened files, nonempty directories, retained nodes/cursors, and directories
after contiguous growth or FAT conversion. It checks exact sector-byte
preservation, tombstones, reuse of retired secondary slots, repeated deletion,
and files larger than the safe-save bound without proportional recovery arrays.
Every dirty-flag, directory and bitmap write boundary injects zero-byte, torn,
or complete writes that return failure; every read and flush boundary, device
replacement, allocation/pin exhaustion, malformed/orphan entries and incomplete
ownership proofs are covered. The driver fixture controls VFS registry callbacks;
`vfs_unlink_pin_test.py` and `vfs_recovery_pin_test.py` separately execute the
production VFS dispatch and registry lifetime logic.

For independent retirement checks, set `EXFAT_RETIRE_FILE_IMAGE` or
`EXFAT_RETIRE_DIRECTORY_IMAGE` to a disposable `mkfs.exfat -c 4K` image with
12,000 sectors. The runner seeds a valid split FAT-linked file or NoFatChain
directory, reloads it in another driver
invocation, removes it, checks production readback and reuse, and persists the
result. Run `fsck.exfat -n` independently. To check before and after retirement,
compile the fixture with the runner's flags and use
`--seed-retire-file-image IMAGE` or `--seed-retire-directory-image IMAGE`, run
`fsck.exfat -n`, then `--retire-file-image IMAGE` or
`--retire-directory-image IMAGE` and check again. This verifies successful
interoperability, not crash repair, guest concurrency or hardware durability.

The exFAT fixture executes production pinned rename for same-slot and moved
sets, maximum-length and Unicode names, sector/cluster boundaries, directory
moves and ancestry rejection, FAT-linked/NoFatChain destination growth,
two-cluster extension, conversion and split parent entry sets. It injects each
observed read, write, flush, allocation and removal boundary, including writes
that partly or fully mutate media before returning failure. Assertions cover
contents, metadata, identities/generations, subsequent replacement/deletion,
gate/pin retention, forced-retargeting with a retained superblock, duplicate
imported aliases and cleanup accounting. A full 4,091-cluster directory
exercises maximum conversion/rename inventory. The VFS fixture separately
checks every rename dispatch variant, post-lock finalization, failed handoff,
detach and legacy fallback. Driver registry callbacks and filesystem callbacks
in the VFS fixture remain controlled dependencies.

For independent rename checks, set `EXFAT_RENAME_IMAGE` to a disposable
`mkfs.exfat -c 4K` image of 12,000 sectors. The runner seeds/reloads a split
FAT-linked file, renames it, moves a maximum-length name into a full NoFatChain
directory requiring conversion, verifies existing entries/readback and persists
the result. Run `fsck.exfat -n` independently. Separate fixture invocations of
`--seed-retire-file-image IMAGE` and `--rename-image IMAGE` permit checking the
imported seed before mutation too. These checks establish final-state
interoperability, not interrupted-state repair or guest/hardware validation.

The exFAT fixture additionally creates complete entry sets across sectors and
fragmented cluster boundaries, including 255-byte ASCII names and Unicode
surrogate pairs. It checks root/FAT-linked/NoFatChain growth, two directory
candidates, split parent metadata, conversion, namespace collisions and orphan
rejection. Every observed mutation/read/flush/allocation/removal boundary is
faulted, including torn or fully completed writes returning failure, both
candidate cancellation and cleanup flush failures. Created files subsequently
exercise enumeration, ordinary write/truncate, conditional replacement, rename,
unlink and slot reuse. The maximum inventory combines a 2 MiB child with a full
4,091-cluster directory and 33 conversion-sector pairs. Recovery dependencies
and block callbacks remain controlled; fixtures account for owned heap cleanup.

For independent creation verification, set `EXFAT_CROSS_CREATE_IMAGE` to a
fresh disposable 12,000-sector `mkfs.exfat -c 4K` image, or invoke the compiled
fixture with `--cross-create-image IMAGE`. Valid seed entries place a maximum
name's primary at a cluster's last slot. Production safe creation stages more
than 512 KiB, reads it back and exercises write, replacement, rename, unlink
and creation into retired slots. A full NoFatChain directory also receives a
spanning set through growth/conversion. Run `fsck.exfat -n` on the result.
Seeding uses ordinary production helpers without injected failures; the checks
prove final-state interoperability, not repair after interrupted writes or guest
concurrency, hardware durability or LeakSanitizer coverage.

The same exFAT fixture executes the private 0–2 MiB exclusive-create hook
against prepared VFS nodes. It checks case-insensitive collisions, entry-set
checksum and readback, contiguous and FAT-linked fresh allocation, a fragmented
2 MiB layout, short input, pre-mutation rejection, torn bitmap/data/FAT and
directory-sector writes, flush failure, and retained recovery state after
device identity loss. VFS create admission and pin ownership are controlled
dependencies; the fixture does not prove full guest integration or external
exFAT-tool interoperability.
It also exercises FAT-linked root-directory growth, end-marker relocation,
fragmented child allocation sharing a FAT sector with the root tail, and
partial bitmap, FAT, directory, flush, cancellation, and removal failures.
The in-memory block callbacks and recovery pins remain controlled fixtures;
independent exFAT-tool verification requires exfatprogs.
With `EXFAT_GROWTH_IMAGE` set to a disposable 12,000-sector image formatted
by `mkfs.exfat`, the fixture fills and extends the root using production
functions, writes the image back, and checks readback. Run `fsck.exfat -n`
separately on that disposable image for an independent consistency check.

The exFAT fixture also checks specification-derived compressed identity runs,
uncompressed tables, the recommended table checksum, mandatory ASCII mappings,
malformed/truncated tables, literal U+FFFF, non-ASCII mappings, fixed name-hash
vectors, and lookup/exclusive-create collisions with a hand-encoded existing
entry. It exhaustively checks stream and filename flag bytes, including empty
streams, and verifies AllocationPossible on generated entries. Expectations
are independent of Mangrove's entry and up-case builders; block I/O remains
mocked.

The system-stream regressions independently place a fragmented allocation
bitmap and a 256-cluster fragmented dense up-case table, with poisoned adjacent
sectors. They exercise production discovery, bitmap bit/range and transaction
mapping, up-case loading, ownership proofs, creation and replacement. Additional
cases cover exact chain termination, bounded root-cycle detection,
loops/short/long/out-of-range chains,
free-marked/shared metadata clusters, malformed and duplicate system entries,
allocation and device-I/O failures, and unchanged version confidence on
retryable failures. The formatter-to-mount round trip now runs in the separate
formatting-admission fixture, preserving the system FAT-link and independently
checksummed two-FAT/two-bitmap boot-region assertions. These boot regions
exercise refusal for both active selections; dirty/media-failure/ClearToZero
admission and stale backup flags are checked too. The media, entropy, clock,
and public block callbacks remain fixtures, not guest or interoperability tests.

`storage_coherency_test.py` compiles production block registry/cache, GPT, mutex
and management range-preflight code. Pthread parking/IRQs, VFS policy and media
callbacks are controlled. It covers zero-write GPT initialization/create/delete
refusal, successful disjoint updates, failed flushes, physical parent/child cache
reuse, partial-write invalidation, disjoint cache preservation, malformed ranges,
parent read-only policy, stale IDs, slot/parent-descriptor reuse, and USB discovery
with children registered before their parent. Duplicate physical-root registration
is rejected without changing registry or cache state. Four threads perform 4,000 gated
write/read handoffs. The cache allocates no heap: 64 entries occupy 265,216 static
bytes, 512 bytes more than the baseline. Saved parent IDs add eight bytes per
GPT mapping (128 bytes at the 16-slot bound).

```sh
python3 tests/storage/storage_coherency_test.py gcc-16
python3 tests/storage/storage_coherency_test.py gcc-16 --baseline
```

The baseline mode reads production sources from
`3c16cf3cac8ddca5a32caba691be704f8cc8b0b8` into a temporary directory. It reproduces
a failed GPT array write that changes partition backing data while retaining a
live child and stale cache. The corrected regression requires `BUSY`, zero
additional writes, and unchanged registrations/cache. No hardware, coordinated
retirement or crash recovery is exercised.

`storage_format_test.py` compiles actual FAT32/MGFS/exFAT formatter callbacks,
the storage dispatcher, native format syscall case, shared admission and private
VFS recovery registry. Heap, kernel locks, device registry/quiescence, user
validation/authorization and device callbacks are controlled dependencies.
The FAT32 probe is extracted from production and the exFAT probe is compiled
with its driver; MGFS probe signature/CRC checking is a stated fixture dependency.
Pthread coordination exercises competing admission while a write is paused.

Cases include stale selections, slot reuse at admission and after owner creation,
direct exFAT calls, read-only state, mounted/private/unresolved owners, overlapping
partitions, registered children, partially overlapping and exactly adjacent
siblings, read-only parents, pin/heap exhaustion, confirmed no-write cancellation, native authorization,
successful formatting, partial and failed writes, subsequent read/probe failures,
device loss, failed flush and failed handoff. Rejections assert zero write
callbacks and no writes to replacement registrations. Uncertain paths inspect the
retained image/progress and prevent retry. Original exFAT formatter/system-stream
layout coverage was moved intact from the mounted-media fixture into this fixture.

```sh
python3 tests/filesystems/exfat/storage_format_test.py gcc-16
python3 tests/filesystems/exfat/storage_format_test.py gcc-16 --baseline
```

The baseline command reads commit `3c16cf3cac8ddca5a32caba691be704f8cc8b0b8`
into a temporary directory and reproduces wrong-ID native formatting and direct
formatting despite an unresolved owner. It does not reset or modify the checkout.
All media are in-memory fixtures or disposable ordinary image files. Available
`fsck.fat` and `fsck.exfat` independently check newly formatted images; missing
tools are reported explicitly. Apple's `fsck_exfat` requires a device interface
and is not substituted for a regular-file-capable checker. These checks do not
prove guest concurrency, hardware completion or power-loss recovery.

Setting `FAT32_IMAGE` to a disposable FAT32 image also runs production mount,
later-cluster lookup, file read, and ownership validation against that image.
That image is opened read-only. Setting `FAT32_REPLACE_IMAGE` as well runs a
512 KiB + 1 byte replacement on a second, writable disposable image containing
`TARGET.TXT` at its root. The writable image is modified by this optional
test. The default host suite does not require image formatting tools.
The FAT32 fixture also executes production exclusive create-with-contents for
empty, small, 512 KiB, and 2 MiB files, plus UTF-8 long-name encoding,
short-alias collision, same- and cross-sector slot layouts, source, FAT-copy,
publication, FSInfo, and device-removal failures. With the optional writable
disposable image, it creates and reads back `CREATED.BIN` after replacement.
It also creates `Résumé from Mangrove.txt` through the production long-name
path. `mdir`, `mtype`, and `fsck.fat -n` can then inspect that disposable image
independently; the default host suite does not run those external tools.
The same fixture retires imported LFN sets across sector and cluster
boundaries using production FAT32 functions, and injects torn directory
writes, failed flushes, and removal. `vfs_unlink_pin_test.py` executes the
production VFS unlink, rmdir and rename dispatch and post-lock pin handoff with
controlled superblock and filesystem callbacks.
`fat32_retirement_test.c` executes the production pinned file-retirement path
and scalable ownership validator. It reproduces shared-cluster corruption for
short names and same- and cross-sector LFNs, asserting unchanged media and no
pin on rejection. It deletes generated 128 MiB contiguous and 512 MiB fragmented
files, verifies every configured FAT copy and cursor, and checks tombstones,
slot reuse, allocation beyond logical EOF, active-FAT selection, malformed
chains, preparation failures, and pin exhaustion. Fault cases cover every
write and flush in a small mirrored/FSInfo retirement, plus mid-chain cleanup
read failures, removal, and re-registration on large files. Media, allocations,
and recovery callbacks are controlled dependencies; they do not prove guest
SMP or device durability. Existing VFS tests separately check production
post-lock handoff and teardown ownership.
Setting `FAT32_UNLINK_IMAGE` runs deletion of root `LARGE.BIN` on a writable
**disposable** externally formatted FAT32 image. The fixture's `--image IMAGE
NAME...` mode also deletes imported short and long names; `FRAG.BIN` must have
at least 16 noncontiguous chain links. Use `mkfs.fat`, `mcopy`, `mdir`, `mtype`,
and `fsck.fat -n` independently to construct and inspect such media. FSInfo
free-count hints are invalidated deliberately after deletion; this advisory
checker message is distinct from an allocation or namespace error.
The FAT32 fixture removes empty same-sector and spanning-name directories
through the production pinned path, including a directory with more than
2 MiB of mapped storage. It injects malformed entries, nonempty directories,
allocation and read failures, torn parent-sector writes, FAT-mirror and FSInfo
failures, flush failures, removal, and delayed pin finishing. Setting
`FAT32_RMDIR_IMAGE` runs production removal against a writable disposable
image containing a 200-character empty directory. `mmd`, `mdir`, and
`fsck.fat -n` independently establish the external format and inspect the
successful result; fault injection remains a controlled host fixture.
It creates spanning LFN sets at sector and cluster boundaries, including a
255-byte name that extends a full directory by two clusters. Fault cases
cover each growth write, mirrored FAT admission, publication-sector writes,
flushes, and removal. `FAT32_CROSS_CREATE_IMAGE` and
`FAT32_CROSS_DELETE_IMAGE` run production creation and subsequent deletion
against the named writable disposable image. Run them separately to inspect
the created name and bytes with `mdir`, `mcopy`, and `fsck.fat -n` between calls.
Setting `FAT32_GROWTH_IMAGE` to a separate writable disposable image whose
root has no single-sector free run executes production directory growth and
creates `Mangrove grown.txt` with complete initial contents. The fixture
also injects allocation, FAT-mirror, cluster-initialization, tail-link,
directory-publication, flush, cancellation, and removal failures in memory.
Successful externally formatted images can then be checked independently
with `mcopy` and `fsck.fat -n`; fault injection does not establish post-crash
recovery or hardware write atomicity.
`vfs_node_registration_test.py` executes production registration and
retargeting to check a lookup node cannot be linked to a detached or removed
superblock. Metadata locking and device liveness are controlled fixtures.
The VFS admission fixture also executes the production mount-identity check
for a retired or reused slot; its root/node/path route checks inspect source
ordering rather than execute complete mount publication.

`mgfs_mount_ownership_test.py` builds complete production VFS/MGFS with MFQS's
controlled heap, locks, immutable registry, and regular-file block transport.
Its disposable images are independently formatted by `mkmgfs` and successful
outcomes checked by `mgfsck`. Offline format encoding prepares corrupt ownership,
duplicate references/names, orphan cycles, allocation past EOF, and imported
fragmented/deep layouts; it never services runtime filesystem operations.

It tests writable rejection and explicit read-only recovery, pending mount
reservation against real VFS management preflight, budget/allocation exhaustion,
and every observed read/write/allocation/flush boundary of ordinary, owned,
history, replacement, creation, retirement, and move operations. Torn writes and
removal retain registry owners through forced detach. Clean paths verify no live
pins or driver allocations after teardown. Cursor tests reuse freed copy-on-write
storage and retired Record slots. Locks/device identity are controlled single-threaded
dependencies; this is not SMP, physical hardware, reboot, or power-loss evidence.

Representative Linux measurements: a 31.25 MiB file with 8,000 fragmented extents
and 64 list blocks required 168,208 proof bytes, 260 total mount/read requests,
and approximately 11 ms mount CPU time. A 256-directory chain required 53,920
proof bytes, 822 reads, and approximately 13 ms; moving its subtree preserves every reference and
refuses an ancestry cycle without recursive kernel-stack growth. A four-range fixture needed
49,456 proof bytes. The inventory budget test retained 521,984 ranges using
8,384,576 bytes before safe rejection. The harness allows 16 MiB total heap so
mounted state does not mask the production 8 MiB proof boundary; normal MFQS
continues to use its 8 MiB host heap ceiling.

Ordinary transaction state is 12,920 requested bytes on x86-64, plus an 80-byte
registry pin: 416,000 bytes for 32 transaction/pin pairs, excluding retained
mounted state and nodes. A two-block overwrite uses five writes and one flush;
staged two-block replacement uses nine writes and three flushes; exclusive
creation uses thirteen writes and four flushes in the small root fixture.
Measurements are controlled host requests, not device latency. Safe transactions
keep their separate, larger inventories. LeakSanitizer remains disabled in the
Debian sandbox; explicit heap/pin assertions do not constitute a leak-sanitizer pass.

`mgfs_label_test.py` links complete production block registry/cache, VFS, GPT,
MGFS, management dispatcher and native label case/error translation. IRQs,
parking/thread context, syscall authorization, heap and memory-backed transport
are controlled dependencies. Independent `mkmgfs` images and offline GPT
encoding supply media; `mgfsck` checks successful label set/clear, Unicode and
maximum-length labels, preserved header/unrelated bytes, and remount results.

Faults cover partial writes through superblock header/label/checksum and sector
boundaries, failed flush/readback/handoff, acknowledged lost writes, removal,
allocation failure, pin/registration exhaustion and repeated relabeling. Tests
retain the original/proposed images, verify unknown outcomes remain excluded,
and reproduce same-backing slot reuse without substituting registry logic.
Production GPT parent/child cache aliases and disjoint siblings are exercised.
Pthread barriers cover active-label removal, mount-validation reservations and
ordinary pin admission racing physical-parent retirement. Clean paths assert
zero retained heap/pins; unresolved paths intentionally retain their owners.
These are host dependency/interleaving tests, not guest SMP, controller, reboot,
power-loss or leak-sanitizer verification. Resource measurements are documented
with the [label contract](../filesystem/mgfs/labels.md).

`mgfs_record_lifecycle_test.py` exercises production index growth/cluster deletion,
record allocation/release, directory append and create cleanup with injected
allocation, read, failed-write and partial-write failures. Metadata checksums,
directory lookup/publication in the create fixture, and VFS registration are
fixtures; separate append cases use the production append function. Its optional
source-root and `exhaustion`/`cleanup` arguments reproduce the committed baseline
regressions (legacy release frees the bitmap slot without removing its ID).

`mgfs_stage_cancel_test.py` compiles production MGFS staging, replacement,
and create-with-contents functions, including the locked VFS hook. The create
cases exercise fresh child data, copy-on-write directory append, same-block and separate-block
record publications, bitmap and table write failures, incomplete input,
parent cleanup, and recovery-pin handoff. Block I/O, heap allocation, VFS
authorization and pin ownership are controlled fixtures. A failed block write
can copy zero or half the submitted image into fixture media; this does not
model crash recovery, sector atomicity, or a running guest.

The MGFS fixture also checks bounded operation-local directory mappings,
fragmented/cross-block reads, corruption/read failures, publication uncertainty,
capacity overruns, and rebuilding the view after a create. Existing large-create
cases are unchanged. See [the measured traversal comparison and profiling
commands](mgfs-traversal-performance.md). The profiler modifies temporary copies
only; it does not add target instrumentation.

`vfs_create_contents_test.py` compiles production VFS exclusive-create
admission and return-node registration functions. Its MGFS hook, recovery
pin, block-device removal, and metadata lock are controlled fixtures; the
production MGFS hook and transaction are exercised by the MGFS fixture above.

`file_create_contents_syscall_test.py` compiles production process handle-slot
reservation, object preparation, and native create-syscall functions. It
checks invisible reservations, activation, cancellation, input-copy failures,
full-size 2 MiB input, and VFS error mapping. The replacement syscall fixture
also executes production snapshot copyout callbacks and admission, including
failure after a data chunk and failure at result-state copyout. User page
tables and VFS publication
are controlled fixtures; it does not model guest SMP or durable media.

For guest verification, use a disposable image (`STATE_DIR` outside `.mangrove`)
and QEMU `-snapshot`. Exercise `history` options, nested session saves, external
history replacement followed by `-n`, and completion/editing on a command that
wraps over several rows. Never use the user's persistent image for destructive
history or fault-injection fixtures.

## Checked directory enumeration

`tests/userspace/directory_enumeration_test.py` compiles production VFS, directory-object,
MGFS/initramfs walkers, syscall cases, libc wrappers, and listing/completion
loops with ASan/UBSan. Controlled filesystem, record I/O, process lookup,
user-buffer, allocation, and formatting dependencies exercise entry/EOF/error
translation, untouched output, consumed name-capacity errors, batch deferral,
retry, cancellation, removal, runtime child mounts, and legacy callbacks.
Pthread readers verify complete-batch cursor serialization. The FAT32/exFAT
mount-identity fixtures additionally execute the production checked driver
callbacks, including failed allocation/read retry and corrupt metadata.
These are host production-function tests, not guest SMP or hardware evidence.
On Linux, `ASAN_OPTIONS=detect_leaks=0 make test` disables LeakSanitizer;
allocation/reference assertions and ASan do not constitute a leak-check pass.

## Imported exFAT Unicode names

`tests/filesystems/exfat/exfat_names_test.py` runs `tests/filesystems/exfat/exfat_names_test.c` with ASan/UBSan.
The fixture includes the production driver and supplies mounted block I/O,
allocation, and recovery-pin dependencies. Independently assembled entry sets
cover 128 accented characters, maximum UTF-16 lengths, supplementary pairs,
pairs crossing filename secondaries, split sectors/clusters, the exact native
byte boundary, later-entry visibility, malformed UTF-16, forbidden characters,
zero lengths, bad hashes/checksums, and tiny conversion buffers. Complete
ownership scans include unrepresentable directories and their children;
cross-linked storage, read/allocation faults, and replaced devices fail safely.
A valid custom volume up-case mapping tests a representable query matching a
longer stored name, complete retained identity, coherent snapshot/facts, and
exclusive-create collision rejection.

When `mkfs.exfat` and `fsck.exfat` are available (including `/usr/sbin` on
Linux), the runner creates a fresh disposable 6,144,000-byte image. The separate
`tests/filesystems/exfat/exfat_names_image.py` encoder uses that volume's up-case table to assemble
seven imported entry sets, including a split primary/stream, a 765-byte name,
supplementary characters, and an oversized directory with a child. The imported
entry timestamps use the fixed 2000-01-01 00:00:00 UTC date, with valid zero UTC
offsets and zero 10-ms increments. Placement starts at the discovered root end
marker, preserving preceding system entries across formatter layouts. Independent
`fsck.exfat -n` runs before and after production-driver readback must pass, and
SHA-256 verifies unchanged image bytes. Missing exfatprogs is reported as a
skip; internal fixtures alone are not interoperability evidence.

On a 4 KiB-cluster image made with exfat-utils 1.4.0, containing two directories
and seven files, the protected ownership scan requested 14 sector reads, two
allocations, and 6,662 peak heap bytes, taking approximately 4.2 ms of sanitizer
host CPU time on Apple Silicon. The complete repeated indexed/sequential/readback
exercise requested 185 reads, no writes or flushes, and 10,396 peak heap bytes,
including retained nodes and the mounted up-case table. A temporary CLI adapter
translated the runner's exfatprogs options for exfat-utils; formatter/checker
algorithms were unchanged. Allocator overhead and stack are excluded. These measurements
are not guest, hardware, power-loss, or universal performance guarantees.
The normal Linux invocation disables LeakSanitizer; no QEMU run is part of
this parser/read-only compatibility regression.

## exFAT file-extension initialization

`tests/filesystems/exfat/exfat_extension_test.py` compiles the production exFAT write, allocation,
metadata publication, read and retained-handoff paths under ASan/UBSan. Block
media, allocation, clock samples and the pin registry are controlled dependencies;
the fixture completes the production handoff outside the operation, as VFS does.
Poisoned final-cluster slack catches gaps that structural checkers cannot detect.
Byte-exact cases cover sector/cluster boundaries, contiguous and fragmented FAT
mapping, conversion to FAT allocation, empty files, imported shorter or zero
ValidDataLength, repeated extensions, truncate/regrow and replacement/regrow.
Streaming 4 KiB readback checks ordinary lengths above 2 MiB and the end of the
fixture's available contiguous extent; overflow and ownership/volume limits fail
before mutation. There is no large expected-file heap buffer.

All observed reads, allocations, writes and completion flushes are faulted in
four growth layouts, including split primary/stream sectors: 484 cases, with
zero-byte, torn and fully copied failed writes. The tests assert no false
confirmed byte count, no blind retry,
retained pending state and revoked confidence after possible mutation. Diagnostic
decoding checks that any coherent persisted ValidDataLength only exposes the
correct initialized prefix, even after a publication or completion failure.

```sh
python3 tests/filesystems/exfat/exfat_extension_test.py gcc-16
python3 tests/filesystems/exfat/exfat_extension_test.py gcc-16 --baseline
```

Baseline mode reads the production driver from
`cb29138da7d20f1b0ea3cd96f86e6682d7752311` into a temporary directory. Two
four-byte imported-file cases expose 12 poisoned bytes at offset 16 and 4,092 at
offset 4,196 after successful handoff. The corrected byte-equality assertion was
also observed failing before the production correction.

With regular-file-capable `mkfs.exfat` and `fsck.exfat`, the runner checks seven
independently encoded disposable 4 KiB-cluster images. All free data and imported
slack are poisoned. A split-sector File/Stream entry set, fragmented allocation,
an adjacent blocker and ValidDataLength/DataLength differences are covered.
Independent checks run before and after mutation; production content is checked
before and after a fresh mount, with an independent Python FAT walker/byte oracle
checking the initialized prefix and logical zero tail. Baseline mode checks both
bad-content images too: structural checks pass despite the leak. Missing tools
are explicitly unavailable. On Apple Silicon these checks used exfat-utils 1.4.0
with a temporary CLI adapter, without altering checker logic. They are image-backed
host fixtures, not QEMU, hardware or crash/power-loss tests.

For a four-byte NoFatChain file on the synthetic 4 KiB-cluster fixture, completed
save costs before readback were:

| Write offset | Baseline sector reads/writes | Corrected sector reads/writes | Flushes | Peak requested heap |
| ---: | ---: | ---: | ---: | ---: |
| 16 | 15 / 6 | 16 / 7 | 4 | 12,092 bytes |
| 4,196 | 19 / 17 | 27 / 25 | 6 | 14,160 bytes |

The extra sector pairs initialize required old-allocation gaps. Fresh storage is
not zeroed twice. Transaction size stays 9,120 bytes; allocations, flush counts
and peak heap are unchanged. Counts are fixture driver-sector requests, including
resolved handoff, not measured physical device I/O or block-cache misses.

## Ordinary exFAT creation

`tests/filesystems/exfat/exfat_create_test.py` compiles production driver functions with ASan/UBSan
through `tests/filesystems/exfat/exfat_create_test.c`. It runs ordinary create/mkdir across seven
layouts: same-sector and spanning entry sets, fragmented directory clusters,
root/non-root growth, split parent updates, contiguous extension/conversion,
and retired-slot reuse. Maximum native names, case collisions, strict malformed
tails, reserved END bytes, shared allocation, current versus trusted permissions,
empty-file growth, nested creation/removal, tombstones and repeated operations
are asserted. The fixture faults allocation/read/write/flush/removal boundaries. Allocation/read failures and every successful-path sector write,
flush and removal boundary are injected, including zero-byte and torn writes,
pin exhaustion and cancelled-allocation cleanup flush failure. Assertions account
for all requested heap cleanup; unresolved fixture state is freed only by the
fixture's explicit teardown, never treated as production recovery.

Block I/O, heap, mount references and pin registry dependencies are controlled.
The separate VFS fixture executes real dispatch, lock-release and reference
handoff with controlled filesystem hooks; the driver fixture's completion
wrapper is not a second VFS implementation. Neither proves guest concurrency,
real-device persistence or post-reboot recovery. The normal Linux invocation
`ASAN_OPTIONS=detect_leaks=0 make test` disables LeakSanitizer.

With exfatprogs available (also searched in `/usr/sbin`), the runner independently
formats two disposable 6,144,000-byte, 4 KiB-cluster images. One is seeded by
`exfat_names_image.py` with complete imported names, including unrepresentable
Unicode names and their directory children. The complete production parser
obtains that imported internal directory node; native pathname limits remain
unchanged. Production ordinary mkdir/create
adds children to the imported directory without changing its original child,
then publishes maximum-length spanning sets, grows/converts a NoFatChain
parent, preserves imported visibility through checked enumeration, checks Unicode
collisions, writes/reads an initially empty file and renames/unlinks it, and
creates/removes an empty directory. `fsck.exfat -n` must pass before and after.
No persistent development images are used. Missing tools are explicitly reported.

The runner prints current requested heap, read/write/flush counts and CPU time
for each layout and independent-image lifecycle. Proof memory is released
before mutation except for retained ancestor timestamp images. Current combined
bounds are in [exFAT resource accounting](../filesystem/exfat/README.md#metadata-and-resource-bounds).
Independent-image peaks include cached live nodes and upcase storage; they are
not per-transaction retention or hardware latency measurements.

## MGFS positional writes and explicit history replacement

`tests/filesystems/mgfs/mgfs_positional_write_test.py` compiles the complete production MGFS
driver with its existing block/heap fixture and extracts actual VFS write,
read, seek, resize/truncate and history dispatch. The byte oracle is supplied
independently of production reads. Media, locks, authorization and pin services
are controlled dependencies. The fixture media callback supports the aligned
extent-read groups issued by production readback; neither it nor the test
reimplements allocation, checksums, mapping, reclamation or mutation.

```sh
python3 tests/filesystems/mgfs/mgfs_positional_write_test.py gcc-16
python3 tests/filesystems/mgfs/mgfs_positional_write_test.py gcc-16 --baseline
```

Baseline mode selects production commit
`487b9bdd6a536820c2f2ffcddcca691568c7de23` without changing the checkout.
The desired positional-write invariant must fail. Diagnostic runs independently
confirm that a successful one-byte VFS overwrite shrinks a 4 KiB file to one
byte and frees its data block. A second baseline case begins with a readable
fragmented mapping, appends successfully into a lower free physical hole, then
observes production read failure while the filesystem still reports clean.
The corrected growth path respects the existing ascending-extent constraint.

Coverage includes 92 repeated prefix/middle/EOF/appended writes across inline,
contiguous and fragmented storage through 2 MiB; block crossings; exact byte
readback; zero/overflow/sparse-write rejection; nonzero resize refusal;
truncate-to-empty; and unchanged ownership of allocation past EOF, including
small extent-backed files. Files with 130 and 512 extents retain their linked
list bytes and bitmap exactly on overwrite. Unsupported mapping growth does
not change original contents. History tests exercise a stale cached node size,
locked latest-record merge, oldest-first limits, reset/empty replacement,
extent-to-inline trimming and explicit reclamation of a fragmented 2 MiB file.
These are sequential aliases, not a guest concurrency claim.

All successful-path read, write and allocation boundaries across seven ordinary
and exact-history layouts are injected: 216 cases, including zero-byte, half
block and fully persisted writes that return failure. Tests detect changed
media before record-generation publication, require quarantine after possible
mutation, and reject further write/truncate/history mutations without new
writes. Definite pre-mutation failures preserve data and remain retryable.
Six prefetch-read or optional grouped-read buffer failures complete through
safe read fallback and receive exact byte verification; failed writes cannot
use this allowance. Existing staged replacement, lifecycle and history
regressions remain separate and retain their established failure assertions.

The 4 KiB prefix-overwrite comparison, measured before verification readback:

| Production VFS workload | Driver block reads | Block writes | Heap requests | Peak requested heap |
| --- | ---: | ---: | ---: | ---: |
| Baseline one-byte overwrite (incorrectly shrinks) | 7 | 4 | 2 | 840 bytes |
| Corrected one-byte overwrite (retains 4 KiB) | 5 | 4 | 3 | 1,512 bytes |

The additional 672 bytes are the existing working-extent array for this
21-record fixture: a retained extent-backed file no longer incorrectly takes
the inline shrink path. No new transaction struct, retained buffer or maximum
stack block is introduced. Corrected 130/512-extent overwrites request three
allocations, peak at 9,360/36,864 bytes, perform 11/20 driver reads and four
writes. Unchanged extent lists and suffix blocks receive no writes. Values are
requested fixture heap and driver I/O, not allocator overhead, physical-device
I/O or disk latency; ordinary arrays still scale with mounted record capacity.
Native macOS GCC and the existing Darwin/Linux sanitizer/linker selection are
used without modifying target source for the host. These tests do not establish
guest integration, SMP, hardware persistence or post-reboot recovery.

## Unique-target exFAT deletion

`tests/filesystems/exfat/exfat_delete_identity_test.py` compiles the complete production exFAT
driver and extracts ordinary/trusted VFS unlink and rmdir dispatch without
reimplementing their selection logic. Block I/O, allocation, synchronization,
mount references, authorization dependencies and the recovery-pin registry
are controlled; successful completion executes the production handoff callback
and requires the VFS operation lock to have been released. This is host
production-path verification, not guest SMP or real-device testing.

Independent imported entry assembly covers 36 duplicate-name combinations
across direct pinned, ordinary VFS and trusted VFS routes, files/directories,
allocated/zero-allocation targets, adjacent/split sectors, fragmented directory
clusters and ASCII/Unicode case pairs. Additional cases cover 255-byte names
spanning three sectors, mixed file/directory ambiguity, a non-root NoFatChain
parent, repeated rejection, missing targets and ordinary unique Unicode
deletion. Duplicate allocations are disjoint: allocation overlap is not used
to obtain the expected rejection. Every rejected operation checks the complete
media image, zero writes/flushes, unchanged parent generation, intact confidence
and absent transaction/pin state. The direct duplicate path allocates no heap.
Five observed identity-scan read boundaries are faulted; malformed later sets,
orphan secondaries, invalid chains and incomplete ownership budgets also
prevent mutation. Existing retirement tests separately cover uncertain writes,
flushes, reclamation and retained recovery evidence after successful admission.

```sh
python3 tests/filesystems/exfat/exfat_delete_identity_test.py gcc-16
python3 tests/filesystems/exfat/exfat_delete_identity_test.py gcc-16 --baseline
```

Baseline mode uses production commit
`4f75dccbf2ebcf5712cace210dfa904c3478297b` without changing the checkout.
The corrected rejection assertion must fail against that driver; diagnostic
reproducers then confirm both unlink and rmdir return success while the same
case-insensitive pathname still resolves. Equivalent unique-name workloads
measure completed direct deletion, including resolved handoff, before any
verification lookup:

| Operation | Baseline sector reads | Corrected sector reads | Sector writes | Flushes | Allocations | Peak requested heap |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Unique unlink | 43 | 44 | 9 | 7 | 6 | 15,348 bytes |
| Unique rmdir | 46 | 47 | 9 | 7 | 6 | 15,348 bytes |

These are fixture driver sector requests, not physical-device I/O. The fixed
transaction remains 9,120 bytes. Existing stack-owned FAT/stream read views
are 544/560 bytes on the measured 64-bit host; they persist only for the locked
identity scan and add no retained state. Larger parents require more reads;
the ownership-proof limits still apply before deletion can mutate storage.
The forced target flags with GCC `-fstack-usage` report a 1,200-byte
`exfat_find_entry_unique_view` frame and a 2,352-byte `exfat_delete_pinned`
frame. The former replaces the 96-byte first-match wrapper on the deletion
path, increasing that wrapper's active stack use by 1,104 bytes. These are
individual bounded frame measurements, not a full call-chain measurement.

With `mkfs.exfat` and `fsck.exfat` available, six fresh disposable 6,144,000-byte
images check ASCII/Unicode duplicate file and directory refusal and successful
unique Unicode deletion. The seeder independently decodes the formatter's
upcase table, encodes hashes/checksums, timestamps 2000-01-01 midnight UTC,
disjoint allocations and split primary/stream entries. Rejected images must be
SHA-256 identical. Baseline mode reproduces both wrong deletions on disposable
images too. Independent structural checks run before/after every image; they
alone do not detect the demonstrated ambiguous-target identity defect.

Verified macOS checks used exfat-utils 1.4.0 with a temporary CLI adapter
mapping the runner's exfatprogs-style `-c 4096` to `-s 8`; formatting/checking
algorithms were unchanged. Missing tools are explicitly reported. The normal
Apple Silicon host suite still cannot compile the unchanged x86 `rdtsc` clock
fixture; that test is unavailable, not passing. No power-loss, post-reboot
recovery or QEMU guest guarantee follows from these tests.

## Zero-allocation exFAT directories

`tests/filesystems/exfat/exfat_zero_directory_test.py` compiles production exFAT code and extracts
the actual VFS create/mkdir/unlink/rmdir dispatch functions for ASan/UBSan tests.
It controls block I/O, heap, mount references, locking and the pin registry;
resolved-pin finishing asserts that operation_lock has been released. It does
not simulate guest SMP or real-device persistence.

The fixture covers zero-allocation lookup, indexed/sequential enumeration,
ownership validation, empty removal, distinct identities, first-child ordinary
create/mkdir, Unicode and maximum native names, split parent entry sets,
cross-sector/cluster children, subsequent growth, retained cursors, directory
moves and ancestry rejection. Complete 2 MiB child contents are staged and read
back under contiguous and fragmented allocation. Malformed lengths, clusters,
allocation flags and checksums reject without mutation. Every successful-path
read, write, flush and allocation boundary is injected, including registration
replacement, zero-byte/torn writes, pin exhaustion and cancelled-allocation
cleanup failures. Unresolved state is explicitly dismantled only by fixture
teardown, never presented as production recovery.

When exfatprogs is available (including `/usr/sbin`), the runner formats a
disposable 6,144,000-byte image with 4 KiB clusters. An independent encoder
imports two unallocated directories, a split parent entry set and a later file.
`fsck.exfat -n` validates the initial image. Production VFS dispatch creates
files/directories, ordinary writes populate a file, and maximum names force
directory growth. A second fixture process loads fresh production driver state,
verifies saved contents, removes the new namespace, and preserves the later
file. Independent checks must pass after both mutation phases. No persistent
development images are touched; missing tools produce an explicit skip.

The runner measures current first-child transaction and complete image-lifecycle
costs, including return nodes where indicated. Source/readback buffers, allocator
overhead and stack are separate. Bounds and limitations are in
[exFAT](../filesystem/exfat/README.md#zero-allocation-directories).
`ASAN_OPTIONS=detect_leaks=0 make test` disables leak detection. These tests do
not establish guest, hardware, power-loss or post-reboot recovery guarantees.

`make test-time` runs known-value epoch/calendar tests, the production C time
library against host C-locale references under ASan/UBSan, CMOS sample-decoder
fixtures, the production clock state under concurrent readers, and the status
query ABI fixture. It covers untrusted, unavailable, invalid, and trusted
states, invalid calendars, whole-second progression, Unix timestamp ordering,
and arithmetic boundaries. Host lock operations, uptime, entropy and CMOS
register sampling are controlled dependencies; this does not qualify physical
RTC hardware or clock accuracy. The `fstest time` phase in
[`make test-guest-mgfs`](guest-filesystem.md) checks the status syscall against
the strict API and actual trusted QEMU clock, process CPU accounting,
monotonic elapsed time and persisted guest transcript.

`make test-guest-shutdown` is a bounded two-vCPU power-lifecycle check. It
requests ACPI shutdown and reboot while an interactive Shoot process is
waiting for its child, verifies that each command line is saved to the history
file, and boots the same disposable MGFS image after the reboot request to
verify history readback inside the guest. The reboot phase uses QEMU
`-no-reboot` to capture the guest-reset event before a host-managed follow-up
boot; it does not qualify firmware completing a same-process reboot cycle or
physical hardware. Each stopped image is independently checked with `mgfsck`.

`make test-startup-presentation` compiles the production optional-network
device warning reporter and checks that successful startup output uses the
shared OS version macros without routine progress announcements. It also
checks that existing boot warnings and failures remain in the production
sources. This is host-side presentation evidence, not a native boot test.

`make test-power-presentation` executes the production `shutdown` and `reboot`
command bodies against controlled power-transition adapters. It verifies each
message is flushed before the syscall, that a nonreturning normal transition
produces no additional command output, and that returned failures remain
visible. It also executes the production quiesce/sync lifecycle helper to
check silent success and visible sync failure, and checks retained process and
ACPI diagnostics. It does not emulate ACPI hardware or a native power event.

## exFAT volume-label recovery

`test-storage` runs `tests/filesystems/exfat/exfat_label_test.py` under ASan/UBSan. It executes
production label, root-growth, ownership proof, private VFS registry and handoff
functions with controlled block callbacks, heap requests, management guard and
locks. Existing
labels, an earlier end marker, and completely occupied roots are tested on
independently `mkfs.exfat`-formatted disposable images. Production readback,
explicit dirty-flag inspection and `fsck.exfat -n` check confirmed outcomes.
Every requested allocation, sector write and flush is faulted, including torn
writes, cleanup, removal/re-registration and handoff failure. Unresolved state
is discarded only by fixture teardown, never by the production operation.

The runner prints measured heap and I/O per layout. The fixed transaction is
9,120 bytes; clean images add 2,060 bytes, percentage images 2,064 bytes. The
conservative maximum retained private owner is 1,072,012 requested bytes,
including the maximum admitted 1 MiB upcase table, state, wrappers, registry pin
and growth inventory: 34,304,384 bytes for 32 owners, excluding allocator
overhead. Root labels have no ancestor timestamp inventory. Temporary proof
memory is separate. These fixtures do not establish leak detection, guest
interoperability, power-loss or reboot recovery.

## exFAT timestamp and allocation metadata

`test-storage` runs `tests/filesystems/exfat/exfat_metadata_test.py` with ASan/UBSan against the
production driver, calendar converter and transaction completion functions.
Block callbacks, heap, pin registry and UTC clock samples are controlled.
Creation, mkdir, extending/in-place writes, replacement, truncate, rename,
unlink and rmdir assert timestamps, preserved creation fields, ordinary-read
access preservation, bitmap accounting, PercentInUse and boot dirty state.
Unavailable or out-of-range UTC must fail before writes; empty truncate is a
no-op without requiring UTC. Calendar boundaries include 1980, leap day and
2107. Shared ancestor sectors test fresh predecessor images after a move.
Every successful-path write, flush, removal and allocation boundary is faulted,
including zero-byte, torn and fully copied failed writes and completion failures.
Uncertain fixture pins are freed only by explicit teardown.

A 4,095-level imported tree exercises the maximum ancestor inventory without
relying on cached parent pointers. Current requested memory, per-operation I/O,
flushes and CPU time are printed; calculated bounds are documented in
[exFAT](../filesystem/exfat/README.md#metadata-and-resource-bounds). VFS/object/
syscall fixtures separately assert `MG_ERR_TIME_UNAVAILABLE` propagation.

When exfatprogs is available, the runner independently formats a disposable
image and imports a genuinely split primary/stream file. Production mount
admission must write nothing. A 2 MiB ordinary write, 513-byte replacement,
nested creation/write/removal and truncate are checked with production readback
and `fsck.exfat -n`. Independent Python decoding explicitly verifies timestamps,
UTC indicators, odd-second increments, checksums, lengths, allocated bitmap
counts and PercentInUse, including 52% usage. This does not infer metadata
correctness merely from a checker pass. No persistent developer images are used.
Leak detection is disabled by the normal ASAN setting; guest/hardware/reboot
and power-loss testing remain separate.

## exFAT SYSTEM-entry visibility

`test-storage` runs `tests/filesystems/exfat/exfat_system_test.py` with ASan/UBSan. The fixture
executes the production driver and extracted production VFS pathname lookup,
open/close, retained-handle reads/writes, indexed enumeration, create and unlink
dispatch. Sequential driver enumeration runs under the operation lock.
Credentials, mount registry, mutexes, block I/O, heap and recovery registry are
controlled dependencies; pin finishing asserts operation_lock is released.

Both ordinary files and directories cover all combinations of SYSTEM with
READ_ONLY, HIDDEN and ARCHIVE. Descendants, Unicode upcase matches, collisions,
split entry sets, retained identities, replacement/rename/removal and credential
or mount-policy rejection are asserted. An unrepresentable SYSTEM name returns
NAME_TOO_LONG without changing output and does not conceal the later file.
Bitmap/upcase/label records are excluded by entry type, not file attributes.
Malformed SYSTEM checksums, invalid bitmap identifiers and shared descendant
allocation reject safely. Every successful-path retained-handle write and flush
boundary is faulted, including zero-byte, torn, fully copied failed writes and
removal; uncertainty retains the pin and does not advance the handle offset.

The internal fixture assembles controlled entry sets. Independently, when
exfatprogs is available (including `/usr/sbin`), the runner formats a disposable
image with `mkfs.exfat` and independently encodes the attribute matrix, Unicode
names, both directory allocation modes and a primary at byte 480. Production
mount, namespace readback and mutations run against that image; `fsck.exfat -n`
checks both initial and completed media. A fresh driver process remounts the
completed image and verifies visibility, attributes, contents and ownership
without writes; hashes verify unchanged input and readback images. Missing tools
are explicitly reported.
The runner prints requested heap, sector requests, flushes and host CPU time.
The visibility change adds no transaction storage or new scan; exposed entries
use the existing node cache. No guest, hardware, reboot or power-loss guarantees
follow from these host fixtures; the normal ASAN setting disables leak detection.

## FAT32 replacement flush and sync

`test-storage` runs `tests/filesystems/fat32/fat32_replace_flush_test.py` with ASan/UBSan against
the production conditional replacement, recovery callback and sync functions.
Controlled block callbacks record every barrier's phase, publication state,
old allocation, reclamation and FSInfo progress. One through four FAT copies,
fragmented allocation, empty and maximum-size contents, stale generations,
definite cancellation and incomplete handoff are checked. Every successful-path
write and flush boundary injects zero-byte, torn or fully copied failed writes,
removal and re-registration; old allocation must remain untouched until the
publication barrier confirms. Unresolved state is disposed only by explicit
fixture teardown. Normal sync has no heap or metadata writes and cannot resolve
an outstanding pin.

The existing identity fixture executes actual block admission with FAT32 sync,
including absent flush callbacks, failures and saved registrations. The VFS
recovery fixture executes actual clean-unmount dispatch, ensuring failed sync
restores all mounts/references and unlocks operation_lock before returning.
Its filesystem hook is controlled; it does not replace the driver fixture.
The native/VFS replacement fixtures separately verify generation and uncertain
error propagation. No guest or real-device persistence is simulated.

With mkfs.fat, mtools and fsck.fat available (including `/usr/sbin`), the runner
formats a disposable 64 MiB FAT32 volume and seeds target/neighbor files. Actual
driver replacement and creation are read back independently through mcopy and
checked with fsck.fat. Separate copies fail staging, publication, completion or
cancellation flushes. Issued-write images preserve the neighbor; prepublication
failures preserve target bytes. Checker-visible orphan allocation is permitted
in uncertain images, and even checker-clean cleanup/cancellation remains an
uncertain runtime result with retained evidence. These observations do not
establish all possible power-loss states or repair guarantees. The original
image is hash-checked unchanged; persistent developer images are never used.

The runner prints memory, sector requests, flushes and CPU time. Bounds and exact
completion guarantees are in [FAT32 replacement](../filesystem/fat32/replacement.md).
The normal `ASAN_OPTIONS=detect_leaks=0 make test` disables leak detection;
guest, hardware, reboot and power-loss testing remain separate.

## exFAT allocation boundaries

`exfat_system_test.py` also runs `exfat_boundary_test.c` using the production
exFAT driver and extracted VFS lookup/open/read/write/unlink dispatch. Block
callbacks, credentials, locks, heap, clock and recovery dependencies are
controlled fixtures. Tests cover contiguous extent counts versus zero-based
indices, the first and final valid clusters, invalid starts, zero/overflowed
counts, bitmap ranges, cleanup, creation, coherent snapshots, conditional
replacement, truncation and deletion. Final-cluster creation injects failed,
torn and fully copied failed writes and failed flushes, retaining quarantine
and recovery ownership.

Disposable independently formatted images exercise one- and two-cluster
contiguous extents ending at the final cluster, a fragmented FAT chain ending
there, and an imported one-cluster overflow. Valid input and completed output
are checked with `fsck.exfat`; malformed input must be rejected without writes.
Input images remain unchanged. These checks do not establish guest, hardware,
crash recovery or power-loss durability. The shared arithmetic predicate adds
no allocations, scans or sector I/O.

## FAT32 volume-label recovery

`test-storage` runs `fat32_label_test.py` under ASan/UBSan. The fixture executes
production FAT32 transactions, storage-management dispatch, the native label
syscall case/error translation and private VFS registry/handoff functions.
Heap, mutex/IRQ services, process authorization, user mappings and block callbacks
are controlled dependencies. A bounded sector overlay models issued writes;
independent input images are never modified.

Disposable `mkfs.fat` images cover existing/unlabeled roots, completely occupied
roots and fragmented multi-cluster roots. Setting, replacement, explicit removal,
repeated relabeling and growth are checked with `fsck.fat`, mtools label listing
and unrelated-file readback. Failed clear cannot return successful native or VFS
status. Every observed write/flush/read/allocation boundary is faulted, including
zero-byte, torn and fully copied failed writes, cancellation, removal, changed
device identity, authorization-to-dispatch slot reuse, pin exhaustion and failed
finishing. Retained owners block
retries, formatting and overlapping partition views. Duplicate labels, hidden
end-marker tails, corrupt mirrors, ambiguous OEM aliases and namespace collisions
are rejected without changing media. See [label resources and guarantees](../filesystem/fat32/labels.md).

These are production-function and independent-image checks, not guest, hardware,
reboot, power-loss or leak-detection evidence. The normal restricted-host ASAN
setting disables leak detection.

## MGFS logical-operation handoff

`mgfs_handoff_test.py` compiles complete VFS/MGFS with a pthread mutex transport
and controlled identity/block/heap services. It pauses the mutator exactly after
raw operation unlock, before pin completion, and starts lookup, read, indexed
enumeration, metadata and a queued history writer. Create, write, truncate,
history and replacement must block observers until completion. Failed callbacks,
removal, torn writes, flush failures, recursive/canceled admission, wrong-owner
completion and 32-pin exhaustion are checked. An opt-out negative control
exposes premature IO and legacy count-only EOF through the same production units;
normal guarded operations must wait for resolved completion.
The fixture uses independent formatting/checking and preserves unexpected failure
artifacts. It does not emulate IRQs, DMA, guest scheduler fairness or SMP.

The guard uses 32 bytes inside the superblock, increasing its x86-64 size from
104 to 152 bytes, with no per-operation heap allocation or additional media I/O.
The small concurrent fixture measured 91,896 peak requested heap bytes including
mount/proof/transaction state. Existing ordinary/staged recovery inventories and
the 32-pin registry limit are unchanged. A waiter retains one mount reference;
FIFO waiting uses the existing thread queue fields. The separate production
mutex/scheduler fixtures qualify wake and cancellation bookkeeping under controlled
host interleavings. `init_storage_test.py` verifies that completed boot discovery,
not instantaneous xHCI runtime idleness, controls storage initialization admission.

`xhci_lifecycle_test.py` also drives the production command/transfer completion
routers during a controlled scheduler yield, then expires the deadline before
the waiter resumes. Completed success must be consumed without quarantining
the device/controller. A delayed STALL, invalid command Short Packet, canceled
generation, real missing completion and unavailable clock retain their failure
contracts. The pre-correction wait ordering fails this regression. DMA,
hardware and scheduling remain fixtures; run it with
`ASAN_OPTIONS=detect_leaks=0 python3 tests/storage/xhci_lifecycle_test.py` for ASan/UBSan
without claiming LeakSanitizer qualification. Native fault injection and the
captured first-flush image-preparation failure are recorded in
[issue #2](https://github.com/oyer0/mangrove/issues/2).

Native safe-file/application campaigns are opt-in with `make test-guest-safe`;
see [guest safe files](guest-safe-files.md) for real Reed/Shoot, maximum-content,
read-only, persistence and recovery-owner evidence. `make test-storage` includes
production FAT32/exFAT forward-stream regressions and historical I/O-amplification
controls; `make test-sync` includes resolved-handoff admission probes across all
three filesystems. Evidence-parser rehearsals remain host infrastructure tests
and are not counted as successful guest execution.

## Checked namespace failure boundaries

`tests/filesystems/vfs/vfs_lookup_callers_test.py` executes production mountpoint, account-store
and network-configuration admission with controlled lookup dependencies. IO
must stop admission; confirmed absence retains legitimate creation behavior.

`tests/filesystems/vfs/vfs_lookup_test.py` compiles complete production VFS and each persistent
filesystem with controlled transport, heap and scheduler dependencies. Independently
formatted MGFS/FAT32/exFAT media is checked before and after execution. ASan/UBSan
regressions distinguish IO, NOT_FOUND, NO_MEM and DEVICE_GONE; sweep read failures
through global/anchored lookup and ordinary/trusted/identity-checked rmdir; and
require active-CWD BUSY, no writes/flushes/pins on refused mutations, healthy
readback and remount. Unlink and rename error exits are also checked.

Historical VFS controls reproduce false NOT_FOUND and deletion of a live CWD
on all three filesystems. They are intentionally unsafe disposable-media
controls, not supported operation semantics. Native metadata-fault qualification
is described in [device lifetime](guest-device-lifetime.md#online-pathname-metadata-errors).

## Native runtime bounds and standards coverage

`make test-libc` compiles actual libc sources with private symbols under ASan and
UBSan, checks each public header in isolation, and links a target-ABI reference
program against the actual Mangrove libc and libm archives. It checks 49,716
bounded `strncat` cases and requires the historical implementation to trigger
ASan; it also checks string APIs, strict C17 headers and target ABI types,
supported integer/floating formatting and conversion against host references,
C-locale ctype, aligned allocation, allocator failures, streams, errno and
termination callbacks. The shared secret-input fixture also verifies that
login's cursor remains hidden until the flushed prompt reaches its blocking
input boundary. Its math group compares 63,235 deterministic float,
double and x87 long-double real results against MPFR, plus 27,689 complex
results against MPC/MPFR, and repeats original Mangrove math sources under
ASan/UBSan. Inputs cover the x87 exponent range, including
large quotient gaps, least-subnormal cases, and values near the `log1p`
singularity. It checks scaling, remainder, cube root, hypotenuse, power, gamma
and log-gamma, error functions, exponential, logarithm, hyperbolic, inverse-hyperbolic, inverse-trigonometric
and trigonometric families. Selected `exp2`/`log1p` and power inputs use all
four rounding modes; other witnesses cover power boundaries, complementary-
error-function recurrence and tails, and negative underflow direction. The
complex group covers all function families at binary32, binary64 and x87
extended precision, including subnormal values and large exact trigonometric
arguments, branch points, signed zeros and near-pole inputs. Its sampled
maximum x87 component errors are 10.5 ULP for `cacosh` and 4.9 ULP for the
other inverse families; direct operations and inverse/power operations use
8-ULP and 32-ULP limits respectively. The
tested MPFR bounds are 1 ULP for cube root,
1.5 ULP for hypotenuse, and 2 ULP for sampled power, error functions, and exponential/logarithm,
hyperbolic, inverse-trigonometric and trigonometric results;
cube root and signed power inputs are also checked in all four rounding modes, including large negative powers near the x87 exponent limits. Selected transcendental
directed-mode results are checked within the stated bound, and calls must
preserve the caller's mode; correct directed rounding is not promised. Tests
cover type-generic dispatch, x87 precision, rounding modes, exception flags, unsupported-symbol,
corrupted-result, corrupted-quotient, per-family coverage and incomplete-record
controls. A separate FMA probe compares 1,592 float, double and x87-long-double operations against MPFR fused results at target precision, across all four rounding modes and including cancellation, full-range reciprocal exponents, subnormal addends, overflow, underflow, infinities and NaNs. The largest FMA accumulator is 8,216 bytes of bounded stack workspace, plus fixed call frames; the implementation makes no dynamic allocation.
The MPFR comparison requires an x86-64
host and the MPFR shared library; host libc does not supply the tested math
functions. Signaling-NaN comparison macros and exceptional `ilogb` flags are
also checked. Leak detection is disabled. This does not establish full C17,
locale, FILE-stream or exhaustive numerical conformance; see the
[runtime inventory](../architecture/c-runtime.md) for current behavior and
limitations.

The existing `test-guest-safe` campaign includes a small `fstest` runtime probe
on safe-file create/verify phases. It runs through the native CRT and process
path, checks integer formats, errno, allocation and floating-point call ABI, and
requires an `atexit` transcript witness after volume operations. It reuses the
secondary-volume QEMU workflow rather than a runtime-specific guest harness.

## Clipboard service and editor integration

`make test-clipboard` runs production daemon/client host regressions, including
an 8 MiB transfer, UTF-8 chunk boundaries, atomic replacement, immutable read
pins, session/PID isolation, cancellation, expiry, restart, queue/table resource
limits and allocation/transport failure. It is included in the MTS host profile.
`make test-reed` also checks clipboard edits and all prepared-edit allocation
boundaries. ASan/UBSan run with leak detection disabled.

`make test-guest-clipboard` runs one two-vCPU source-matched Q35/OVMF guest with
USB keyboard/storage and disposable images. It defaults to KVM on Linux; use
`GUEST_CLIPBOARD_ACCEL=tcg` explicitly for TCG. `GUEST_CLIPBOARD_BUILD` selects
build storage and `GUEST_CLIPBOARD_OUTPUT` must name a new retained evidence
directory. The [clipboard contract](../architecture/clipboard.md) describes the
native workload and remaining qualification limits. The optional `cliptest`
binary is not installed in normal images.

### Large safe files and Reed documents

`make test-large-safe-files` runs production MGFS, FAT32 and exFAT creation,
replacement and coherent snapshots with 8 MiB + 17 byte inputs. It verifies exact
streamed bytes, stale-generation rejection and allocation failure before writes.
MGFS also exercises an 8 MiB fragmented mapping and bounds snapshot reads
linearly. Controlled media adapters are separate from independent filesystem
checking; backend flush/failure and ownership suites remain required evidence.
The replacement/create syscall fixtures stage 8 MiB through actual production
functions, including immutable input, partial allocation cleanup and invalid
address-size controls. `test-sync` includes the large backend fixture.

`make test-reed-large`, also included in `test-reed`, runs production buffer and
editor helpers under ASan/UBSan on 16 MiB multiline and single-line documents.
It covers delta undo/redo, exact dirty state, prepared large range edits and
allocation failures. Scalar-decode counters bound repeated navigation work;
reported timings are host observations, not native performance guarantees.
The safe-save fixture covers 8 MiB load/save/create and the optional final
newline, conflicts, uncertain results and failure preservation.

For native large-document qualification, use the existing disposable workflow:

```sh
make -j4 RTC_UTC_TRUST=1 binaries cliptest
python3 tests/guest/guest_clipboard_test.py --build-dir build --large
```

It opens an 8 MiB UTF-8 source, copies and cuts it, undoes the cut, pastes in a
separate Reed process, verifies undo/redo, saves, reopens inside Reed, returns
to Shoot, and shuts down. Stopped-image checking validates both files exactly
and runs independent MGFS verification. Select All copies editable bytes; the
target retains its own final-newline setting. The source is preserved byte for
byte; a target configured with a final newline gains one if the source lacks it.
KVM/QEMU is not physical hardware or power-loss qualification. LeakSanitizer
remains unqualified when `ASAN_OPTIONS=detect_leaks=0` is used.

## Native process arguments

`make test-process-arguments` runs sanitizer-backed production capture,
argument-page construction and spawn orchestration fixtures, including
initial-process and session-context kernel argv setup. Coverage includes
4,096 entries, empty and UTF-8 strings, the full 8 MiB admission boundary,
the derived count ceiling, invalid pointers and overflow, each staging allocation and each argument frame/map failure in the
4,096-entry workload, and rollback of child/parent handles before publication.
`make test-list` additionally checks 1,000 operands and failure recovery.
`make test-shoot` also covers 2,048 sorted glob matches and actual `del`
dispatch against a disposable host filesystem model, including recursive trees,
hidden entries and failures before dispatch. These are host fixtures.

`make test-guest-arguments` builds trusted-UTC production binaries and launches
one isolated two-vCPU Q35/OVMF guest (KVM by default on Linux). It verifies the
native CRT ABI with 4,096 argv entries and a 2 MiB argument, invalid-pointer
rejection, 512 sorted UTF-8 Shoot glob matches, ordinary and recursive deletion,
hidden-entry preservation, child status and orderly shutdown. The normal
64 MiB root image has 1,071 Record slots, so the simultaneous filesystem
fixture is deliberately sized to fit that image. The runner compares exact
persisted transcripts and independently checks the stopped MGFS image before
removing disposable media. Explicit TCG runs use `GUEST_ARGUMENTS_ACCEL=tcg`;
Linux uses KVM by default and fails clearly when it is unavailable. This does not establish physical-hardware or
power-loss qualification. Leak detection remains a separate host limitation.
