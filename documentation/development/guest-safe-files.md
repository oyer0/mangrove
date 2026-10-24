# Native safe-file and application qualification

`make test-guest-safe` builds an isolated UTC-trusted system and uses disposable
secondary USB volumes, normal login, Shoot, deviced/volumed and native syscalls.
It never installs test binaries in release payloads or opens developer media.
Dependencies and Q35/OVMF/xHCI topology are shared with the
[secondary-volume runner](guest-secondary-volumes.md). The additional host UDP
listener binds only loopback. QEMU user networking and the existing guest
network stack carry test notifications; no external server or DNS is required.

```sh
make test-guest-safe \
  GUEST_SAFE_BUILD="$PWD/build/guest-safe-build" \
  GUEST_SAFE_OUTPUT="$PWD/build/guest-safe-acceptance" \
  GUEST_SECONDARY_ACCEL=tcg
```

Output directories must be new. The default application run reserves 3 GiB of
free artifact capacity per campaign, with two campaigns for each filesystem.
The Python entry point accepts `--backend`, `--repetitions`, `--run-id`, bounded
`--timeout`, `--readonly`, `--negative-control` and `--faults`.
Requested coverage is explicit in the report; a reduced command does not claim
the omitted read-only, negative-control or fault behavior. Exact run identifiers reproduce
the independently specified payload and logical operation sequence. Image and
binary hashes, native assertions, keyboard actions, framebuffer witnesses,
waited process status, QEMU accounting and failures remain in the selected
output directory. Host inspections occur only after QEMU has stopped and exited.

## Native operations and application evidence

Every filesystem runs the same public native safe-file workload. Complete
contents have lengths 0, 1, 511, 512, 4,096, 8,193 and 2,097,152 bytes. Expected
bytes are calculated independently in C and Python from byte position, run
identifier and version. Observed filesystem output never supplies expected data.
The host namespace oracle preserves each format's presentation: mtools' trailing
directory slash is a listing delimiter, and Mangrove's short-only FAT aliases
use uppercase ASCII spelling. Expected names are declared independently;
generic Unicode casefolding is not used.

The workload checks exclusive nonempty/empty creation, failed collisions with
unchanged output handles, coherent snapshots, unchanged result state on a short
snapshot, independent-handle conditional replacement, stale refusal, successful
snapshot refresh, repeated replacement, ordinary mutation after replacement,
offset preservation, rename with retained handles, exact readback/EOF,
close/reopen, namespace uniqueness, allocation/slot reuse, missing parents,
permission denial and oversized-input rejection. ASCII and accented filename
cases distinguish MGFS byte-sensitive names from FAT32/exFAT volume-specific
case equivalence. Owner inspection must show no retained owner for the tested
volume on confirmed paths; unrelated root-service transactions are not mistaken
for leaked secondary-volume ownership.

A real interactive Shoot child writes through output redirection, explicitly
persists history and exits. Native process wait and close must succeed. Real
Shoot scripts then copy the result and execute a deliberately failing child.
The failure must return status 1 and prevent a following command from running.
The host independently checks both redirected files, history records and absence
of the forbidden continuation. Input redirection is not an implemented Shoot
capability and is not claimed.

For a bounded two-vCPU Reed workflow that does not require the host's loopback
UDP rendezvous, use:

```sh
make test-guest-reed GUEST_REED_BUILD="$PWD/build/guest-reed-build" \
  GUEST_REED_ACCEL=tcg
```

This target uses the ordinary production hybrid kernel and a test-only `fstest`
mode. Its two-vCPU interactive Shoot child receives normal emulated USB HID
events. It checks command completion, cursor insertion, a command longer than
the terminal width, redirection, `history` Up/Down draft restoration, command
completion, directory completion, output readback and returning to the home
prompt. It lists a disposable `hello world!` file, then launches a per-run
Shoot script that opens that same file through both escaped and quoted argv
forms before running Reed on the existing test file. The controller observes
the opened filename and contents for each form. It then observes the edited
dirty buffer, sends `u`, observes the
clean `base` buffer, sends the documented Shift+U chord, and observes
`editedbase [+]` before saving. It uses Save As, returns to Shoot, reads the
saved Reed file, and exits. QMP accepting a key event alone does not qualify an
editor action; the rendered state transition is required. The test also checks
the exact redirected contents, persistent command history, Reed source and
copy contents, waited child statuses, terminal restoration, and independent
stopped-image MGFS checking. This is a focused MGFS workflow, not qualification
across all filesystem backends or physical hardware. The interactive child
transcript contains 41 public `SHOOT1` checks; the evidence validator rejects
missing, altered, or out-of-section operation records. The verify phase of the
conflict target below separately reopens the saved copy inside Reed.

