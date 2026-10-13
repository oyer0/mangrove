# Block, filesystem, and volume lifecycle

Mangrove separates storage mechanism, generic device reporting, and removable
volume policy:

```text
kernel block, GPT, filesystem drivers, and VFS -> mechanism and authority
deviced                                      -> generic device snapshots
volumed                                      -> removable-volume policy
mount, unmount, eject, lsdsk, diskutil       -> clients
```

## Device and partition identity

Every registered block object receives a monotonic boot-local instance ID.
Registry slots, display names such as `disk2`, USB ports, and GPT partition
numbers are not lifecycle identity. A partition has its own instance and an
authoritative parent relationship. Removing and recreating `part1`, or
replacing media in the same port, produces fresh block identities.

The registry marks an instance offline before teardown and admits no new I/O.
Block reads, writes, and flushes resolve liveness for that exact ID and are
serialized with removal. Long-lived mounts and destructive-operation requests
retain or revalidate instance and parent identity, preventing stale work from
reaching replacement media.

VFS records the device ID before calling a filesystem's mount method and
rechecks that same ID before publishing the mount. A slot reused while mount
work is in progress cannot be published as the original device.

MGFS saves its registration ID at mount and supplies it for every filesystem
block read and write, including grouped file reads. The block layer checks the
expected ID again after acquiring its I/O gate, so a retired or reused registry
slot cannot redirect an old MGFS instance to replacement media. A callback
admitted before retirement may finish while removal begins; unregister cannot
reclaim backing until the gate drains. An operation that observes retirement
reports failure rather than a successful read or write.

Mounted FAT32 reads and writes use the registration ID captured by the
filesystem at mount entry.
Mounted exFAT reads, writes, dirty-flag updates, and sync flushes use the ID
captured while constructing its filesystem state. The identity-bound flush
uses the same I/O admission gate as reads and writes; a missing backend flush
callback retains the existing synchronous-write contract. FAT32 has no
filesystem sync callback. Probe, formatting, and label operations use separate
device-management paths, not a retained filesystem instance.

FAT32 and exFAT assign boot-local inode IDs to observed directory entries.
Repeated lookup returns the same live node, so ordinary writes and truncation
refresh its size for every retained handle. FAT32 keys the live entry by its
parent directory cluster and short-entry slot; exFAT updates its entry-set
location when a supported rename moves it. Deletion tombstones the old node,
and reuse of a directory slot receives a new ID. Mutations advance a
mount-local generation; a failed write with an uncertain media outcome revokes
version confidence for the mount. Coherent native snapshots may bind that
generation. FAT32 and exFAT support conditional replacement and exclusive
create-with-contents. These generations
neither survive remount nor detect independent changes to mounted media.

For snapshot-sized files, current-facts checks read their mapped chains,
reject loops and malformed links, and verify exFAT bitmap allocation. FAT32
checks referenced FAT entries for free marks. Duplicate first clusters among
entries observed during this mount also revoke version confidence. FAT32
walks complete directory chains for lookup and enumeration. Its private,
read-only namespace ownership preflight traverses the reachable tree from the
root, claims every file and directory data cluster in a temporary bitmap,
checks chain lengths and links, and rejects sharing, free references, invalid
links, cycles, malformed long-filename entries, and inconsistent mirrored FAT
copies. When FAT mirroring is disabled, only the selected active FAT is
authoritative. FATs, reserved sectors, and FSInfo lie outside the data-cluster
ownership bitmap; mount geometry separates those regions.

The preflight must run under the mount operation lock. Its result is valid only
until that lock is released; it is not a permanent ownership certificate and
cannot detect independent changes to mounted media. It performs no media
writes. FAT32 replacement and exclusive create-with-contents consume the result
under that same lock. A read or allocation failure remains retryable;
definite corruption revokes mount-local
version confidence. The scan caps the bitmap at 1 MiB (8,388,608 cluster
numbers), claimed clusters at 262,144, directories at 4,096, directory-chain
clusters at 4,096, scanned directory slots at 65,536, and FAT copies at four.
An incomplete scan returns an error and proves nothing. Requested heap peaks
at 1,146,880 bytes:
1 MiB bitmap, 32 KiB directory queue, and one 64 KiB directory buffer,
excluding allocator overhead and stack. A small two-cluster fixture measured
33,288 requested bytes above its existing nodes, three FAT-sector reads and
two directory-cluster reads. A disposable 128 MiB, 512-byte-cluster image
with a multi-cluster directory measured 65,540 requested bytes and 111 sector
reads. A worst-case complete scan may read roughly four FAT sectors per claimed
cluster plus directory data;
ordinary reads and lookups do not run this namespace-wide scan.

