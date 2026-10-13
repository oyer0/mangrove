# exFAT

Mangrove includes a native exFAT filesystem driver and formatter for ordinary
removable-media use. The implementation follows the published exFAT on-disk
format and does not define Mangrove-private directory entries or metadata
sidecars.

## Supported geometry and validation

The current block layer supports exFAT on 512-byte logical sectors. The driver
validates the main and backup boot regions, boot checksum, revision, FAT and
cluster-heap bounds, cluster count, root-directory cluster, allocation bitmap,
and up-case table before publishing a mount. Cluster size is bounded to 1 MiB.
Serious structural corruption causes probe or mount refusal rather than an
attempted repair.

Both boot regions are written by the formatter. Mount validation requires a
valid main region and a structurally valid, consistent backup region. Mangrove
does not provide an exFAT repair operation.

## System streams and admitted layouts

The Allocation Bitmap and Up-case Table primary entries describe FAT-chained
streams (Microsoft specification sections 7.1.3 and 7.2.3). They have no
ordinary-file `NoFatChain` flag: contiguous placement also requires valid FAT
links. Bitmap reads, writes, allocation searches, transaction metadata updates,
up-case loading, and ownership proofs all use that same chain interpretation.
The formatter links each multi-cluster system stream separately.

Before admitting a mount, Mangrove checks exact system-chain lengths and
`FFFFFFFF` termination, cluster bounds, allocation bits, and disjoint ownership
of the bitmap, up-case table, and root directory. This is a bounded metadata
check, not a complete filesystem checker or recovery operation. It uses at most
1 MiB of temporary ownership bitmap and 262,144 cluster claims; volumes outside
those proof bounds are refused rather than partially validated. Root discovery
also detects FAT cycles in constant space and requires a reachable end marker
within 65,536 directory entries; larger or completely full root directories
without an end marker remain unsupported.

Only one-FAT/one-bitmap volumes are admitted. Bitmap identifier zero belongs to
FAT zero; reserved BitmapFlags bits are not file-stream flags. NumberOfFats=2
is reserved by the specification for TexFAT, whose active and inactive
FAT/bitmap pairs are not ordinary mirrored copies. Mangrove does not implement
TexFAT transactions and refuses both active selections before mounting or
mutating. Backup VolumeFlags are stale; selection and admission use the main
boot region.

Volumes arriving with VolumeDirty, MediaFailure, or ClearToZero set are also
refused. Mangrove does not perform the required recovery or flag-transition
protocol for these incoming states. Dirty state set by an admitted Mangrove
mount remains part of its existing runtime write/flush lifecycle. No crash
atomicity or power-loss recovery is established.

Older Mangrove-formatted volumes whose multi-cluster system streams contain
separate EOC entries are malformed and are refused; admission does not
silently repair on-disk metadata.

## Files and directories

The driver supports lookup, enumeration, reads, writes, extension, truncation,
file and directory creation, ordinary-file deletion, empty-directory
deletion, and rename or move within one mounted exFAT filesystem. It supports
contiguous `NoFatChain` streams and FAT-chained fragmented streams. The
allocation bitmap is authoritative for cluster ownership, and FAT chains are
bounded and checked for loops and out-of-range entries.

File lengths and offsets are 64-bit and are not limited by FAT32's 4 GiB file
size boundary. Reads respect both valid-data length and data length. New file
space is initialized before it becomes readable.

Directories use standard exFAT file, stream-extension, and filename entry
sets. Mangrove verifies secondary counts, entry-set checksums, name hashes,
cluster fields, and lengths. Stream entries require `AllocationPossible`;
`NoFatChain` additionally requires a nonempty valid cluster extent. Filename
entries have zero allocation flags. Reserved bits are ignored during decoding
and preserved when updating an existing entry; ownership preflight remains
conservative about reserved stream bits. Allocation-bitmap, up-case-table,
and volume-label entries are filesystem metadata and are not exposed as
ordinary files.

For a non-root contiguous directory, the validated stream length determines
its complete capacity, including clusters beyond the first. Invalid or
unaligned lengths and extents beyond the cluster heap are rejected. An open
directory cursor retains its byte offset but reads the directory's current
allocation mapping under the mount operation lock on each call. Confirmed
growth preserves existing byte offsets, so a cursor can continue through a
new mapping; concurrent namespace changes may still cause entries to be
skipped or repeated. Once a cursor reaches the end, it remains ended.

