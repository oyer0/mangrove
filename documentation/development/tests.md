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
detachment, and pin exhaustion; it does not exercise an exFAT metadata commit
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
production VFS unlink and rmdir dispatch and post-lock pin handoff with
controlled superblock and filesystem callbacks.
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