exFAT has a private, read-only namespace ownership preflight for a live file
or directory. It claims the allocation bitmap and upcase-table streams, the
complete root and nested directory mappings, and every reachable file mapping
in a temporary cluster bitmap. It accepts FAT-linked and contiguous
NoFatChain streams. Shared clusters, free-marked references, invalid links,
cycles, inconsistent lengths, malformed entry sets, and incomplete traversal
reject the proof. Definite corruption revokes mount-local version confidence;
I/O and allocation failures before mutation remain retryable. The caller
must hold the mount operation lock through the scan and any decision using its
result. It does not detect out-of-band media changes. The exFAT replacement
transaction consumes this proof under the same operation lock.

exFAT replacement accepts 0–2 MiB of complete kernel-owned input. The old
file mapping has an independent 2 MiB bound. A validated file entry set is
supported when its primary and stream entries occupy the same or different
512-byte directory sectors; filename entries can occupy later sectors and
remain unchanged. The operation records a VFS recovery pin before mutation,
stages new clusters
through the allocation bitmap and, for fragmented files, FAT copies, then
flushes staging before publication. It attempts one prepared write per affected
directory sector, writing a split stream before its primary checksum, and
flushes publication before reclamation. It never reuses the old file's
allocated clusters during staging. A confirmed publication advances the
mount-local generation and updates live nodes before reclaiming old clusters.
Failed or ambiguous metadata writes, data writes, cleanup, and flushes retain
the mount-owned transaction and an unresolved pin. Reads and mutations are
gated while that state is uncertain. A failed directory-sector write is not
proof that its bytes remained unchanged. The transaction retains old and
proposed publication images, allocation ledgers, current metadata-write
images, per-sector attempt/completion and flush progress, FAT-copy progress,
and cleanup cursors. The pin keeps those records discoverable after device
removal; there is no automatic retry or on-disk
recovery journal. ExFAT mount-local generations do not survive remount or
detect out-of-band modification. The size and entry-set restrictions are
implementation limits, not exFAT format limits.

exFAT exclusive creation uses the same staged-allocation and recovery-pin
machinery for 0–2 MiB of complete input. It validates the parent and the
reachable namespace under one mount operation-lock hold, then requires a free
entry-set run wholly within one 512-byte parent-directory sector.
The supported run consists of deleted primary entries before the directory
end marker or a zeroed tail that leaves a following end marker. A valid
UTF-16 filename may use multiple filename entries, provided the complete set
fits that sector. When the FAT-linked root has no suitable run, safe creation
can place the complete set in a zeroed following sector, appending one
directory cluster if required. It stages and flushes that sector before
linking the cluster, then retires an earlier end marker as the namespace
visibility boundary. The candidate allocation, FAT images, old and proposed
directory-sector images, and write progress remain with the pinned recovery
transaction after uncertainty. Non-root FAT-linked and NoFatChain directory
growth, and entry sets spanning sectors, are still unsupported. Names compare
through the mounted upcase table, including
system-attribute file entries; a matching name returns ALREADY_EXISTS without
mutation. Fresh contents and allocation metadata are staged before any
directory-sector publication attempt.
The proposed entry-set image includes the primary, stream, and filename
entries with their checksum; neighboring sector bytes remain unchanged.
Confirmed namespace visibility binds a new mount-local identity and generation
to the prepared return node. A failed or ambiguous write or flush retains the
transaction and
UNRESOLVED recovery pin, quarantines reads and mutations, and returns no
handle. This is runtime containment, not crash atomicity or power-loss
durability.

On the controlled 512-byte-cluster fixture, root growth with a one-byte child
requested 12,476 bytes at peak and while pinned, with 21 sector reads, 10
writes, and three flushes. A 2 MiB child requested 28,856 bytes at peak and
while pinned, with 4,126 reads, 8,200 writes, and three flushes. At the
32-pin limit, the latter is 923,392 requested bytes of retained metadata,
excluding allocator overhead, VFS objects, and syscall input buffers. These
are host-fixture measurements, not guest latency or device-I/O benchmarks.

With 512-byte clusters, the host fixture measured 7,264 requested bytes for
an empty create and 23,648 bytes for either contiguous or fragmented 2 MiB
creation while the transaction and prepared private node remained retained.
At the 32-pin limit, that latter figure is 756,736 requested bytes across
mounts, excluding allocator overhead, VFS objects, and syscall input buffers.
The namespace scan's temporary allocation is released before transaction
staging. The controlled 512-byte-cluster fragmented 2 MiB create fixture
measured 20,502 driver sector reads and 16,386 writes with bounded read views.
See [metadata-read measurements](../development/fat-metadata-performance.md)
for the exact measured revision and workloads. These are driver requests,
not physical-device I/O or universal volume costs.

With 512-byte clusters and both old and new FAT-linked files at 2 MiB, the host
fixture measured 41,840 bytes of requested transaction heap, excluding allocator
overhead, VFS pin objects, and the kernel-owned syscall input. Thirty-two such
retained transactions would request 1,338,880 bytes on that basis. Old
NoFatChain mappings use a validated range instead of a cluster array. The
namespace preflight's temporary bitmap and queue are released before staging.
Its scan and the per-cluster bitmap/FAT updates can dominate I/O on a large
or fragmented volume; ordinary reads do not run the full preflight.