## Names, attributes, and labels

Path components are converted between UTF-8 and exFAT UTF-16, including valid
surrogate pairs. Invalid encodings and exFAT-forbidden characters are
rejected. Lookup is case-insensitive through the volume's validated up-case
table, while directory enumeration preserves stored casing. Compressed identity
runs use their exact stored counts; compressed and uncompressed tables must
cover U+0000–U+FFFF and preserve the specification's mandatory ASCII mappings.
Names equivalent under the up-case mapping cannot coexist.

The directory and read-only attributes affect VFS behavior. Hidden, system,
and archive attributes are preserved as exFAT metadata, but Mangrove does not
invent POSIX permissions, symbolic links, or special files for exFAT.

The native exFAT volume label is read by the probe layer and can be changed on
an unmounted volume through `diskutil`. Label input must be valid UTF-8,
convert to valid UTF-16, fit the exFAT label limit, and satisfy exFAT's native
character restrictions. Formatting creates an unlabeled filesystem.

## Write and lifecycle behavior

A writable mount marks the volume dirty before exposing it. Mutating
operations update directory entry sets, the allocation bitmap, FAT state, and
stream lengths through the mounted filesystem instance. Sync or clean unmount
flushes writes and clears the dirty flag only after successful completion. A
read-only backing device is mounted read-only; writable initialization errors
are not silently downgraded.

Device removal follows the generic block/VFS generation-safe lifecycle. An
operation remains tied to its original block-device instance and fails if that
instance disappears; it cannot continue against a same-port replacement.

Lookup nodes have mount-local IDs that remain stable across Mangrove writes,
truncation, and supported renames, including moves to another directory.
Confirmed mutations advance a mount-local generation. A failed mutating write
or flush can revoke version confidence, so a later snapshot cannot present a
confident version. The native snapshot path can copy one coherent file version
under the mount operation lock. Its generation does not survive remount or
detect independent changes to mounted media. Conditional replacement and
exclusive creation with contents support 0–2 MiB files within the admitted
layouts. Replacement accepts validated primary and stream entries in the same
or different 512-byte directory sectors, including cluster boundaries.
Filename entries may occupy later sectors and remain unchanged.

Conditional replacement retains complete original and proposed primary/stream
sector images before mutation and refreshes them after staging. New contents
and allocation metadata are flushed before publication. A split set publishes
its stream sector first, then its primary/checksum sector, with one attempt per
sector. A further flush must succeed before any old cluster is reclaimed.
Failed publication or flush retains both allocations and the images, exact
attempt/completion flags, and unresolved recovery pin. Confirmed publication
updates the mount-local generation and live size/mapping even if later cleanup
fails; the caller receives uncertainty and its save expectation is preserved.
Definite prepublication failures can cancel only fresh allocations; cancellation
writes must flush before the resolved pin is handed back. There is no automatic
retry or restoration of an ambiguous sector. A split checksum can be invalid
between the writes, so power loss may require external repair; runtime pins
provide neither crash atomicity nor post-reboot recovery.

The fixed transaction requests 9,080 bytes on x86-64. Fresh allocation ledgers
use four bytes per candidate. Old FAT-linked mappings use four bytes per
cluster; validated NoFatChain mappings retain a start/count range instead.
At the current independent 2 MiB old/new mapping bounds and 512-byte clusters,
the maximum replacement metadata is 41,848 requested bytes per pin, or
1,339,136 bytes across 32 mounts. A small split 700-to-513-byte replacement
measured 9,088 bytes, four allocations (including temporary preflight memory),
33 sector reads, eight writes, and three flushes. A fragmented 2 MiB old/new
fixture measured 41,848 bytes, five allocations, 33,083 reads, 16,450 writes,
and three flushes. These are controlled host driver requests, excluding
allocator overhead, VFS objects, and kernel-owned syscall input buffers.
The temporary namespace proof is freed before transaction allocation; its
calculated maximum heap peak is 1,196,032 requested bytes. These bounds are
temporary implementation limits, not exFAT format restrictions.

