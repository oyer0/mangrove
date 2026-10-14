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
also detects FAT cycles in constant space and accepts either a reachable end
marker or confirmed exhaustion of a completely full chain within 65,536
directory entries; larger roots remain unsupported.

Only one-FAT/one-bitmap volumes are admitted. Bitmap identifier zero belongs to
FAT zero; reserved BitmapFlags bits are not file-stream flags. NumberOfFats=2
is reserved by the specification for TexFAT, whose active and inactive
FAT/bitmap pairs are not ordinary mirrored copies. Mangrove does not implement
TexFAT transactions and refuses both active selections before mounting or
mutating. Backup VolumeFlags are stale; selection and admission use the main
boot region.

Volumes arriving with VolumeDirty, MediaFailure, or ClearToZero set are also
refused. Mangrove does not perform the required recovery or flag-transition
protocol for these incoming states. An admitted writable mount starts without changing media. Each mutation
retains and flushes dirty-state images before changing filesystem contents,
and clears dirty state only after confirmed transaction completion. No crash
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

Data cluster numbers occupy the half-open interval `[2, ClusterCount + 2)`.
A contiguous extent starting at `FirstCluster` can therefore contain up to
`ClusterCount + 2 - FirstCluster` clusters, including the final data cluster.
Count-based extent validation is shared by file facts, directory capacity,
ownership checks, allocation inventories and bitmap ranges. First-cluster
validation precedes widened subtraction; empty, overflowing and out-of-range
extents are rejected. Zero-allocation streams use their separate validated
representation. Zero-based mapping indices retain the corresponding inclusive
maximum `ClusterCount + 1 - FirstCluster`.

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

The DIRECTORY attribute determines object type; READ_ONLY removes write access.
SYSTEM, HIDDEN and ARCHIVE are preserved ordinary-file metadata. They do not
suppress pathname lookup, indexed/sequential enumeration or access to directory
descendants. SYSTEM does not confer privileges or bypass permissions, read-only
mount policy, ownership validation or recovery gates. Optional presentation
filtering belongs in a separate user-interface policy.