The current scan accepts at most 1 MiB of cluster bitmap, 262,144 claimed
clusters, 4,096 directories, and 65,536 allocated directory-entry slots.
Its bitmap is proportional to volume clusters and its directory queue grows
from 16 to at most 4,096 entries. During the final queue growth, peak
requested memory is at most 1,196,032 bytes, excluding allocator overhead
and stack; steady maximum is 1,146,880 bytes. A 64-cluster in-memory fixture
requests 392 bytes and reads five sectors with bounded read views. Locked
ownership and safe-create scans reuse operation-local FAT/bitmap sectors and
forward directory cursors; complete validation and claim checks remain intact.
Ordinary public directory helpers retain their uncached traversal paths. The
scanner rejects unsupported active entry types rather than infer their
allocation ownership. Mount and transactions interpret both bitmap and upcase
streams through validated FAT chains, including fragmented placement. Only
one-FAT/one-bitmap ordinary exFAT volumes are admitted; TexFAT is refused.
These proof bounds and supported layouts are implementation restrictions, not
exFAT format limits. Replacement publication updates primary and stream
entries in one or two sectors; unchanged filename entries may follow in later
sectors. A split-set publication is not crash atomic, and an interrupted
checksum update may require external repair. Creation requires its complete
set to fit in one sector.
Ordinary FAT32 create and mkdir use existing directory slots. Safe
create-with-contents can also extend a directory.

FAT32 exclusive create-with-contents accepts 0–2 MiB with either a strictly
validated 8.3 name or a validated UTF-8 long name through the 255-byte VFS
limit. A long-name entry set spans at most three 512-byte sectors. The
8.3 short-name base has 1–8 ASCII letters, digits, underscores, or hyphens;
the optional extension has 1–3 of the same characters. Long names use a
deterministic collision-checked portable `~n` alias. Creation uses the
[FAT32 namespace policy](../filesystem/fat32/names.md): pinned Unicode
simple uppercase for imported LFNs, portable ASCII aliases, and operation-level
refusal when an OEM alias cannot be interpreted confidently. It checks the current
parent identity and write permission, name collision, and complete namespace
cluster ownership under the mount operation lock. A private VFS return node,
FAT32 identity, allocation ledger, publication images, and recovery pin are
prepared before mutation. The replacement transaction's fresh-chain staging,
FAT-copy updates, cancellation, FSInfo handling, and handoff are reused. One
prepared directory-sector write publishes a same-sector entry only after all
contents are staged when no end-marker relocation is needed. A failed
publication or postpublication FSInfo update
quarantines the mount and retains the transaction and discoverable pin. A
confirmed prepublication failure cancels fresh allocation without making the
file visible. The FAT32 identity is attached to the preallocated VFS node only
after confirmed cleanup; a failed VFS return discards that private node state.
When a long-name set spans sectors, the transaction retains up to three
original/proposed sector pairs and their attempted/confirmed write masks.
It writes LFN-only sectors first, flushes, then writes and flushes the short
entry sector. If an earlier end marker hides the prepared set, that marker's
sector is written and flushed last as the visibility boundary. A directory
can grow by up to two initialized clusters for a complete name set. Candidate
clusters, mirrored FAT allocation/link images, zeroing progress, and link
progress remain in the recovery inventory. A possibly successful tail link
makes its candidate directory-owned; cancellation never frees it as an
unpublished file allocation. An attached spanning-name transaction hides
namespace lookup and enumeration until resolved pin handoff. Failed writes or
flushes retain uncertainty and the pin. Power loss may leave an unlinked
allocation, an empty linked cluster, orphan LFNs, or a hidden entry; external
repair may be needed. This is runtime containment, not crash atomicity or
power-loss durability.
With 512-byte clusters, the controlled fixture measured 33,805 peak requested
heap bytes above its preallocated return node and 18,648 retained bytes before
pin finish for a 2 MiB create, excluding allocator overhead and the VFS pin.
The locked creation hook measured 8,225 sector-write requests and 12,327
sector-read requests; subsequent fixture read-back is excluded. At
the 32-pin limit, 32 such unresolved transactions could retain 596,736 bytes
of requested FAT32 metadata, excluding pins, nodes, and allocator overhead.
With 512-byte clusters, a controlled 255-byte name requiring two appended
clusters measured 33,288 peak requested heap bytes above the preallocated
return node, 23,032 retained bytes before pin finish, 14 sector reads,
8 sector writes, and 7 flush calls. Thirty-two such unresolved transactions
would retain 737,024 requested bytes, excluding allocator overhead, VFS pins
and nodes, and syscall input. The ownership proof's temporary allocation is
released before mutation. File-content storage remains independently bounded
at 2 MiB; these measurements use an empty file.
An empty same-sector create into a full 64 KiB-cluster directory measured
98,312 peak requested bytes; the cluster-sized zeroing buffer
is temporary. These are host-fixture allocation requests, not allocator
overhead or guest latency.