At the smallest supported cluster size, a complete maximum-size replacement
has a conservative upper bound of 20,483 sector-write attempts: two dirty-flag
copies, 4,096 fresh bitmap updates, 4,096 data sectors, at most 8,191 FAT
allocation/link updates, two publication sectors, and 4,096 old bitmap clears.
Successful replacement uses three flushes. Reads and CPU work also include
the namespace proof and allocation search, bounded by the current 8,388,608
volume clusters, 262,144 ownership claims, 65,536 directory slots, and 4,096
directories. Fragmented bitmap mapping can add repeated FAT-prefix reads.
The small and maximum fragmented host fixtures took approximately 0.05 ms
and 10–13 ms of CPU time with ASan/UBSan; these do not establish a guest,
hardware, or universal wall-clock bound. The operation lock spans this work.

Ordinary file writes and zero-length truncation use a recovery pin acquired
before persistent mutation. They validate namespace allocation ownership and
the current entry set under the mount operation lock. Growth records candidate
clusters before bitmap updates, initializes them, constructs any required FAT
chain, and flushes staged data before updating the entry set. A split primary
and stream entry is updated through a prepared stream-sector write followed by
the prepared primary/checksum-sector write. Confirmed truncation publishes the
empty mapping before reclaiming old clusters. Failed or ambiguous writes,
cleanup, or flushes retain the transaction, revoke version confidence, and
return an uncertain result without a confirmed byte count. A resolved pin is
finished after the operation lock is released. These ordinary operations use
the ownership scan's current 262,144-cluster bound, independent of the 2 MiB
conditional-save limit. A FAT-linked old inventory plus fresh candidates can
request at most 1,048,576 ledger bytes at that bound; with the fixed transaction,
the conservative retained upper bound is 1,057,656 bytes per pin, or 33,844,992
bytes for 32 pins across mounts, excluding allocator and VFS-object overhead.
Old NoFatChain allocations use the compact range, so truncating them requires
no proportional old-cluster array. Small in-place writes retain only the fixed
9,080-byte transaction structure. The ownership scan remains necessary before
in-place mutation to detect shared allocations; a released scan is not a
permanent certificate. Repeated namespace scans, bitmap-chain mapping, and
FAT-linked per-sector mapping from the chain's start remain scaling costs;
the latter can require quadratic chain traversal for long sequential writes.
In-place data writes may have partially changed existing sectors when an operation becomes
uncertain; the mount is then gated. The split entry-set update is not crash
atomic: power loss between its sector writes can leave an invalid checksum
that requires external repair. Power loss before publication can also leave
allocated but unreachable clusters or a FAT chain longer than its recorded
DataLength. Runtime pins do not repair these states after reboot.

Unlink and empty-directory removal use the same pinned transaction and accept
validated file entry sets spanning sectors and fragmented directory clusters.
The supported entry-set bound (20 entries of 32 bytes) requires at most three
512-byte sector image pairs. Admission proves namespace-wide unique allocation
ownership and validates current identity, mapping, read-only attributes, and
an empty directory before any persistent mutation. Open nodes become tombstones
after confirmed primary-sector retirement; retained handles and directory
cursors cannot read freed storage or adopt a later occupant of the same slots.
Unopened files can be removed without allocating a permanent VFS node.

Retirement clears only each entry's InUse bit. It writes and flushes the primary
sector first, then writes each remaining secondary sector once and flushes the
complete namespace retirement. Only then are allocation bitmap bits cleared.
The final cleanup flush and resolved VFS pin handoff must complete before
success is reported. Failure retains original/proposed sector images, exact
write flags, the original allocation mode/start/count, cleanup current/next
clusters, and the pending bitmap image. The attached transaction gates reads,
namespace reuse, and all further mutation. Unresolved pins remain discoverable
across removal and mount detachment. There is no automatic retry or repair.
All unused entry types (01h–7Fh) are eligible for slot reuse; live orphan
secondaries outside a validated set cause traversal/ownership failure.

Cleanup deliberately leaves FAT links unchanged. The bitmap defines allocation
ownership; stale FAT values in free clusters are permitted. The protected cursor
can therefore follow the original on-disk chain in linear time, with no retained
cluster array proportional to file size. It depends on that unchanged original
FAT and mount gate, not a reusable ownership certificate. Deletion has no 2 MiB
content limit, but the complete proof still bounds the volume to 8,388,608
clusters, total ownership claims to 262,144 clusters, directory allocations to
65,536 slots in aggregate, and pending directories to 4,096. Larger otherwise
valid layouts return an incomplete-proof error before mutation. In particular,
large mostly empty directory allocations can exceed the slot budget and cannot
currently be removed. These temporary proof limits require separate scalable
namespace engineering; the safe-save content bound is independent.