Classification follows the directory entry type: validated File entry sets are
namespace objects, including SYSTEM directories. Allocation Bitmap, Up-case
Table and Volume Label primary entries remain filesystem metadata and are not
ordinary namespace objects. Attribute flags do not change that classification;
malformed sets and orphan secondaries still fail validation. This distinction
follows the [exFAT specification](https://learn.microsoft.com/en-us/windows/win32/fileio/exfat-specification#744-fileattributes-field).
Mangrove does not invent POSIX permissions, symbolic links or special files for
exFAT. Imported-name and native enumeration length limits apply equally to all
ordinary entries.

The native exFAT volume label is read by the probe layer and can be changed on
an unmounted volume through `diskutil`. Label input must be valid UTF-8,
convert to valid UTF-16, fit the exFAT label limit, and satisfy exFAT's native
character restrictions. Formatting creates an unlabeled filesystem.

## Write and lifecycle behavior

A writable mount does not write during admission. Pinned mutations retain both
boot copies' original/proposed dirty and clean images. Dirty state is flushed
before data or metadata changes; confirmed completion flushes all required
metadata before clearing dirty state. Sync only flushes and cannot clear an
unresolved transaction. Read-only mounts reject mutations regardless of caller
credentials or backing-media writability.

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
request at most 1,048,576 ledger bytes at that bound. Completion metadata and
ancestor timestamps are accounted for in [resource bounds](#metadata-and-resource-bounds).
Old NoFatChain allocations use the compact range, so truncating them requires
no proportional old-cluster array. Small in-place writes use the fixed transaction plus actual completion and
ancestor records; they do not allocate maximum-sized ledgers. The ownership scan remains necessary before
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

Power loss after primary retirement can leave allocated but unreachable data
and live orphan secondaries. Interrupted cleanup can leave an allocated suffix
of an unreachable chain. A torn directory or bitmap sector can damage unrelated
entries or allocation bits. The in-memory images contain runtime evidence but
provide no post-reboot recovery or crash atomicity; independent checking/repair
may be necessary. Successful disposable-image checks do not prove repair of
all interrupted states.

## Timestamps and allocation percentage

Timestamp-requiring mutations use the validated UTC
[system clock](../../kernel/timekeeping.md). RTC-as-UTC trust must be explicitly
established by deployment; default builds have unavailable realtime. Required
mutations return `MG_ERR_TIME_UNAVAILABLE` before media changes if UTC is
unavailable or outside 1980–2107. The CMOS-backed source has whole-second
resolution. Creation and modification fields encode odd seconds using the
specified 10-ms increment, without fabricated fractions; access fields have
only two-second granularity. UTC offset zero is explicitly valid.

Creation initializes creation, modification and access times. Content and
length mutations update modification/access times, including in-place writes.
Ordinary reads preserve access times. Existing creation fields are preserved;
rename preserves the moved object's timestamps. Directory content changes
update affected directories and their ancestors, because parent entry-set
changes modify those directories' contents. The root has no file entry set.
One captured sample supplies the operation. Complete ownership preflight
collects ancestor locations; shared sectors are reread from confirmed
predecessors before changing a primary entry and checksum.

The validated allocation bitmap is counted once per filesystem state, including
allocated but unreachable clusters and excluding padding bits. Confirmed bitmap
changes maintain checked allocation accounting. Before allocation or reclamation,
the main boot sector's PercentInUse becomes `0xFF` and is flushed. After confirmed
allocation, cleanup and required timestamp publication, it receives
`floor(allocated_clusters * 100 / ClusterCount)` and is flushed. Cancellation
restores accurate accounting only after confirmed cleanup. A trusted incoming
known value is preserved until a change; a stale or unknown value can be
corrected on confirmed completion. The backup percentage may remain stale as
permitted by the format. PercentInUse and VolumeFlags are boot-checksum exclusions.

An uncertain bitmap result invalidates accounting and retains unknown state.
An ambiguous percentage-sector write or flush retains its original/proposed
images without retry; torn writes can make even that field indeterminate.
Resolved-pin finishing includes ancestor timestamps, final percentage and clean
boot metadata. Failure returns uncertainty, retains the pin and gates mutation;
it cannot report ordinary success. Runtime evidence does not survive reboot.
Power loss can leave a split checksum, stale timestamps, orphan allocation or
indeterminate boot metadata requiring independent repair. Ordered flushes do
not establish crash atomicity or power-loss durability.

## Metadata and resource bounds

On x86-64 the fixed transaction requests 9,120 bytes. Clean boot images add
2,060 bytes; allocation-percentage images add 2,064 bytes when needed.
Fresh and old FAT-linked ledgers use four bytes per cluster; NoFatChain old
mappings use a compact range. Namespace sector inventories add a 24-byte header
and 1,040 bytes per affected sector. Growth, conversion and extra-cluster records
are allocated only for layouts requiring them.

Ancestor completion adds a 16-byte header and 1,080 bytes per affected directory,
up to 4,095 non-root ancestors under the current proof limit: 4,422,616 bytes.
This is preallocated during the existing ownership scan. The complete proof's
maximum requested peak, including the fixed transaction, 1 MiB claim bitmap,
4,096 40-byte directory jobs and ancestor inventory, is 5,644,152 bytes.
It is freed before mutation except for retained ancestor images. Queue resizing
has a smaller transient peak. Cached-node association can cost the product of
ancestor and live-node counts; long fragmented stream mapping and repeated
namespace scans remain independent scalability costs.

Conservative retained bounds, excluding allocator overhead, stack, filesystem
state, VFS/pin objects and syscall input buffers, are:

| Operation | Per pin | 32 pins across mounts |
| --- | ---: | ---: |
| Conditional replacement, maximum old/new ledgers and ancestry | 4,468,628 bytes | 142,996,096 bytes |
| Ordinary mutation, maximum ledger and ancestry | 5,484,436 bytes | 175,501,952 bytes |
| Creation, maximum child/growth/conversion and ancestry | 4,500,188 bytes | 144,006,016 bytes |
| Rename, maximum growth/conversion and ancestry | 4,486,108 bytes | 143,555,456 bytes |
| Retirement, maximum namespace images and ancestry | 4,439,004 bytes | 142,048,128 bytes |

These sums combine independent bounds and need not be jointly attainable on one
layout. Filesystem upcase storage is separately bounded at 1 MiB per state.
Small root operations have no ancestor allocation. A deep 4,095-directory fixture
measured 4,596,898 peak bytes and 4,436,772 retained transaction/return-state bytes,
8,210 sector reads, 4,101 writes and six flushes, approximately 17–21 ms host CPU.
Each ancestor adds at most three reads, one primary-sector write and one shared
completion flush. Percentage handling adds at most two writes/two flushes;
clean completion adds two writes/one flush. Counting bitmap bits is O(ClusterCount)
once per state, with bounded sector caches rather than volume-sized storage.
These are requested-memory and host-driver measurements, not hardware latency,
call-stack, crash-recovery or durability bounds. The existing proof limits need
scalable replacement before broader large-filesystem support.

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

Ordinary empty-file creation needs no child ledger or data buffer. Mkdir zeros
sector-sized staging buffers rather than allocating an entire cluster buffer.
Selecting a run validates the allocated parent tail in one streaming pass,
without a permanent cache or per-entry chain restarts. Current measurements and
test boundaries are in [development tests](../../development/tests.md#ordinary-exfat-creation).

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

Its exported kernel entry requires an explicit expected registration ID
through shared [formatting admission](../../storage/lifecycle.md#destructive-administration).
Raw formatting is private. Direct calls cannot bypass read-only, overlapping
mount/private-owner or unresolved-recovery exclusion. Failed destructive writes
retain the formatting owner and pending image; no rollback or crash recovery is
implied. The userspace formatting request ABI is unchanged.

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