FAT32 removal of an empty long-name directory uses a recovery-pinned VFS
handoff. A complete ownership proof and strict emptiness walk run under the
mount operation lock before mutation. The driver inventories every directory
cluster up to the existing 4,096-cluster walk bound, independently of the
2 MiB file-content limit. It retains up to three original/proposed parent
directory-sector pairs. The short-entry sector is retired and flushed first,
then other LFN sectors are retired; only confirmed namespace retirement
permits directory-chain reclamation. Every confirmed FAT free advances a
cleanup cursor. A failed or ambiguous write, flush, or cleanup leaves the
transaction and pin discoverable, revokes mutation confidence, and hides
namespace reads. After confirmed cleanup, VFS finishes the pin outside the
operation lock. Open nodes for the removed directory remain tombstoned and
cannot follow reused storage. Power loss can leave orphan LFN entries or
unreclaimed clusters; this runtime protocol is not crash atomic or durable.
On the controlled 512-byte fixture, a three-cluster spanning-name directory
used 33,800 peak requested heap bytes, retained 5,356 bytes before pin
finish, and requested 34 sector reads, 6 sector writes, and 3 flushes.
The controlled 2,049-cluster directory (2,098,176 mapped bytes) used 34,050
peak requested heap bytes and retained 13,540 bytes, with 12,351 reads and
2,050 writes. These figures exclude allocator overhead and VFS pin storage.
The 4,096-cluster inventory requests at most 16,384 bytes; for a three-sector
name set the retained FAT32 payload is at most 21,728 requested bytes by the
fixture's structure sizes, or 695,296 across 32 unresolved pins, excluding
allocator overhead and pinned superblocks. A 4,096-cluster directory with
64 KiB clusters requires reading up to 524,288 data sectors per complete
directory walk; mirrored cleanup can request up to 16,384 FAT-sector writes
with four copies. Ownership proof, emptiness validation, and inventory are
separate reads under the operation lock, so large directory removal can hold
that lock for a long time. These are implementation bounds, not FAT32 format
limits.

FAT32 conditional replacement accepts 0–2 MiB of complete kernel-owned input.
It checks the retained mount-local inode and generation, current short entry,
and complete namespace ownership before mutation. It inventories at most 2 MiB
of old allocated clusters, including clusters beyond logical EOF, and stages
new data in a separate chain. This old-chain bound is an implementation limit,
not a FAT32 format limit. Every allocation candidate is recorded before its
FAT update; mirrored FAT copies are checked and updated individually. A single
prepared 512-byte directory-sector write publishes the new first cluster and
size, preserving neighboring entries and other fields. Only a confirmed
publication permits freeing the old chain. Optional FSInfo locations and
signatures are checked before mutation; valid hints are invalidated after
cleanup. Partial or failed writes quarantine the mount and retain both sector
images, chain inventories, phase, progress, and a discoverable VFS recovery
pin. The pin prevents normal unmount and retains the transaction after forced
device removal. A pending FAT or FSInfo sector write also retains complete
before and proposed sector images and its copy progress, so a torn metadata
write cannot erase the runtime evidence of neighboring fields. Ordinary FAT32
mutations remain nontransactional. The replacement transaction provides
runtime containment, not crash atomicity or power-loss durability.
A torn FAT-sector write may damage neighboring allocation entries, including
an old file's chain; quarantine prevents further runtime mutation but cannot
guarantee that the old mapping remains readable until recovery.
At 512-byte clusters, a fixture with 2 MiB of old allocated storage and a
2 MiB replacement measured 35,504 peak requested heap bytes above its live
node, 34,992 retained transaction bytes before pin finish, 12,321 sector
writes, and 16,519 sector reads. The retained figure includes two 4,096-entry
cluster ledgers and the transaction object with complete directory and pending
metadata-sector images; it excludes allocator overhead and the VFS pin. At
the 32-pin registry limit, that geometry could retain 1,119,744 requested
transaction bytes across mounts. The separate ownership scan can temporarily
request up to 1,146,880 bytes; it is released before mutation. Larger cluster
sizes reduce ledger length but increase the temporary data buffer up to 64 KiB.
The transaction releases that buffer before retaining an uncertain outcome.

Safe FAT32/exFAT transactions coalesce a fresh allocation and its previous
fresh-cluster link when both entries share one FAT sector. Both cluster
identities and complete before/proposed images remain recoverable; bitmap,
publication, cleanup and flush ordering are unchanged. See
[metadata read/write measurements](../development/fat-metadata-performance.md)
for exact behavior, current performance comparisons and retained-memory limits.

Identity-bound I/O prevents an old mount from sending callbacks to a new
registration in a reused slot. It does not make ordinary FAT32 or exFAT
mutations transactional: FAT32 allocation, chain freeing, FAT copies, and
FSInfo hints still require separately fallible writes.
exFAT bitmap/FAT updates and multi-sector directory entry sets can likewise
fail after partial persistent changes; its volume-dirty flag is not a recovery
record. A failed write or flush after callback admission may have changed
media. Revoking mount-local version confidence prevents subsequent ordinary
driver operations from treating that state as confirmed. Ordinary exFAT
mutation paths still lack transaction recovery inventory.