The fixed transaction is 9,080 requested bytes on x86-64. Retirement adds a
24-byte header and 1,040 bytes per affected sector: at most 12,224 bytes per
retained pin, or 391,168 bytes for 32 pins across mounts. A three-sector file
retirement used four allocations including temporary proof storage, 12,224
peak/retained transaction bytes, eight writes and three flushes; small fixtures
used 66–69 reads. A 4,097-cluster imported file (larger than 2 MiB) used 11,184
transaction bytes, 4,101 writes and three flushes, with 10,369 FAT-linked or
4,154 NoFatChain reads, and about 2–4 ms host CPU time with ASan/UBSan. Reader
cursors, VFS/pin objects and allocator overhead are excluded. The temporary
proof's maximum requested heap remains 1,196,032 bytes and is freed before
transaction allocation. At the claim limit, successful retirement requires at
most 262,149 write attempts (two boot, three directory, 262,144 bitmap) and three
flushes; namespace scans and fragmented bitmap mapping dominate read costs.

Power loss after primary retirement can leave allocated but unreachable data
and live orphan secondaries. Interrupted cleanup can leave an allocated suffix
of an unreachable chain. A torn directory or bitmap sector can damage unrelated
entries or allocation bits. The in-memory images contain runtime evidence but
provide no post-reboot recovery or crash atomicity; independent checking/repair
may be necessary. Successful disposable-image checks do not prove repair of
all interrupted states. Rename still uses its separate ordinary metadata path
and has no equivalent pinned retirement contract.

Exclusive creation requires its complete entry set to fit in one 512-byte
directory sector. If a directory has no suitable existing run, creation can
relocate a zero-filled end marker into the next sector and append one initialized
cluster when necessary. A contiguous `NoFatChain` directory extends in place
when its next cluster is free. Otherwise Mangrove builds a complete FAT chain
for the existing contiguous extent and the new cluster before changing the
parent stream to FAT-linked mode. The current complete-namespace scan limits an
admitted directory to 65,536 entries; conversion retains at most 33 old-extent
FAT sector image pairs at 512-byte clusters. This is an implementation limit.
On x86-64, those pairs request at most 34,320 bytes; the transaction and a
maximum-size fresh-file ledger can retain up to 68,704 requested bytes per
unresolved pin, or 2,198,528 bytes across 32 pins. These figures exclude heap
allocator overhead, VFS objects, and the syscall's input buffers.
The new entry sector is staged and flushed before linkage or conversion. For a
non-root directory, the checksummed parent stream is then updated so its
ValidDataLength and DataLength cover the complete allocation. Its primary and
stream entries may occupy different sectors; the stream sector is written
first, followed by the primary checksum sector. The old end marker is retired
last and makes the new file visible. Growth retains the candidate allocation,
FAT, directory, and parent-entry sector images, write progress, and a recovery
pin across uncertain outcomes. This path requires a following end-marker slot
in the entry sector and rejects nonzero bytes after the old end marker.
New file entry sets spanning directory sectors remain unsupported. Recovery pins retain
uncertain transaction state, and version-confidence loss prevents unsafe
follow-up saves. These mechanisms do not establish crash atomicity or power-loss
durability. An interruption between a FAT tail link and the parent length
update can leave an extra linked cluster. An interrupted `NoFatChain` conversion
can leave dormant FAT links or a parent stream whose mode and chain disagree;
an interruption between separate parent stream and checksum writes can leave
an invalid entry set. Such states may require external repair.

`volumed` recognizes exFAT as mountable removable media and applies the same
label-derived `/vol` policy, suppression, unmount, and eject behavior used for
FAT32 and MGFS.

## Formatting

`diskutil` accepts `format <partN|disk> exfat` inside an authorized storage
management session. The existing target-generation, mount, system-volume,
read-only, bounds, confirmation, flush, re-probe, and verification checks
apply unchanged. The formatter creates standard main and backup boot regions,
FAT, cluster heap, allocation bitmap, recommended up-case table, root
directory, and required system entries from the selected target geometry.

The embedded recommended up-case table and its reproducible generation path
are documented in [upcase-provenance.md](upcase-provenance.md).
