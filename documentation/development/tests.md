# Focused test coverage

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

The ordinary write/truncate pin fixture executes production VFS dispatch,
post-unlock handoff, file-object translation, and native syscall result
translation with controlled filesystem callbacks and mutexes. It covers
confirmed partial writes, uncertain outcomes, failed pin finishing, device
detachment, and pin exhaustion; the controlled VFS fixture does not itself execute a filesystem metadata commit
or a guest device.

It covers timekeeping, terminal, Reed, MGFS version admission, VMM, heap,
history, synchronization, storage, development-image guard, and password tests.

| Target | Production code exercised | Test boundary |
| --- | --- | --- |
| `test-vmm` | User mapping/path allocation functions | Physical tables, mutexes and shootdowns are fixtures; no SMP integration. |
| `test-heap` | Allocation/free and local validation | Heap backing is host memory; growth is a fixture. |
| `test-history` | Shoot history/ring logic; VFS and MGFS merge/snapshot; MGFS write/truncate, spanning tombstones, record-ID/create lifecycle, and 2 MiB private replacement and creation staging | Focused state/block fixtures plus complete VFS/MGFS ownership-admission, recovery and cursor regressions; controlled hardware, not crash recovery. |
| `test-mfqs` | Complete VFS and MGFS/FAT32/exFAT units: mount, creation/recovery handoff, ordinary write/read/resize, snapshot/conditional replacement, nonempty exclusive creation, nested namespace lifecycles and real close/unmount/remount | Independent Python byte model, evidence-gated quick/extended campaigns, exact replay, disposable independently checked images, poisoned slack/free storage and historical negative controls; controlled single-threaded hardware/context/clock dependencies. Broader qualification remains incomplete; missing tools report INCOMPLETE. |
| `test-sync` | Scheduler handoff publication/selection, termination admission, process and session retirement, retained handle lookup, 2 MiB file-replacement and exclusive-create syscall copying and libc ABI, streamed VFS snapshot expectation and coherent node metadata, mutex wake/ownership, recursive block gate | Deterministic interleavings and pthread workers; scheduler parking, IRQs, process resources, user mappings, and VFS outcomes are fixtures. No guest scheduling claim. |
| `test-storage` | Block retirement, GPT backing cleanup, mutation admission; AHCI completion/recovery, xHCI teardown, BOT retry, cache/read-ahead and USB batching | Pthread/IRQ fixtures, simulated MMIO, timing, DMA, command and device failures. These are production-path host tests, not hardware integration or physical recovery verification. |
| `test-terminal` | UTF-8 decoder; terminal scroll/render/overlay/input positioning | In-memory framebuffer and fixture font/locks; not graphical QEMU. |
| `test-reed` | Reed buffer edits and production snapshot/load, conditional-save, exclusive-create, Save As, stale-generation conflict prompt, and saved-baseline paths | Native file calls and terminal events are controlled fixtures; no guest UI or filesystem integration. |
| `test-dev-tools` | Development-image update script guard | Disposable images and missing/busy/idle tool fixtures. |
| `test-password` | SHA-256, HMAC, PBKDF2 and authentication | Host compiler/entropy fixture; comparison with Python references. |

`test-launch` remains a compatibility alias for VMM, heap and history tests.
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
FAT32_NAMESPACE_IMAGE="$testdir/names.img" python3 tests/fat_exfat_mount_identity_test.py gcc-16
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
python3 tests/storage_coherency_test.py gcc-16
python3 tests/storage_coherency_test.py gcc-16 --baseline
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
python3 tests/storage_format_test.py gcc-16
python3 tests/storage_format_test.py gcc-16 --baseline
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

`tests/directory_enumeration_test.py` compiles production VFS, directory-object,
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

`tests/exfat_names_test.py` runs `tests/exfat_names_test.c` with ASan/UBSan.
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
`tests/exfat_names_image.py` encoder uses that volume's up-case table to assemble
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

`tests/exfat_extension_test.py` compiles the production exFAT write, allocation,
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
python3 tests/exfat_extension_test.py gcc-16
python3 tests/exfat_extension_test.py gcc-16 --baseline
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

`tests/exfat_create_test.py` compiles production driver functions with ASan/UBSan
through `tests/exfat_create_test.c`. It runs ordinary create/mkdir across seven
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

`tests/mgfs_positional_write_test.py` compiles the complete production MGFS
driver with its existing block/heap fixture and extracts actual VFS write,
read, seek, resize/truncate and history dispatch. The byte oracle is supplied
independently of production reads. Media, locks, authorization and pin services
are controlled dependencies. The fixture media callback supports the aligned
extent-read groups issued by production readback; neither it nor the test
reimplements allocation, checksums, mapping, reclamation or mutation.

```sh
python3 tests/mgfs_positional_write_test.py gcc-16
python3 tests/mgfs_positional_write_test.py gcc-16 --baseline
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

`tests/exfat_delete_identity_test.py` compiles the complete production exFAT
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
python3 tests/exfat_delete_identity_test.py gcc-16
python3 tests/exfat_delete_identity_test.py gcc-16 --baseline
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

`tests/exfat_zero_directory_test.py` compiles production exFAT code and extracts
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

The `test-time` target also executes the production system clock and CMOS
sample decoder under ASan/UBSan with controlled RTC availability, UTC trust,
invalid calendars, whole-second advancement, arithmetic boundaries, and
concurrent readers. Host lock operations, uptime, entropy, and CMOS register
sampling are substituted; this does not validate physical RTC hardware or
clock accuracy.

## exFAT volume-label recovery

`test-storage` runs `tests/exfat_label_test.py` under ASan/UBSan. It executes
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

`test-storage` runs `tests/exfat_metadata_test.py` with ASan/UBSan against the
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

`test-storage` runs `tests/exfat_system_test.py` with ASan/UBSan. The fixture
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

`test-storage` runs `tests/fat32_replace_flush_test.py` with ASan/UBSan against
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
reproduces historical false NOT_FOUND and EOF through the same production units.
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

Native safe-file/application campaigns are opt-in with `make test-guest-safe`;
see [guest safe files](guest-safe-files.md) for real Reed/Shoot, maximum-content,
read-only, persistence and recovery-owner evidence. `make test-storage` includes
production FAT32/exFAT forward-stream regressions and historical I/O-amplification
controls; `make test-sync` includes resolved-handoff admission probes across all
three filesystems. Evidence-parser rehearsals remain host infrastructure tests
and are not counted as successful guest execution.