GPT discovery creates child block instances. Filesystem probes publish
recognized type and native label as copied metadata. Current supported
filesystems are MGFS, FAT32, and exFAT; unknown or corrupt media remains a
visible block object without becoming mountable.

## Mount authority and removal

The VFS mount table is the authoritative source for mounted state and path.
Root and role-associated boot storage are system-managed. Removable mounts are
restricted to policy-selected children of `/vol`; client commands cannot
choose arbitrary mount paths.

A clean unmount preflights open file/directory references and process working
directories, stops new filesystem I/O, flushes the filesystem and block
device, then detaches the namespace. Multi-volume eject preflights every
mounted child before the bounded batch detach, so a busy child prevents
partial teardown.

Physical removal is different from clean unmount. The block instance becomes
offline and VFS immediately detaches its namespace without attempting a flush.
Its superblock remains dead while stale handles unwind; later access returns
the device-gone result. A new device instance cannot revive those handles.

VFS also has internal recovery pins for filesystem transactions. Admission
registers a boot-local pin ID and takes one superblock reference while the
instance is live. Active and unresolved pins remain discoverable after forced
detach; either prevents clean unmount through the existing reference preflight.
A confirmed or safely resolved active operation releases its pin only after
leaving the superblock operation lock. An unresolved pin cannot be finished or
retired by this API and intentionally keeps filesystem state alive until a
separately designed recovery or abandonment path exists. The pin holds no
replacement data and performs no recovery or persistent I/O by itself.
The registry holds at most 32 live pins, including unresolved pins from dead
mounts. Admission at capacity returns no-space before taking a reference or
starting mutation; it never evicts unresolved state. This bounds registry
enumeration and pin objects, but each pinned superblock retains its filesystem
state, and each filesystem transaction has independent retained-memory costs.
Unresolved pins cannot be retired through this API.

Ordinary VFS file writes and truncation may opt into recovery-aware filesystem
hooks. A hook runs under the mount operation lock, acquires its pin before
its first potentially persistent mutation, and returns a confirmed byte count
or error separately from any resolved pin. VFS finishes a resolved pin after
unlocking; a failed finish reports uncertainty while the pin and filesystem
transaction remain discoverable. An uncertain hook outcome reports no
confirmed bytes and retains an unresolved pin. A definite partial write may
still report its confirmed byte count. Existing filesystem callbacks continue
to use their prior byte-count and truncate contracts. Native object writes
can report the existing `MG_ERR_UNCERTAIN` without a syscall signature change.
This runtime ownership contract does not make an ordinary write atomic or
provide recovery after power loss.

MGFS has private staging-and-cancellation and publication paths for the
internal conditional replacement operation. They accept an exact-length,
block-streamed source for an existing MGFS 1.2 regular file, validate its
generation and physical ownership
under the mount operation lock, and obtain a recovery pin before persistent
mutation. The MGFS-private replacement path accepts at most 512 data blocks
(2 MiB). At worst fragmentation, its 512 extents use two inline slots and five
linked 126-entry extent-list blocks. The transaction records at most 517 fresh
allocation candidates, including those list blocks. A staging-only internal
test path retains its 128-block limit; public
replacement and exclusive creation accept up to 2 MiB. Small
transactions use the existing inline inventories; larger replacements allocate
one proportional inventory before mutation. It also reserves two complete
4 KiB table images before mutation. Allocation candidates are
recorded before bitmap writes; staged data and list blocks are always fresh.
The original record and data remain untouched during staging. Prepublication
cancellation reclaims all staged allocations and restores the original superblock
state. Replacement first inventories at most 512 old mapped data
blocks and at most 512 old extent-list blocks; a valid zero-inline record may
place every extent in its own list block. Oversized old mappings are rejected
before mutation. These are temporary implementation limits, not MGFS format
limits. The complete proposed table block is then written once; only confirmed
publication permits old data and list-block reclamation. Cleanup progress is
retained separately from staged-allocation cancellation progress. Complete
success restores the original superblock state and releases the pin after
leaving the operation lock.

The mount-owned transaction stays attached during the pin-finish handoff.
Ordinary MGFS mutation entry points check it under the operation lock, so none
can change the retained images before finish resolves. The pin binds an MGFS
handoff callback at admission. VFS keeps that pin registered while it takes
the operation lock and calls the callback without its metadata lock. Only a
successful callback detaches and frees the transaction; VFS then unlocks and
releases the pin reference. A callback failure leaves the transaction attached
and its pin registered as UNRESOLVED. A failed lock attempt leaves the pin
registered and the transaction attached. Ordinary pin finish cannot bypass a
bound callback. Reads remain available during a resolved handoff; device
detachment continues to reject new VFS operations.

