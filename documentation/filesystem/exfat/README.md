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

Imported filename validity is independent of the native component limit.
The standard entry set permits 1–255 UTF-16 code units, including paired
surrogates. The parser keeps all units for hash and up-case comparison and a
complete UTF-8 spelling of up to 765 bytes plus NUL. This capacity is derived
from the format: a BMP unit needs at most three bytes; a surrogate pair needs
four bytes for two units. Retained identities also keep the complete spelling,
including when a shorter case-equivalent native query opens that file.
No truncation, replacement characters, normalization, or host locale is used.

Malformed UTF-16, zero NameLength, forbidden characters, invalid checksums or
name hashes remain errors. The filename character restrictions reject
U+0000–U+001F and `"*/:<>?\|`; U+007F–U+009F are not format-forbidden.
These rules follow the [exFAT specification](https://learn.microsoft.com/en-us/windows/win32/fileio/exfat-specification),
sections 7.6.3 and 7.7.3.

Native pathname components and returned directory names still permit at most
255 UTF-8 bytes. An otherwise valid imported name exceeding that field returns
`MG_ERR_NAME_TOO_LONG`, leaves output unchanged, and consumes exactly one
entry. Subsequent enumeration continues; lookup and allocation ownership scans
include the complete entry and its directory descendants. Capacity errors are
not EOF or corruption. See the
[checked enumeration contract](../../architecture/processes-and-syscalls.md#checked-directory-enumeration)
for batch deferral, listing warnings, and the required future length-aware
interfaces. A name exceeding the pathname limit cannot be addressed by its
stored spelling through the current ABI; a representable up-case-equivalent
query can still match it.

On x86-64 the private parsed entry is 1,320 bytes and a retained identity is
840 bytes, with both name buffers bounded by the on-disk format.
Parsing allocates no heap memory. Encoding is bounded by 255 units and is
performed once after checksum/hash validation. Volume up-case lookup and
namespace traversal costs retain their existing bounds and bottlenecks.
Production compiler stack-usage measurements show a largest driver frame of
5,056 bytes for rename (including three local full-name objects); this is a
frame measurement, not a maximum call-chain or guest stack measurement.

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

The fixed transaction requests 9,096 bytes on x86-64. Fresh allocation ledgers
use four bytes per candidate. Old FAT-linked mappings use four bytes per
cluster; validated NoFatChain mappings retain a start/count range instead.
At the current independent 2 MiB old/new mapping bounds and 512-byte clusters,
the maximum replacement metadata is 41,864 requested bytes per pin, or
1,339,648 bytes across 32 mounts. A small split 700-to-513-byte replacement
measured 9,104 bytes, four allocations (including temporary preflight memory),
33 sector reads, eight writes, and three flushes. A fragmented 2 MiB old/new
fixture measured 41,864 bytes, five allocations, 33,083 reads, 16,450 writes,
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
the conservative retained upper bound is 1,057,672 bytes per pin, or 33,845,504
bytes for 32 pins across mounts, excluding allocator and VFS-object overhead.
Old NoFatChain allocations use the compact range, so truncating them requires
no proportional old-cluster array. Small in-place writes retain only the fixed
9,096-byte transaction structure. The ownership scan remains necessary before
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

The fixed transaction is 9,096 requested bytes on x86-64. Retirement adds a
24-byte header and 1,040 bytes per affected sector: at most 12,240 bytes per
retained pin, or 391,680 bytes for 32 pins across mounts. A three-sector file
retirement used four allocations including temporary proof storage, 12,240
peak/retained transaction bytes, eight writes and three flushes; small fixtures
used 72–77 reads. A 4,097-cluster imported file (larger than 2 MiB) used 11,200
transaction bytes, 4,101 writes and three flushes, with 10,375 FAT-linked or
4,160 NoFatChain reads, and about 2–4 ms host CPU time with ASan/UBSan. Reader
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
all interrupted states.

## Rename

Rename uses the same pinned transaction and mount operation lock as other
protected mutations. It validates current parents/source, collisions (including
system-attribute files and duplicate case-equivalent imported names), complete
namespace ownership, generation capacity and
device identity before mutation. The ownership traversal also rejects moving a
directory into its own subtree, without a separate ancestry scan. Rename neither
restages nor reclaims the file allocation and has no independent 2 MiB size cap.

When the new name fits the old slot inventory in the same directory, secondary
sectors are written and flushed before the primary/checksum sector. Otherwise,
destination secondaries are written and flushed, then its primary is published
and flushed. Only then is the source primary retired and flushed, followed by
its remaining secondaries and another flush. Each sector gets one attempt per
phase. Shared-sector images are refreshed from confirmed predecessors after
destination/growth changes, preserving unrelated entries. Timestamps, mapping,
lengths and reserved fields survive rename. Confirmed completion updates the
retained node's location/name and advances file and parent generations; prior
save expectations become stale. Inodes, handles and offsets remain unchanged.
Directory cursors retain their byte offsets and use current mapping, with the
existing skip/repeat behavior during namespace changes.

Destination growth reuses bitmap/FAT initialization, parent-stream updates,
NoFatChain conversion and end-marker relocation. It first exposes initialized
empty capacity, then publishes the renamed entry set. A maximum 255-byte name
may need two new 512-byte clusters; the second allocation's images are allocated
only when needed. Sets may span sectors and fragmented clusters. Non-END entry
types after an end marker remain conservatively unsupported; END entries may contain nonzero
reserved bytes.

Inventories and pin capacity are allocated before mutation. Failed writes,
flushes, removal or reads after mutation retain the transaction and revoke
version confidence. No ambiguous write is retried/restored; possibly linked
directory clusters and file contents are never freed. Success requires complete
destination publication, source retirement and post-unlock pin finishing.
Failed handoff returns uncertainty. A boot-image read failure before any write
can return definite failure with a resolved pin.
The VFS passes its retained superblock explicitly to the rename hook, so forced
detach cannot redirect an admitted operation through a retargeted node pointer.

In-place rename requests 10,160–12,240 transaction bytes on x86-64. Relocation
without growth retains at most 15,384 bytes. Growth/conversion retains at most
59,344 bytes per pin, or 1,899,008 across 32 mounts: up to three source sectors,
two sector-aligned destination sectors, growth records and 33 conversion-sector
pairs. A non-growth destination can need three sectors. Temporary proof storage
(up to 1,196,032 bytes) is freed before transaction allocation. These figures
exclude allocator overhead, stack and VFS/pin objects; smaller operations allocate
their actual image counts and optional growth records.

Controlled sanitizer fixtures measured 35–75 reads, 5–17 writes, 2–9 flushes and
roughly 0.15–0.7 ms host CPU time for representative layouts. A full 4,091-cluster
directory requiring conversion and two appended clusters measured 59,344
peak/retained bytes, eight allocations, 16,477 reads, 49 writes, eight flushes and
about 100 ms. These are host driver measurements, not hardware bounds. Under the
current proof limits, a conservative maximum is 2,098 sector-write attempts and
nine flushes, including initialization of at most 2,048 added directory sectors.
Namespace proof and allocation searches remain scaling costs. The aggregate
65,536-slot budget counts allocated capacity, including unused tails, and can
refuse otherwise valid large, mostly empty directories before rename. Replacing
that temporary budget requires scalable namespace-proof engineering.

Power loss during in-place rename may leave an invalid checksum. Interrupted
moves may leave duplicate references to one allocation or orphan secondaries.
Interrupted growth may leave unreachable clusters, mapping/length disagreement,
or a split parent checksum. Torn writes can damage neighboring entries. Runtime
pins and quarantine provide no post-reboot repair, atomic cross-directory rename
or durability guarantee; independent checking/repair may be needed. Successful
disposable-image checks establish final-state interoperability only.

## Ordinary file and directory creation

Ordinary create and mkdir use the same ownership-validated, pinned publication
engine as [exclusive creation](#exclusive-creation), including entry sets spanning
sectors/clusters, end-marker relocation, FAT-linked growth, and NoFatChain
extension/conversion. Mounted exFAT exposes pinned creation hooks only;
legacy callbacks cannot bypass their recovery ownership. All admission and publication run under operation_lock;
VFS preallocates the return node and retains the mount through resolved-pin
finishing outside the lock. No allocation is needed after confirmed publication.

Files start with zero length and first cluster zero, and can subsequently grow
through ordinary writes. Mkdir stages one completely zeroed cluster before
publishing a DIRECTORY entry set with NoFatChain allocation and DataLength equal
to ValidDataLength equal to the cluster size. exFAT directories have no dot
entries. Child allocation is separate from parent-growth candidates. Admission
accounts for both added parent capacity and the child's directory capacity in
current proof budgets, so creation cannot knowingly exceed the next ownership
scan's limits.

Ordinary callbacks apply current-credential write access and default file or
directory security metadata. Trusted owned-create/mkdir callbacks use the same
transaction but accept the authorized kernel caller's explicit owner/permissions;
read-only, identity, collision, ownership, and liveness checks still apply.
Security metadata remains mount-local, as in existing exFAT nodes. Native ABI
signatures and the distinct ALREADY_EXISTS, NO_SPACE, UNSUPPORTED, and UNCERTAIN
results are unchanged. Uncertain mutation returns no successful node and retains
all recovery state; it is not retried automatically.

On x86-64, mkdir's conservative maximum retained transaction request is 57,036
bytes (1,825,152 bytes for 32 pins across mounts), excluding allocator overhead,
VFS nodes, stack, and the separately freed temporary namespace proof. It replaces the
2 MiB child's 16,384-byte ledger with one four-byte allocation candidate.
The fixed transaction and growth/publication inventories are shared unchanged.
Ordinary empty-file creation needs no child ledger or data buffer; mkdir zeros
sector-sized staging buffers rather than allocating an entire cluster buffer.
Selecting a run validates the allocated parent tail in one streaming pass, with
no permanent cache or additional heap. This adds directory-sector reads when a
free run appears early; it does not restart the chain for each entry.
Measurements and test boundaries are in [development tests](../../development/tests.md#ordinary-exfat-creation).

These operations provide runtime containment, not crash atomicity or persistent
recovery. Power loss during growth, split parent checksums, secondary staging, or
primary publication can leave unreachable allocation, orphan secondaries, or an
invalid entry-set checksum requiring independent repair. In-memory retained pins
do not survive reboot, and flush completion alone does not make publication atomic.

## Zero-allocation directories

Imported non-root directories may have DataLength, ValidDataLength and
FirstCluster all zero, with NoFatChain clear. This is an empty directory with
no owned clusters, not an invalid FAT chain. The [exFAT specification](https://learn.microsoft.com/en-us/windows/win32/fileio/exfat-specification)
permits zero directory allocation and requires nonzero length for NoFatChain.
Nonzero FirstCluster with zero directory length, unequal directory lengths,
and zero-length NoFatChain mappings are rejected. The root still requires its
allocated FAT chain.

Lookup and checked enumeration report an empty namespace without storage I/O;
ownership validation claims no clusters for it. Empty-directory removal retires
its checksummed parent entry set without allocation cleanup. Identity remains
bound to that parent entry location, so distinct empty directories do not share
identity merely because their FirstCluster is zero. Rename ancestry checks use
that identity and do not confuse cluster zero with another directory.

First-child file creation, mkdir and moves into such a directory reuse the
existing pinned growth transaction. It reserves one or two fresh clusters,
records bitmap/FAT changes, initializes all capacity, and stages complete child
contents before publishing the parent's FirstCluster and equal stream lengths.
The first allocation uses FAT-linked mode; later growth follows the ordinary
FAT-linked protocol. There is no old tail link or end marker to relocate.
Same-sector child sets are staged before parent publication. Spanning sets
publish initialized empty capacity first, then use secondary-before-primary
publication. Confirmed completion updates the live directory mapping before
binding child identities. Enumeration cursors resolve that current mapping;
an already exhausted cursor remains at EOF and must be reopened.

All inventories and the recovery pin are prepared before mutation. Cancellation
before parent publication clears confirmed fresh bitmap allocations and flushes
cleanup. Once parent publication may have happened, directory and child storage
remain accounted for by retained recovery state. Failed or torn writes, failed
flushes, removal and uncertain cleanup return uncertainty and revoke mutation
confidence, without a successful child node. Split parent entries retain both
sector images and exact attempted-write progress.

With 512-byte clusters, maximum native names and a 2 MiB child, the measured
requested transaction peak and retained payload are 39,096 bytes, or 1,251,072
bytes for 32 pins across mounts. This excludes allocator overhead, VFS nodes,
stack, input buffers and the separately freed namespace proof. Contiguous and
fragmented child allocation measured 4,133/16,567 reads, 8,204/12,364 writes,
five flushes and approximately 3/8 ms host CPU time. Small first-child operations
requested 18,608–22,788 peak bytes including the VFS node. These are controlled
host measurements, not hardware latency bounds. Existing namespace-proof and
safe-content limits remain unchanged.

Runtime pins do not survive reboot. Interrupted first allocation can leave
unreachable allocation, an invalid split parent checksum, or orphan child
secondaries; torn sectors can damage neighboring entries. Ordered writes and
flushes provide no crash atomicity or power-loss durability. Independent image
checks verify completed final states, not repair of interrupted states.

## Exclusive creation

Exclusive creation accepts 0–2 MiB of complete contents and VFS names of at
most 255 UTF-8 bytes. The primary, stream and filename entries may span sectors
and fragmented directory clusters. UTF-16 encoding, secondary counts, upcase
collisions and the complete entry-set checksum are validated. Namespace proof
rejects malformed sets and live orphan secondaries before slot selection; an
existing equivalent name returns ALREADY_EXISTS without mutation.

Before mutation, VFS preallocates the return objects and the driver allocates
identity state, fresh-allocation ledgers, directory-growth records, every
publication image and the recovery pin. One-sector sets reuse the fixed images;
spanning sets add a 24-byte header and 1,040 bytes per affected sector (at most
three). The operation lock covers admission, staging and publication. The
complete source is staged and flushed before namespace publication. For a
spanning set, secondary sectors are written and flushed before the prepared
primary/checksum sector is written and flushed. Each sector is attempted once
per phase. Only confirmed publication binds the prepared node's identity,
initial generation and mapping; VFS finishes the resolved pin after unlocking
before returning a usable handle.

A free run can cross sectors in existing capacity. If no suitable run exists,
growth initializes and flushes capacity, links or converts the allocation,
updates the non-root checksummed parent stream and relocates the old end marker.
Spanning creation first exposes empty initialized capacity, then publishes the
set. One-sector growth retains its staged-set/marker-last protocol. A maximum
name can require two appended 512-byte clusters. FAT-linked roots and non-root
directories, contiguous NoFatChain extension and NoFatChain-to-FAT conversion
use the shared growth machinery. Sector images are refreshed after confirmed
growth updates, preserving neighboring records and shared FAT/bitmap changes.
Slot selection validates the complete allocated tail, even after finding a
usable run. Non-END entry types after an end marker remain conservatively
unsupported: creation never silently repairs or exposes hidden tail records.
Nonzero reserved bytes in END entries are accepted.

Cancellation before linkage/publication clears each confirmed fresh bitmap
allocation, including both appended directory candidates, and flushes cleanup.
After linkage or namespace publication may have been attempted, possibly
reachable allocations are never cancelled. Failed or ambiguous writes, flushes,
reads after publication, removal or cleanup retain images, write progress and
allocation ownership, revoke version confidence and return uncertainty with no
usable new handle. Ambiguous writes are not retried or restored.

On x86-64 the measured maximum requested transaction payload is 73,424 bytes:
a 9,096-byte fixed transaction, 840-byte prepared identity, 16,384-byte child
ledger, 8,608-byte growth record, 2,072-byte second-candidate record, two
publication sector pairs plus their 24-byte header, and 33 conversion-sector
pairs (34,320 bytes). This maximum combines a 2 MiB child and a full fragmented
layout requiring conversion; 32 retained pins can request 2,349,568 bytes.
A non-growth set can use three images but needs no growth/conversion inventory.
Allocator overhead, VFS objects, stack and syscall input buffers are excluded.
The temporary namespace proof (up to 1,196,032 requested bytes) is freed before
transaction allocation. Smaller files allocate proportional ledgers/images.

Controlled sanitizer fixtures measured 700-byte spanning creations at
12,040–23,760 requested transaction bytes, 27–55 reads, 7–19 writes and 3–8
flushes, approximately 0.1–0.3 ms host CPU time. A 2 MiB child in a full
4,091-cluster NoFatChain directory measured 73,424 peak/retained bytes, 24,750
reads, 8,238 writes and seven flushes, approximately 140 ms. These are host
measurements, not guest/hardware bounds. A conservative upper bound is 18,477
sector-write attempts and eight flushes, combining worst-case child staging
with bounded directory initialization/conversion. Cancellation adds at most
4,098 bitmap clears and one cleanup flush to a failed staging operation.

The current complete proof still caps the volume at 8,388,608 clusters,
ownership at 262,144 claimed clusters, allocated directory capacity at 65,536
slots in aggregate and queued directories at 4,096. Creation checks that added
file/directory allocation and directory capacity remain within these bounds.
These temporary limits can reject valid layouts; replacing them and the
2 MiB safe-content bound requires dedicated scalability work. Conversion
retains at most 33 old-extent FAT sector pairs under the current proof budget.

Ordered writes are runtime containment, not crash atomicity or durability.
Power loss before primary publication can leave allocated unreachable contents
and orphan live secondaries. Growth interruption can leave mapping/length
disagreement, dormant conversion links or an invalid split parent checksum.
Torn sectors can damage neighboring entries. In-memory pins provide no repair
after reboot; independent checking/repair may be required. Successful
interoperability checks do not establish repair of interrupted states.

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

## Unmounted volume-label transactions

Every public exFAT label call acquires the storage-management quiescence guard,
then reserves an exclusive private VFS recovery owner before
loading the filesystem state. The owner is not a namespace mount. Its saved
registration identity and physical range exclude overlapping mounts, formatting,
partition management and other private owners. Detached unresolved pins retain
that exclusion and the driver state; a new registration cannot receive its I/O.
The caller and registry own separate references. Resolved callbacks run after
the operation lock is released, and failure retains the owner and transaction.

The operation validates the complete allocation ownership and root namespace,
then uses the safe-create slot and root-growth machinery. Fresh directory
storage is initialized before linking; a possibly completed tail link makes it
directory-owned and forbids cancellation reclamation. Earlier END markers are
relocated using retained images. An exactly full, validated directory needs no
END slot: growth appends initialized capacity without rewriting a marker.
Walkers distinguish validated mapping exhaustion from I/O failure.

Original/proposed label, growth, dirty-flag and final clean-flag sector images
are retained before attempted writes. Namespace and allocation completion are
flushed before restoring the initially clean volume. Failed/torn writes, failed
flushes, device loss or failed handoff return uncertainty, retain the registry
pin, and prevent automatic retry. Definite preflight failures leave media
unchanged and dispose the private owner. Boot checksum computation excludes
VolumeFlags, and each boot copy retains its unrelated mutable flags. Ordered
writes and runtime pins provide no crash atomicity or post-reboot recovery;
power loss can require external allocation or namespace repair.

The host label fixture uses actual driver and private-registry functions,
independently formatted images, production readback and `fsck.exfat`.
Measurements and limits are documented in [development tests](../../development/tests.md#exfat-volume-label-recovery).
