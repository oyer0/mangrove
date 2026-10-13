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
| `test-history` | Shoot history/ring logic; VFS and MGFS merge/snapshot; MGFS write/truncate, spanning tombstones, record-ID/create lifecycle, and 2 MiB private replacement and creation staging | Separate state mocks and fault-injected block fixtures; not crash recovery. |
| `test-sync` | Scheduler handoff publication/selection, termination admission, process and session retirement, retained handle lookup, 2 MiB file-replacement and exclusive-create syscall copying and libc ABI, streamed VFS snapshot expectation and coherent node metadata, mutex wake/ownership, recursive block gate | Deterministic interleavings and pthread workers; scheduler parking, IRQs, process resources, user mappings, and VFS outcomes are fixtures. No guest scheduling claim. |
| `test-storage` | Block retirement, GPT backing cleanup, mutation admission; AHCI completion/recovery, xHCI teardown, BOT retry, cache/read-ahead and USB batching | Pthread/IRQ fixtures, simulated MMIO, timing, DMA, command and device failures. These are production-path host tests, not hardware integration or physical recovery verification. |
| `test-terminal` | UTF-8 decoder; terminal scroll/render/overlay/input positioning | In-memory framebuffer and fixture font/locks; not graphical QEMU. |
| `test-reed` | Reed buffer edits and production snapshot/load, conditional-save, exclusive-create, Save As, stale-generation conflict prompt, and saved-baseline paths | Native file calls and terminal events are controlled fixtures; no guest UI or filesystem integration. |
| `test-dev-tools` | Development-image update script guard | Disposable images and missing/busy/idle tool fixtures. |
| `test-password` | SHA-256, HMAC, PBKDF2 and authentication | Host compiler/entropy fixture; comparison with Python references. |

`test-launch` remains a compatibility alias for VMM, heap and history tests.
The oversized harness has been split into subsystem files. Temporary C programs
are extracted from production definitions; fixtures duplicate only the required
surrounding ABI. These tests do not establish whole-kernel integration or ABI
compatibility by themselves. Build and guest tests are still required.

The mgfs_device_identity_test.py fixture executes production block admission,
FAT32 mounted read/write and FAT-entry helpers, exFAT mounted I/O, dirty-flag
updates and sync, plus MGFS identity-bound paths. Pthread-backed waiters and
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
retryable failures. A production formatter-to-mount round trip verifies the
system FAT links. Independently checksummed two-FAT/two-bitmap boot regions
exercise refusal for both active selections; dirty/media-failure/ClearToZero
admission and stale backup flags are checked too. The media, entropy, clock,
and public block callbacks remain fixtures, not guest or interoperability tests.

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
supplementary characters, and an oversized directory with a child. Independent
`fsck.exfat -n` runs before and after production-driver readback must pass, and
SHA-256 verifies unchanged image bytes. Missing exfatprogs is reported as a
skip; internal fixtures alone are not interoperability evidence.

On a 4 KiB-cluster image with two directories and seven files, the protected
ownership scan requested 14 sector reads, two allocations, and 6,344 peak heap
bytes, taking approximately 6 ms of sanitizer host CPU time. The complete
repeated indexed/sequential/readback exercise requested 486 reads, no writes or
flushes, and 10,396 peak heap bytes, including retained nodes and the mounted
up-case table. Allocator overhead and stack are excluded. These measurements
are not guest, hardware, power-loss, or universal performance guarantees.
The normal Linux invocation disables LeakSanitizer; no QEMU run is part of
this parser/read-only compatibility regression.

## Ordinary exFAT creation

`tests/exfat_create_test.py` compiles production driver functions with ASan/UBSan
through `tests/exfat_create_test.c`. It runs ordinary create/mkdir across seven
layouts: same-sector and spanning entry sets, fragmented directory clusters,
root/non-root growth, split parent updates, contiguous extension/conversion,
and retired-slot reuse. Maximum native names, case collisions, strict malformed
tails, reserved END bytes, shared allocation, current versus trusted permissions,
empty-file growth, nested creation/removal, tombstones and repeated operations
are asserted. The fixture runs 1,155 allocation/read/write/flush/removal fault
cases. Allocation/read failures and every successful-path sector write,
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

Seven controlled layouts measured 12,104–23,824 peak requested bytes for empty
file creation and 12,108–23,828 for mkdir, including the 72-byte VFS return node,
with 3–8 flushes. Proof memory is freed before mutation. The maximum conversion
inventory fixture separately accounts for the retained transaction; current
bounds and scalability limitations are in [exFAT](../filesystem/exfat/README.md#ordinary-file-and-directory-creation).

The 4,090-cluster conversion inventory measured 57,036 peak/retained requested
transaction bytes (excluding the preallocated VFS node), 16,431 reads, 48 writes,
seven flushes and ten allocations, taking approximately 105–118 ms host CPU time.
This is not a universal latency bound. The retained upper bound is 1,825,152
bytes for 32 pins across mounts. The complete proof remains separately bounded
at 1,196,032 temporary requested bytes. Allocator overhead and stack are excluded.
The exact peak and I/O figures for image lifecycles include cached live nodes;
they do not measure a single retained transaction.

Complete allocated-tail validation measured 402 sector reads for an empty create
in a 128-cluster contiguous directory and 413 for its fragmented counterpart,
with three writes in both cases. This adds 129–131 reads compared with stopping
slot selection at its first candidate, without extra heap or per-entry chain
restarts. Independent image workflows requested 1,357/1,808 reads, 143/167 writes,
44/51 flushes and 28,140/34,524 peak heap bytes (fresh/imported), respectively;
those peaks include cached live nodes and the mounted up-case table.

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

Measured first-child peaks are 18,608–22,788 requested bytes including the VFS
node, with 3–6 flushes. A maximum-name 2 MiB create requests 39,096 transaction
bytes and five flushes; 32 retained pins account for 1,251,072 bytes. Independent
image population/removal measured 28,920/22,136 peak requested bytes,
716/2,255 reads, 83/32 writes and 43/42 flushes. Source/readback host buffers,
allocator overhead and stack are excluded; image figures include cached nodes.
The separately freed proof and remaining limits are documented in
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

With 4 KiB clusters, existing-label replacement requests 18,512 peak/retained
bytes, 59 reads, five writes and four flushes; marker-relocating growth requests
27,120 bytes, 104 reads, 17 writes and six flushes. The fixed transaction is
9,096 bytes; separate clean-state images request 2,060 bytes. The conservative
maximum retained private owner is 1,069,860 requested bytes, including the
maximum admitted 1 MiB upcase table, driver state, root wrappers, registry pin,
transaction and root-growth inventory: 34,235,520 bytes for 32 owners, excluding
allocator overhead. The temporary ownership proof is released before mutation;
its independent 1,196,032-byte budget must also be included in preflight peaks.
At maximum admitted 1 MiB clusters, label growth initializes at most 2,048
sectors and has at most 2,057 write attempts and six flushes. Host CPU timing
is printed per layout; these fixtures do not establish real-device latency,
leak detection, guest interoperability, power-loss or reboot recovery.