On an uncertain allocation, data write, table publication, or cleanup, MGFS
quarantines mutations and retains both table images, fresh-allocation ledger,
old mappings, mutation phase, cleanup progress, and unresolved pin. A failed
table-image write may have changed any sector of the table block; filesystem
reads are rejected on that mount. This read guard is specific to uncertain
publication and does not change reads for unrelated mutation quarantines.
Confirmed publication followed by failed reclamation keeps the new mapping
and old cleanup inventory; it never cancels the new allocations. No automatic
retry, table restoration, or retirement exists.
The ledger bounds block bookkeeping. On x86-64, the transaction object is
26,952 bytes; the maximum proportional inventory is 41,000 bytes, so an
unresolved replacement retains at most 67,952 requested heap bytes of
transaction metadata. The ownership preflight adds a 4,144-byte scan object
and at most 16,400 bytes of target ranges, for a measured peak of 88,496
requested heap bytes under a 512-extent old mapping. A 2 MiB replacement
against the small fixture old mapping measured 51,704 bytes. These figures
exclude allocator headers, stack buffers, VFS pin/superblock state, and caller
input buffers. Thirty-two worst-case unresolved replacement pins could retain
2,174,464 bytes of MGFS transaction metadata in aggregate, plus those other
costs. This is an inventory-derived budget, not a 128 KiB allocation cap.
Staging scans up to the entire data-area bitmap for
free blocks and adds one bitmap write per allocation and reclamation; the
ownership preflight also scans allocated records and their mappings. This is
runtime containment, not crash atomicity or power-loss durability. The internal
VFS replacement interface uses an existing retained file handle and its
open-time record identity and generation. It checks current record facts and
authorization under the mount operation lock, then runs the MGFS transaction
under that same lock. A resolved recovery pin is finished after unlocking;
only confirmed publication, cleanup, and pin finish refresh the handle's save
expectation. Native `file_replace()` exposes this existing-file path; it does
not create files.
Confirmed publication updates cached sizes for all registered VFS nodes of the
record, including when old-storage cleanup later fails. Successful ordinary
MGFS writes and truncation also update those cached sizes. Open-handle offsets
are unchanged. The public 2 MiB replacement bound is an implementation limit.

MGFS 1.2 also has a create-with-contents transaction used by an internal VFS
exclusive-create interface. It holds the mount operation lock through admission,
staging, both record-table
publications, and cleanup. The complete child contents and a copy-on-write
directory append are staged in fresh data-area blocks. The child record is
published first; the parent directory record is the visibility boundary.
When both records share a table block, the parent image is built from the
confirmed child image. The old directory tail and extent-list blocks are
reclaimed only after confirmed parent publication. A child published without
a confirmed parent publication remains an orphan under mutation quarantine,
with its allocation and record metadata retained through an unresolved pin;
the kernel does not assume a failed table write left media unchanged.

The MGFS-private create transaction accepts at most 512 child data blocks
(2 MiB) and 512 existing parent directory data blocks. It validates mapped
blocks, extent count, and sparse old extent-list chains separately. A partial
old tail is copied to fresh storage, and an appended entry may need a second
fresh directory block. The proposed parent can therefore require up to 514
extent descriptors and five linked extent-list blocks. A maximally fragmented
child requires five linked list blocks. The shared ledger reserves up to 524
fresh allocation candidates before mutation; child and parent mapping
inventories grow proportionally when the inline arrays are insufficient.
The public VFS, native syscall, libc, and Reed creation limits are 2 MiB.
On x86-64, the create transaction object requests 48,648 bytes. Its maximum
child ledger/extent inventory requests another 20,576 bytes, and its maximum
parent mapping inventory requests 36,928 bytes. Thus an unresolved create can
retain 106,152 requested heap bytes of MGFS transaction metadata. The fixture
measured a 126,696-byte peak with a maximally fragmented child and 512-block
parent, including temporary ownership-preflight allocations. Thirty-two such
unresolved pins could retain 3,396,864 bytes of MGFS transaction metadata.
These figures exclude allocator overhead, stack buffers, VFS pin/superblock
state, record-ID index growth, and caller input buffers.
These are implementation bounds rather than MGFS format limits. The recovery-pin
cap bounds transaction count but does not make total retained recovery memory
small. VFS allocates the result node
and read/write handle before mutation, binds them to the published child ID,
and returns the handle only after the pin handoff succeeds. Native
`file_create_contents()` and Reed enforce the same 2 MiB content limit.
The publication sequence provides
runtime containment, not crash atomicity or power-loss durability.

Reed limits its editable UTF-8 text to 2 MiB and checks the serialized byte
length before either safe save operation. The optional final newline counts
as one file byte: a full 2 MiB editing buffer with that option enabled cannot
be saved until the user shortens it or disables that option. An exact 2 MiB
file ending in a newline loads as 2 MiB minus one editable byte plus the
newline setting, and can be saved without changing its contents. Rejected
saves preserve edits, undo state, the open handle, and its save expectation.
Stale-generation conflict choices remain Cancel, exclusive Save As, or one
explicit generation-checked Overwrite after a complete snapshot. Uncertain
outcomes are never retried automatically.