For native stale-save conflict and redo qualification without a host loopback
UDP listener, use:

```sh
make test-guest-reed-conflict \
  GUEST_REED_CONFLICT_BUILD="$PWD/build/guest-reed-conflict-build" \
  GUEST_REED_ACCEL=tcg
```

This focused MGFS target builds test-only `fstest` and Reed rendezvous code and
boots two-vCPU QEMU TCG with `RTC_UTC_TRUST=1`. `fstest` starts Reed on the
existing file. Reed creates a run-bound marker only after opening the file and
capturing its save expectation; `fstest` then replaces the file through the
guest VFS and publishes a second marker. Reed continues without host access to
the mounted image. The ordering is deterministic, and the marker waits are
bounded. These test paths are omitted from normal Reed and `fstest` builds.

The October 22, 2026 source-matched run passed with two CPUs online. Retained
framebuffer text shows the clean `base` buffer after undo and then
`editedbase [+]`; the test-only status row records
`U modifiers=2 action=redo bytes=10`, confirming the USB HID Shift+U event
reached Reed's redo binding. Reed saved that redone buffer directly into the
stale-save path. Cancellation preserved the external replacement and kept the
unsaved edit, which was then saved under a new name. The verify phase reopened
the saved file inside Reed and confirmed normal terminal restoration. Create
and verify child statuses were zero, and independent stopped-image MGFS checks
were clean. This qualifies the exercised QEMU/MGFS workflow, not physical
hardware behavior or other filesystem backends.

Only the create phase launches the integrated interactive Shoot child. Verify
and read-only phases qualify their own editor-completion states without
requiring the create-phase shell action counter. Every phase still requires
its native child status, exact file evidence and independent stopped-image
checks; a missing editor or create-phase shell witness fails qualification.