For a maximum-content syscall, 512 input chunks request 2,097,152 bytes and
their pointer ledger requests 4,096 bytes on x86-64. The streamed snapshot
requests no size-proportional heap allocation; MGFS uses a 4,096-byte transfer
buffer and a 4,096-byte extent-list buffer on the kernel stack. Its existing
grouped block-read helper may temporarily request up to 32,768 heap bytes.
VFS adds no size-proportional allocation.
At maximum size, Reed can simultaneously request roughly 2 MiB each for
editable text, its previous baseline, serialized save bytes, and a prepared
new baseline; its undo and redo stacks each permit 8 MiB of text. Including
the syscall copy gives a conservative 26 MiB component sum before MGFS
metadata. The measured maximum MGFS transaction/preflight peaks above add
88,496 requested bytes for replacement or 126,696 for creation. These sums
exclude allocator overhead, terminal state, and fixed VFS/process objects;
they are bounds on requested components, not a measured whole-system RSS.
An unresolved transaction retains only its MGFS metadata and recovery pin,
not Reed or syscall input buffers. Large files still require a complete
Reed buffer and a complete kernel-owned input copy; streaming or spooling
across that userspace boundary remains future work.

Mangrove supports MGFS 1.2 exclusively. Bootloader, kernel, and host tools
reject older minor versions. Persistent development images are checked before
copying or updating; unsupported images are preserved and require explicit
recreation after backup.

## `volumed` policy

Volumed registers the `volume` endpoint, subscribes to device, block, and USB
events, and rebuilds a bounded authoritative snapshot at startup and after
event overflow. It operates on leaf partitions, or a whole-disk filesystem
when no partition child exists. It never enumerates USB hardware itself.

Automount eligibility requires a current removable, non-system-managed,
recognized, unmounted, non-suppressed instance. Supported filesystems mount at
`/vol/<label>` when the label is safe as a path component, otherwise at the
current block name. Exact duplicate labels receive deterministic `-2`, `-3`,
and later suffixes without renaming existing mounts or changing stored labels.
Read-only media is mounted read-only; an unexpected writable-mount failure is
not retried as read-only.

Manual unmount and eject set suppression in the kernel block instance. That
state survives a volumed restart but disappears with physical removal or
reboot. Explicit mount clears suppression after success. Per-instance mount
failure and mountpoint ownership state cannot transfer to a replacement.

## Destructive administration

`diskutil` is the selected-disk administration context. The kernel accepts
format, label, and GPT mutation only from an authenticated, process-local
storage-management session and resolves the exact current block instance.
Mounted, busy, read-only, or system-managed targets are rejected. Formatter
writes are extent-bounded; GPT and formatter paths flush, re-probe, and verify
before reporting success. Formatting does not unmount or mount a target.

GPT mutation validates both table copies and their CRCs, uses aligned usable
ranges, writes backup structures before primary structures, flushes, refreshes
the partition model, and verifies the resulting topology. It does not repair
inconsistent tables or wipe deleted partition contents.

## AHCI command failure isolation

AHCI uses one page-sized, permanently port-owned transfer buffer. Commands
never DMA into caller-owned file buffers. Large requests are split into bounded
transfers; successful read chunks are copied to the caller only after status and
transferred-byte checks pass. Earlier chunks of a failed multi-chunk request may
already have completed, so a failed write is not a rollback guarantee.

Command errors or timeouts isolate the port and attempt bounded command-engine
and FIS-engine shutdown, in that order. No command is retried or restarted on
that port. All published DMA allocations remain retained for the boot, even if
engine shutdown cannot be confirmed. Isolated devices remain visible in the
existing registry but reject subsequent reads, writes and flushes. ATA flush
commands are checked like other commands; an unsupported flush fails explicitly.
This is runtime lifetime protection, not crash atomicity or power-loss recovery.

## Authoritative code

- `kernel/src/block.c`, `kernel/src/storage/gpt.c`, and filesystem drivers
- `kernel/src/vfs.c`, `kernel/src/device.c`, and storage syscall handling
- `libc/include/mg/device_service.h`, `mg/volume_service.h`, and `mg/storage.h`
- `userspace/deviced/main.c`, `userspace/volumed/main.c`, and
  `userspace/diskutil/main.c`

## SMP ownership and retirement

The block I/O gate is a scheduler-backed mutex with recursive admission for
partition callbacks. Its short state lock protects recursion and callback depth;
no spinlock spans I/O or scheduler parking. Quiesce is nonblocking and refuses
an active callback, including a callback on the requesting thread.

A registered online device becomes retiring when `block_device_mark_gone()`
clears online under the registry lock. New I/O is refused. The descriptor remains
registered while active I/O drains and while VFS consumes its removal identity.
`block_unregister_by_id()` retires first, then tries quiescence. On false, callers
must preserve all backing resources. On true, the drained descriptor is removed
and its cache invalidated. Raw registry pointers are not lifetime leases; I/O
captures an instance ID before parking and validates it after admission.
Driver-owned descriptors still require their owning driver's lifetime.

GPT partition backing is cleared only with the gate held and after successful
unregister. Failed cleanup retains occupied backing slots and reports failure;
USB teardown retains its parent and class state for a later attempt. GPT read,
scan and planning operations share the mutation admission guard with GPT/format
writes because they use common static work buffers. Admission uses atomic
compare/exchange, returns busy rather than spinning, and holds the existing I/O
gate across validation, writes and readback. This serializes management work
against callbacks and destruction without adding allocations or a new blocking
lock. Physical retirement may still invalidate an operation in progress; errors
are reported and multi-write GPT/format operations are not disk transactions.

Lock order: management admission -> logical I/O gate -> short registry or
partition-state lock. Registry and partition locks are released before calling
one another, VFS, IPC, or gate admission. VFS metadata preflight is short and
never acquires a blocking filesystem mutex. Mutex wakeup orders state lock ->
scheduler lock; scheduler lifecycle drops its lock before cancelling waiters.
No crash atomicity or power-loss durability is implied by these locks.

## MGFS record identity and failed creation

The mount's Record-ID index contains live identities. Successful record release
removes the old ID and compacts its linear-probe cluster before another operation
can reuse the slot. New IDs remain monotonically consumed; deleted identities
never acquire replacement contents. The index grows only when live occupancy
requires it, before creation changes disk metadata. Lookup and ordinary churn do
not scan disk or rebuild the whole index.

Creation prepares its VFS object and index capacity first. Known unpublished
failures release the new record and any known, unreferenced directory allocations.
An uncertain metadata write, failed cleanup, or failure after publication returns
the existing uncertain-outcome error and retains resources that might be named on
disk. MGFS then blocks mutations on that mount, including attempts to mark it
clean; reads remain available where metadata validates. Do not retry the create
blindly. Offline checking/recovery is required before resuming writes.

This guard is mount-local and is not a persistent recovery protocol. Existing
NEEDS_FSCK metadata is retained where writes allow, but MGFS has no transaction
rollback or automatic crash recovery. Remounting does not establish that an
uncertain filesystem is safe; its contents must be checked/recovered first.

The same mount-wide guard covers ordinary writes, truncation, deletion and
rename. Once a write has attempted to change file data, or allocated storage,
any subsequent failure quarantines the mount, including a failed read before
the Record generation advances. Successful Record or namespace publication
also makes later reclamation and clean-state failures uncertain. A failed
tombstone write retains the target Record and quarantines the mount, including
either write of a spanning entry. Rename cannot safely retry after publishing
its destination but failing to remove its source.

Validation, allocation and read failures before a substantive mutation do not
quarantine. Known unpublished create/directory allocations may still be cleaned
up; failed cleanup requires quarantine. The NEEDS_FSCK marker alone is not a
substantive mutation for this distinction. Successful operations keep their
existing behavior and disk I/O; tracking adds no allocations or read-backs.
Reads remain allowed where metadata validates. The guard does not restore data
already overwritten by an ordinary failed write, guarantee readable torn
metadata, or provide atomicity or power-loss durability. An ordinary write
still reports failure through its existing zero-byte result; integer-returning
mutations report the uncertain-outcome error when quarantine is established.

## MGFS conditional-replacement ownership preflight

The private `mgfs_ownership_preflight` is used by conditional replacement
admission; exclusive creation uses its typed ownership validator. Each caller
must hold the mounted superblock's operation lock for the entire check and subsequent staging. It
reads allocated Records, extent-list blocks, and allocation bitmap blocks. It
checks that every referenced data-area block is marked allocated and that no
other Record refers to the target file's data or extent-list blocks. The target's
own data and extent-list ranges must also be disjoint. This prevents staging
from selecting a referenced block whose bitmap bit is incorrectly clear and
prevents replacement cleanup from freeing storage shared with another Record.

A definite malformed mapping or ownership conflict quarantines mutations on
that mount, without changing media or freeing blocks. Failed reads and temporary
memory allocation before mutation remain retryable. The check does not prove
that unrelated Records have no overlap with each other, repair corrupt media,
or protect against an external raw writer changing the device after the scan.
It provides no crash atomicity or power-loss durability.

Memory is proportional to the target's data extents and extent-list blocks,
plus one bitmap block, one table block, and a bounded scan context; there is no
persistent global ownership index. The target ranges are sorted in
`O(T log T)` time. A scan reads allocated Record table blocks and extent-list
blocks, and examines each referenced data block's bitmap bit. With a one-block
bitmap cache, heavily fragmented mappings can cause repeated bitmap reads;
worst-case work is proportional to all referenced blocks and extent-list
entries, rather than just the target file size. Allocation failure leaves the
mount and media unchanged.

Host fault-injection coverage is in `mgfs_write_test.py`,
`mgfs_directory_test.py`, `mgfs_record_lifecycle_test.py` and
`mgfs_mutation_failure_test.py`. These execute production mutation control flow
with simulated storage/helper failures; they are not QEMU integration tests.