The October 22 acceptance failure after Shoot submitted `exit` exposed a
scheduler current-thread lookup spanning CPU migration; see the
[SMP ownership contract](../architecture/smp.md#scheduling) and
[Issue #1](https://github.com/oyer0/mangrove/issues/1). A controlled two-vCPU
experiment captured the stale lookup before mutation and reproduced the
unchanged restored-stack guard after dispatch rollback. It did not implicate
Shoot, Reed, or allocator backing. The original interrupted secondary image
had 87 allocated blocks without owners; it remains preserved, unrepaired
failure evidence, independently of the scheduler correction.

An earlier source-matched production-default two-vCPU TCG run after the
scheduler ownership correction completed create, verify and read-only phases.
All three native child statuses were zero; both CPUs were online, terminal
restoration and Shoot exit were observed, and independent root/secondary MGFS
checks reported zero errors and warnings. It recorded 41 Shoot, 333 safe-file,
18 Reed and 24 volume assertions in create, followed by 104/20/23 safe-file,
Reed and volume assertions in verify and 17/9/23 in read-only. That run
predated native redo and conflict qualification; the focused run described
above provides those results. These QEMU results support the tested behavior,
not completion of all Stage 5 acceptance or physical-hardware qualification.

The full `test-guest-safe` campaign retains its host loopback UDP rendezvous
for broader external-mutation workflows. The focused
`test-guest-reed-conflict` path coordinates the same-guest VFS mutation through
bounded file markers, so it works when host loopback binding is denied. Host
safe-save conflict regressions remain complementary evidence; they do not
replace the native stale-save check.

In the broader `test-guest-safe` external-host mutation path, unmodified Reed
loads a text file and receives real USB keyboard input. The runner observes
rendered load, insert, save and command-prefix states. An independent native
handle changes the file after Reed's first confirmed save. The next Reed save
must display a conflict. Cancellation must keep the newer on-disk contents and
the editor's edits. Save As must refuse an occupied name, then exclusively
create a missing destination containing those edits. Both the original and
new file are checked through native snapshots and independent stopped-media
decoding. Reed's waited exit status is mandatory.

Reed owns its alternate screen; the parent cannot print over it. Before launch,
a bounded request/acknowledgement exchange establishes the existing native UDP
route to a loopback host listener, including initial ARP readiness. At most 64
100 ms receive attempts are allowed; transport outcomes are recorded. After the
external file mutation, a run-bound datagram acknowledges completion. It gates
the next key, carries no file contents or filesystem authority, and cannot
substitute for the native assertions, conflict screen or media checks. Missing,
wrong-run, nonlocal or duplicate external-change notifications fail. Filesystem
mutations are never retried by this protocol.

Each successful native child ends with authorized, confirmed secondary unmount.
A fresh boot uses the same root and secondary images without repopulation or
repair. It verifies every safe-file payload and namespace, launches Reed on the
persisted Save As file, and confirms another unmount. A separate genuinely
write-protected boot verifies snapshots, exclusive-create denial, unchanged
output handles and Reed's denied Save As while retaining edits. The entire
secondary image must remain byte-identical. Reed currently requests a writable
handle to open an existing file; the read-only editor case therefore uses a new
buffer and does not claim existing-file read-only editing support.

The retained-reference USB-removal workload also checks stale native snapshots
and replacement after logical loss and after reattachment. Both must return
DEVICE_GONE; snapshot output and state remain unchanged. Exclusive creation from
a removed CWD likewise refuses the old binding without changing its output
handle. These checks extend the existing identity-bound replacement campaign,
including fresh-handle readback and independent stopped-media checks.

Wrong-content controls change one stopped-media data byte without changing the
guest oracle. Both host comparison and the real native verification child must
reject byte zero. The child must exit 43, omit Shoot's success continuation, and
leave the wrong bytes intact. Missing execution, incorrect counts/statuses,
malformed transcripts, failed launch, timeout or incomplete application steps
cannot qualify as PASS. Evidence-validator tests are infrastructure checks,
not simulated successful guest execution.

## Failure and recovery evidence

The existing [device-lifetime runner](guest-device-lifetime.md) also accepts
`--mutation replace`. A coherent native snapshot binds the independent handle
before the intended conditional replacement. MGFS campaigns inject first-write,
staging-flush, entry-publication and old-allocation cleanup failures. Publication
and cleanup sector addresses come from the independent fixture decoder. The
replacement becomes inline, so its first allocation-bitmap write can only be
old-storage reclamation after publication.

The native result must be UNCERTAIN, followed by actual authorized inspection of
an unresolved owner with the correct selected/physical identities. Unmount must
remain BUSY after file closure. Physical USB removal, child exit and normal root
operations must preserve that owner and its offline identities. Apparently
checker-clean media does not resolve runtime uncertainty. Failed media is kept
without repair or writable remount. The missing-fault control must fail because
replacement actually succeeds, not because of a launch or adapter failure.

A copied previously qualified safe-file image can be selected with
`--backend` and `--source-media` for replacement write/flush qualification. The
source is never attached or changed. The runner independently validates target
bytes and records the source hash, checks the copy after removal, and uses the
same native volume service, pin inspection and process lifecycle. For example,
an additional first-write control uses the exact run identifier from the
selected qualified image's report:

```sh
python3 tests/guest/guest_lifetime_test.py --build-dir build/guest-safe-build \
  --output build/replace-write-control --backend fat32 --mutation replace \
  --kind write --source-media build/qualified-secondary.img --run-id "$RUN"
```

The application runner's `--faults` option runs the flush control; first-write
controls use the explicit command above. FAT32/exFAT
sector-specific publication/cleanup schedules remain separately covered by the
production-function sanitizer regressions rather than inferred from MGFS guest
results.

## Resolved handoff admission

MGFS, FAT32 and exFAT use the existing per-mount FIFO logical-operation guard.
It precedes the raw operation mutex and remains owned across raw unlock and
resolved pin completion. The completing thread reacquires the raw mutex without
recursively acquiring admission. Readers cannot observe a driver's temporary
confidence gate as UNCERTAIN, absent content or EOF. Genuinely unresolved pins
remain registered; admission is released and subsequent operations follow
sticky uncertainty or device-loss policy. Private, exclusively reserved label
owners retain their existing raw-lock completion protocol.

The production VFS/driver regression pauses immediately after raw unlock and
queues actual host threads for lookup, read, streamed snapshot/metadata,
enumeration and another mutation. It covers ordinary write/truncate, create,
replacement and rename, completion refusal, torn writes, flush failure, offline
removal and registry exhaustion. MGFS history remains separately covered.
Disabling only logical admission reproduces the false UNCERTAIN window on
FAT32/exFAT. Mutex, allocation, IRQ and transport dependencies in these host
interleavings are controlled; production FIFO and real guest scheduling have
separate regressions. No new gate, pin inventory or superblock allocation is
introduced: the existing 32-byte guard and 160-byte superblock are reused.

## Forward read costs

FAT32 snapshots validate the complete mapping and then keep a forward cluster
cursor through their 4 KiB transfer chunks. An identity-bound active-FAT view
is local to mapping validation, ordinary reads and snapshots. Physically
consecutive mapped clusters are coalesced only within the snapshot scratch
buffer after following their FAT links. They do not restart the chain for
every chunk. exFAT ordinary reads and snapshots use a file-local forward cursor
and an identity-bound FAT-sector view; snapshots retain that view across chunks.
The file cursor has filesystem bounds, not directory-scan bounds. Both paths
preserve sink errors and confirmed read prefixes. exFAT bytes beyond
ValidDataLength are returned as zeros without reading poisoned allocation slack.
Views die before unlock; no cached mapping or new recovery state survives an
operation. The target snapshot prologues reserve 6,384 bytes for FAT32
and 4,832 for exFAT; the nested exFAT cursor reader reserves 624 bytes. These
individual frames are below the existing 64 KiB thread kernel-stack allocation;
they are not a measured whole-call-chain stack maximum.

Production-function ASan/UBSan fixtures cover fragmented/contiguous allocations,
cluster sizes, maximum snapshots, copyout rejection, read failures and removal.
An independently formatted FAT32 image also checks an exact maximum snapshot
without changing media. Real guest images independently check both filesystems.
Historical controls reject the previous algorithms for their measured I/O,
rather than an adapter error: with 512-byte clusters, a 2 MiB FAT32 snapshot
issued 1,058,305 reads versus 4,163 with forward traversal; the exFAT fixture
issued 8,398,856 versus 12,329. These are controlled driver-sector measurements,
not hardware throughput. Namespace proofs, allocation scans and other known
metadata-I/O amplification remain separate scalability work.

## Resource and guarantee boundaries

The test-only `fstest` adds two static 2 MiB buffers; they are present even when
another mode is selected. Native syscall input staging and filesystem
transaction budgets retain their existing bounds. One editor and its parent
are used at a time; directory enumeration is bounded to seven declared files.
Parent observation of the first editor save has a 90-second condition deadline.
The runner bounds each boot, keyboard sequence, QMP events and diagnostics.
Application boots have a default 600-second deadline (configurable from 30 to
600 seconds), including the maximum-content allocation workload. It bounds
failure detection and never substitutes for an operation or completion witness.
Reports record wall/CPU time, runner RSS, artifacts and per-device QEMU
reads/writes/flushes. Guest heap peaks are not instrumented; QEMU counters are
integrated workload observations, not isolated driver or hardware costs.

The native maximum-content exercise is distinct from large interactive editor
stress: the keyboard-driven editor uses small valid text. Native pathname,
Unicode and mapped-allocation limits are unchanged. Host MFQS, sanitizer fault
regressions and independent checkers provide separate evidence. They do not
replace real guest execution.

The campaigns here do not exercise the current global shutdown path. The
production poweroff/reboot path now quiesces applications and leaf services
and synchronizes mounted filesystems. Two-boot observations do not prove
power-loss atomicity, persistent recovery or general reboot repair. TCG
single-vCPU qualification does not cover SMP, real hardware, macOS or leak
detection.
