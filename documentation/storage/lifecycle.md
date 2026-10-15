# Block, filesystem, and volume lifecycle

Mangrove separates storage mechanism, generic device reporting, and removable
volume policy:

```text
kernel block, GPT, filesystem drivers, and VFS -> mechanism and authority
deviced                                      -> generic device snapshots
volumed                                      -> removable-volume policy
mount, unmount, eject, lsdsk, diskutil       -> clients
```

## Read-only mount policy

Read-only policy is immutable state on the retained VFS superblock, initialized
before mount publication. It survives removal of the mount-table slot and cannot
be bypassed by trusted credentials, owned creation, administrator authorization,
or an already-open writable handle. Shared mutation dispatch checks it before
calling either pinned or legacy filesystem hooks. Recovery-pin admission also
rejects read-only superblocks before allocating or registering a pin.

FAT32 and MGFS retain this policy at driver mount admission and reject mutation
entry points before transaction preparation. FAT32's mounted write and allocation
helpers enforce it as well. exFAT retains its private mount policy, initializes
the same superblock field, and rejects mounted writes. Rejection changes no
persistent contents, allocation hints, generations, or transaction/quarantine
state. Status APIs report access denial; existing count-only write helpers return
zero without advancing the offset. Detached handles retain the existing
invalid/device-gone behavior. Read-only mounts still allow reads and snapshots.

This policy covers mounted filesystem operations. Formatting and label changes
are separate device-management operations restricted to unmounted media; they
do not provide a route around mount policy. No remount-to-writable API is added.

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

Mounted FAT32 reads, writes and sync use the registration ID captured by the
filesystem at mount entry.
Mounted exFAT reads, writes, dirty-flag updates, and sync flushes use the ID
captured while constructing its filesystem state. The identity-bound flush
uses the same I/O admission gate as reads and writes; a missing backend flush
callback retains the existing synchronous-write contract. FAT32 sync refuses
unresolved transaction state and flushes only that retained registration;
failed sync aborts clean unmount/eject. Probe and formatting use separate
device-management paths. exFAT label mutations use a private retained recovery
owner, without exporting an ordinary namespace mount.

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

FAT32 exposes no raw cluster-allocation, chain-extension or chain-freeing API.
Allocation changes, cancellation and reclamation use private primitives inside
validated, recovery-owned transactions, including mirrored FAT progress and
FSInfo handling. Optional boot diagnostics exercise allocation through VFS
file/directory lifecycle operations. The read-only `fat32_get_cluster_link`
inspector remains available to callers retaining a superblock and holding its
operation lock. It reads the selected active FAT through the saved device
identity; one observed link is neither an ownership proof nor a comparison of
all FAT mirrors.

The preflight must run under the mount operation lock. A standalone result is valid only
until that lock is released; it cannot detect independent changes to mounted
media. Ordinary FAT32 writes and resizing may retain a mount-local certificate
after a complete proof. Rebuilding the proof invalidates old per-node mapping
summaries. Every mounted write invalidates it before block
admission; only confirmed ordinary transactions that preserve unique ownership
restore it during pin handoff. Other mutations force a new proof. No certificate
is reused while version confidence is revoked, a transaction is pending, or the
device identity is dead. This is an internal invariant, not a reusable public
ownership certificate. It performs no media
writes. FAT32 replacement and exclusive create-with-contents consume the result
under that same lock. A read or allocation failure remains retryable;
definite corruption revokes mount-local
version confidence. Claims use lazily allocated bitmap chunks of at most 4 KiB,
retained until the complete proof ends. The pointer index covers the mounted
FAT32 cluster range and requests at most 64 KiB; large sparse volumes do not
allocate their entire bitmap. There is no fixed cluster-claim or directory-count
ceiling. A segmented FIFO releases consumed pages and reuses its final page for
narrow trees. One directory scratch buffer is reused without recursion.

The proof has an 8 MiB requested-heap budget, including the pointer index,
bitmap chunks, pending-directory pages and scratch buffer. Exhausting the budget
returns `VFS_ERR_BUFFER_TOO_SMALL`; allocation failure returns `VFS_ERR_NO_MEM`.
Both release every temporary allocation, leave media unchanged and prove nothing.
The budget accommodates a maximum-size FAT32 file at 512-byte clusters (roughly
1 MiB of claim bits) with room for namespace traversal, while bounding temporary
kernel memory. It is a resource policy, not a format or file-size limit; dense
large volumes or wide trees can still exceed it. Allocator overhead and kernel
stack are separate. Proofs on different mounts may run concurrently; 32 active
mounts could request at most 256 MiB in total. There is no reservation of that
memory: ordinary heap exhaustion fails the read-only proof safely. No proof
allocation is retained by a recovery pin.

Ownership traversal uses the volume's data-cluster count as its chain bound and
claims every reachable directory cluster, including allocation after an end
marker. Duplicate claims reject cycles and cross-links. Strict LFN, dot-entry,
end-tail and mirrored-FAT checks remain active. This proof does not certify
unreachable allocated chains or repair media. Other directory operations retain
their existing 4,096-cluster walk and safe-growth slot restrictions.

Generated production-function fixtures measured 37,449 requested bytes and
2,051 sector reads for the exact 128 MiB chain regression, 1,055,241 bytes for a
maximum-size FAT32 file, 8,720 bytes for 12,000 nested directories, and 569,880
bytes for 70,000 queued directory children. File-chain traversal uses two
operation-local mirrored-sector views; alternating fragmented runs remain
cached without skipping mirror comparisons or device checks. Worst-case random
fragmentation can still request up to four FAT-sector reads per claimed cluster.
Each reachable directory is scanned once; ordinary reads and lookups do not run
this proof. Host CPU timings and independent image checks are reported by the
scaling fixture; they do not establish guest lock-duration or hardware bounds.
See [FAT metadata measurements](../development/fat-metadata-performance.md) for
cache lifetime and verification limitations.

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

exFAT unlink and empty-directory removal use the same mount transaction and VFS
post-unlock pin handoff. They inventory at most three directory-sector pairs,
retire/flush the primary sector, retire/flush remaining secondary sectors, and
only then clear allocation bitmap ownership. The final cleanup flush precedes
resolved handoff. Uncertainty preserves images, per-sector attempts, original
allocation mode/start/count, the current/next cleanup cursor and pending bitmap
image. The original FAT chain stays intact; the mount gate prevents reuse while
that cursor is owned. No 2 MiB deletion cap or proportional old-chain array is
introduced. Complete namespace proof limits still apply, including the aggregate
65,536-directory-slot budget; an incomplete proof rejects before mutation.
Retained nodes/cursors become tombstones instead of following freed storage or
reused directory slots. Retirement requests at most 12,232 metadata bytes per
pin (391,424 across 32 mounts), excluding allocator and VFS-object overhead.
Runtime pins do not repair post-crash orphan secondaries or unreachable
allocations; see the [exFAT contract](../filesystem/exfat/README.md).

exFAT rename uses an optional pinned hook in all ordinary, trusted and
expected-identity VFS paths. One retained superblock and operation lock cover
both directories; the pin's reference covers post-unlock handoff. The driver
inventories source/destination sectors before mutation, publishes and flushes
destination secondaries and primary before retiring/flushing the complete source
set, and never frees the moved allocation. Same-slot renames publish the primary
checksum last. Growth reuses the existing directory protocol, with up to two
appended clusters for a maximum-length name. Shared source-sector images are
refreshed after confirmed destination/growth changes. Uncertainty retains
namespace evidence and possibly linked allocations; failed handoff cannot return
success. Stable identities survive movement, while generation advancement makes
old expectations stale. Moves into the source subtree reject before mutation.
Maximum retained rename metadata is 59,336 requested bytes per pin (1,898,752
across 32 mounts), excluding proof, allocator and VFS overhead. The temporary
allocated-slot proof budget still restricts large, mostly empty directories.
Interrupted moves can leave duplicate references or orphan secondaries after
power loss; runtime containment is not atomic rename or post-reboot repair.

exFAT exclusive creation uses the shared staged-allocation, namespace sector
images and recovery-pin machinery for 0–2 MiB of complete input. Valid entry
sets can span up to three sectors and fragmented directory clusters. Admission
validates the parent, full namespace ownership and upcase-equivalent collisions.
Unused slots or zeroed end tails can be reused; malformed sets and live orphan
secondaries reject before mutation. File contents are staged and flushed before
publication. Spanning sets publish/flush secondary sectors before their primary
checksum sector. Growth can append two minimum-size clusters, extend contiguous
NoFatChain directories or convert them, update split parent metadata and relocate
end markers before spanning entry publication. Current sector images preserve
confirmed neighboring changes. Same-sector growth retains marker-last visibility.

All return objects, ledgers, images and pin capacity precede mutation. Confirmed
publication binds a new stable identity/generation; post-unlock pin completion
precedes returning the handle. Cancellation before linkage clears every fresh
allocation and flushes cleanup. Possibly referenced allocations are retained
on uncertainty; no usable handle is returned. Interrupted publication can leave
orphan secondaries, unreachable contents or invalid checksums after power loss.
Runtime quarantine does not provide atomic publication or post-reboot recovery.

The measured worst creation transaction requests 72,904 metadata bytes, or
2,332,928 across 32 pins, excluding allocator/VFS overhead and syscall input.
A 2 MiB child with full directory conversion measured 24,750 reads, 8,238 writes
and seven flushes. Temporary namespace proof peaks at 1,196,032 requested bytes
and is freed before staging. Existing proof limits, including 65,536 allocated
directory slots, still restrict otherwise valid layouts and need scalable
replacement; see the [exFAT contract](../filesystem/exfat/README.md).

With 512-byte clusters and both old and new FAT-linked files at 2 MiB, the host
fixture measured 41,856 bytes of requested transaction heap, excluding allocator
overhead, VFS pin objects, and the kernel-owned syscall input. Thirty-two such
retained transactions would request 1,339,392 bytes on that basis. Old
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
Ordinary FAT32 create, mkdir, and exclusive create-with-contents share the
same validated transaction, including cross-sector names, directory growth
and end-marker relocation.

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
With 512-byte clusters, the controlled fixture measured 19,176 peak requested
heap bytes above its preallocated return node and 18,664 retained bytes before
pin finish for a 2 MiB create, excluding allocator overhead and the VFS pin.
The locked creation hook measured 8,225 sector-write requests and 12,327
sector-read requests; subsequent fixture read-back is excluded. At
the 32-pin limit, 32 such unresolved transactions could retain 597,248 bytes
of requested FAT32 metadata, excluding pins, nodes, and allocator overhead.
With 512-byte clusters, a controlled 255-byte name requiring two appended
clusters measured 23,560 peak requested heap bytes above the preallocated
return node, 23,048 retained bytes before pin finish, 14 sector reads,
8 sector writes, and 7 flush calls. Thirty-two such unresolved transactions
would retain 737,536 requested bytes, excluding allocator overhead, VFS pins
and nodes, and syscall input. The ownership proof's temporary allocation is
released before mutation. File-content storage remains independently bounded
at 2 MiB; these measurements use an empty file.
An empty same-sector create into a full 64 KiB-cluster directory measured
77,184 peak requested bytes; the cluster-sized zeroing buffer
is temporary. These are host-fixture allocation requests, not allocator
overhead or guest latency.

## FAT32 ordinary creation and directory initialization

VFS selects optional `create_pinned` and `mkdir_pinned` hooks while retaining
its operation reference through post-unlock recovery-pin finishing. Legacy
filesystem hooks remain available for drivers without these hooks; FAT32's
legacy callbacks refuse mutation. FAT32 has no alternate owned-create callback.
Under operation_lock, VFS checks current access, read-only mounts and mounted
child-name collisions. The driver validates the parent, current namespace and
complete ownership proof before any persistent mutation. VFS allocates a private
72-byte return node; driver identity and transaction allocations also precede
mutation. No ordinary allocation follows confirmed publication.

Ordinary file creation publishes first cluster zero and size zero. Subsequent
ordinary writes allocate storage incrementally. Mkdir stages one fresh cluster
containing `.` pointing to itself and `..` pointing to the parent's first cluster
(or zero for the root), followed by a zero end marker. The parent directory entry
has the directory attribute and size zero. This child initialization and its
mirrored FAT allocation are flushed before any parent visibility write. Parent
growth uses separate fresh allocations and the existing retained linkage images;
possibly linked parent clusters cannot be cancelled as private child storage.

Contents/allocation staging, namespace publication, FSInfo cleanup and resolved
handoff must all be confirmed. Publication and cancellation now flush even without
parent growth. A mutation or flush failure retains the unresolved pin, ledgers,
sector images and progress; no usable return node is exposed and the mount loses
version confidence. Definitive prepublication failure can cancel only provably
private allocations; failed cancellation is uncertain. Newly created identities
start at generation one and the parent generation advances after confirmed
completion. Namespace mutation invalidates the cached ordinary-write proof.

VFS registers the prepared node under operation_lock but lookup cannot expose
it until the filesystem's handoff completes. Failed handoff discards private
node state under that same lock. After completed handoff, the node follows normal
mounted-cache lifetime: another lookup may already retain it. A subsequent detach
can prevent return to the creating caller, but must not free that public node.
The operation reference keeps the superblock alive through these decisions.
The same public-node rule applies to exclusive create-with-contents: a late
failure to return its prepared handle does not free a node another lookup may
have acquired. Its private-state cleanup also runs under operation_lock.
Native create/mkdir signatures are unchanged; uncertainty uses the existing
`MG_ERR_UNCERTAIN` mapping. Retained nodes become tombstones on removal and do
not follow reused directory slots.

The common record remains 2,248 bytes. Ordinary create adds no child allocation
ledger; mkdir adds one four-byte entry and a temporary cluster buffer. A worst
spanning-name mkdir with two appended parent clusters retains 23,020 transaction
bytes plus a 64-byte prepared identity on uncertainty. Including the 56-byte
registry entry, 32 such pins retain 740,480 requested bytes, excluding allocator
overhead and pinned superblocks. The independently budgeted 8 MiB namespace
proof ends before transaction admission; the 72-byte VFS node may coexist with
that proof. The largest measured transaction peak is 80,436 requested bytes
including the return node and identity, with a 64 KiB staging buffer. Detailed
measurements are in [FAT metadata performance](../development/fat-metadata-performance.md#fat32-ordinary-create-and-mkdir-resources).

Existing traversal/growth limits (4,096 directory clusters and 65,536 growth
slots), the proof's memory budget, ambiguous OEM alias refusal and strict
end-tail validation remain explicit restrictions. No content-size ceiling is
introduced for ordinary subsequent writes. Rename shares the same namespace
placement and directory-growth mechanisms, with separate source-retirement
and directory-parent evidence.
After power loss, private allocations can be orphaned, linked parent capacity
can survive without the new child, or prepared LFNs can require repair. A torn
sector can damage neighboring metadata. Runtime pins are not post-reboot
recovery, crash atomicity or a durability guarantee.

Every FAT32 file unlink uses the recovery-pinned retirement path, whether the
name is short, a same-sector LFN, or a spanning LFN. Direct legacy unlink and
rmdir callbacks return unsupported without mutation because they cannot
transfer recovery ownership. Mounted VFS dispatch selects the pinned hooks.
Under one mount operation-lock hold, strict namespace validation and the
complete scalable ownership proof precede transaction preparation and every
persistent mutation. Shared, cyclic, malformed, or incompletely validated
allocations are never reclaimed.

File retirement counts the entire chain, including allocation beyond logical
EOF, against the mounted cluster geometry. It retains no file-sized array and
has no deletion-specific content limit. The transaction holds a current
cluster, validated successor, completed-cluster count, current FAT-copy
progress, and before/proposed images for the pending metadata sector. It
captures the successor before clearing any FAT copy and advances only after
all configured copies confirm the free. Prior confirmed frees are represented
by the completed cursor; the remaining chain stays reachable through its
retained successor. There is no automatic retry after uncertainty.

The complete short/LFN entry set is retired and flushed before reclamation.
The short-entry sector is retired and flushed first; only then are remaining
LFN sectors retired. Namespace reads and mutations remain gated until resolved
pin handoff. Removed nodes become tombstones and cannot follow reused slots or
clusters. Failed writes, flushes, cleanup reads, device removal, and partial FAT
mirror updates retain the transaction and unresolved pin. No possibly live
namespace reference is followed by reclamation. VFS finishes a resolved pin
outside the operation lock using the existing superblock lifetime contract.
Power loss can leave orphan LFNs or unreclaimed storage, and a torn sector can
corrupt neighboring metadata; these pins provide runtime containment, not
post-reboot repair or crash atomicity.

The 64-bit production fixtures request 2,248 bytes for the common transaction
and 3,120 bytes for the retirement images: 5,368 retained bytes per file unlink,
or 171,776 across 32 pins. This excludes allocator overhead, VFS pin storage,
and retained nodes/superblocks. The ownership proof's independent 8 MiB budget
is released before the transaction is allocated; ordinary parent lookup still
uses its existing bounded cluster buffer and directory-walk limit. Resource
exhaustion fails before mutation. Cleanup performs linear per-cluster FAT I/O;
large operations may hold the mount lock for a long time. Sector batching and
remaining directory traversal limits require later scalability work.

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
used 5,372 peak requested heap bytes, retained 5,372 bytes before pin
finish, and requested 34 sector reads, 6 sector writes, and 3 flushes.
The controlled 2,049-cluster directory (2,098,176 mapped bytes) used 13,556
peak requested heap bytes and retained 13,556 bytes, with 12,351 reads and
2,050 writes. These figures exclude allocator overhead and VFS pin storage.
The 4,096-cluster inventory requests at most 16,384 bytes; for a three-sector
name set the retained FAT32 payload is at most 21,744 requested bytes by the
fixture's structure sizes, or 695,808 across 32 unresolved pins, excluding
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
size, preserving neighboring entries and other fields. Staged data/FAT changes
must flush before publication; publication must flush before freeing the old
chain. Reclamation and FSInfo writes must then flush before resolved handoff.
Definite cancellation flushes its fresh-allocation cleanup before completion.
Attempted/confirmed barriers are retained separately, without automatic retry.
Optional FSInfo locations and signatures are checked before mutation; valid hints are invalidated after
cleanup. Partial or failed writes quarantine the mount and retain both sector
images, chain inventories, phase, progress, and a discoverable VFS recovery
pin. The pin prevents normal unmount and retains the transaction after forced
device removal. A pending FAT or FSInfo sector write also retains complete
before and proposed sector images and its copy progress, so a torn metadata
write cannot erase the runtime evidence of neighboring fields. Ordinary FAT32
mutations use their recovery-aware paths; in-place data writes do not
provide atomic whole-file replacement. Conditional replacement provides runtime
containment, not crash atomicity or power-loss durability.
A torn FAT-sector write may damage neighboring allocation entries, including
an old file's chain; quarantine prevents further runtime mutation but cannot
guarantee that the old mapping remains readable until recovery.
At 512-byte clusters, two maximum 4,096-entry ledgers and the fixed transaction
retain at most 35,032 requested bytes, or 1,121,024 across 32 pins, excluding
allocator and VFS-object overhead. The independent ownership proof's 8 MiB
budget is temporary and released before transaction allocation. Scratch is at
most one 64 KiB cluster and is released before return/handoff, including on
uncertainty. Current measurements, barrier details and sync semantics are in
[FAT32 replacement and sync](../filesystem/fat32/replacement.md).

Safe FAT32/exFAT transactions coalesce a fresh allocation and its previous
fresh-cluster link when both entries share one FAT sector. Both cluster
identities and complete before/proposed images remain recoverable; bitmap,
publication, cleanup and flush ordering are unchanged. See
[metadata read/write measurements](../development/fat-metadata-performance.md)
for exact behavior, current performance comparisons and retained-memory limits.

Identity-bound I/O prevents an old mount from sending callbacks to a new
registration in a reused slot. It does not make ordinary mutations atomic:
allocation, FAT copies, FSInfo hints and directory sectors require separately
fallible writes. Recovery-aware FAT32 write/resize and exFAT write/truncate
retain transaction evidence on uncertainty. FAT32 namespace mutations use
pinned dispatch; direct legacy callbacks refuse mutation. A failed write or flush after
callback admission may have changed media. Revoking mount-local version
confidence prevents later driver operations from treating that state as
confirmed. Runtime pins do not repair persistent media after reboot.

GPT discovery creates child block instances. Filesystem probes publish
recognized type and native label as copied metadata. Current supported
filesystems are MGFS, FAT32, and exFAT; unknown or corrupt media remains a
visible block object without becoming mountable.

## exFAT ordinary and trusted owned creation

VFS also provides optional `create_owned_pinned` and `mkdir_owned_pinned` hooks.
They follow the same private-node, operation-reference and post-unlock pin handoff
as ordinary creation. Trusted kernel callers supply authorized owner/permissions;
VFS still checks read-only mounts and child-mount collisions under operation_lock.
Drivers without owned pin hooks retain their existing legacy dispatch.

exFAT routes ordinary and owned file/directory creation through its existing
exclusive-create engine. All return identity, allocation, publication images and
pin resources precede mutation. Empty files have no allocation; directories stage
one zeroed NoFatChain cluster with equal valid/data lengths and no dot entries.
Current access checks apply to ordinary calls; trusted owned calls retain their
existing explicit authorization semantics. Failure, detachment, public-cache
lifetime and resolved handoff follow the same VFS rules described above.

See [exFAT creation](../filesystem/exfat/README.md#ordinary-file-and-directory-creation)
for format, resource limits and runtime-versus-crash boundaries.

## FAT32 rename and directory moves

All ordinary, trusted and identity-checked rename dispatch uses the pinned
filesystem callback. Identity-checked ordinary dispatch verifies both parents'
write access; authorized dispatch preserves its explicit authorization scope.
Trusted rename still respects read-only mounts. Native move and its authorized
variant allow an existing
destination only when it identifies the source; the driver still scans the
complete destination namespace for collisions with other LFNs and short aliases.
Another destination identity returns `ALREADY_EXISTS`. Case-only renames retain
identity and preserve the requested spelling, using an LFN when uppercase 8.3
cannot represent it. An unchanged spelling is a validated no-op.

The mount operation lock covers current directory metadata, strict source and
destination name scans, the complete allocation-ownership proof, ancestry
validation, transaction admission and every mutation. Operation-local node
views use the explicitly retained superblock, so forced detach cannot redirect
admitted I/O. Source inode identity, contents, timestamps, attributes and
allocation survive movement. Namespace authorization follows the existing
VFS parent policy; preserved read-only file attributes still restrict content
writes independently. Confirmed completion changes its directory-slot
location and advances its generation, making earlier save expectations stale;
retained handles keep their identity and offset. Removed-slot tombstones are
not rebound.

The transaction is pinned before the first FAT or directory write. Placement,
Unicode validation, alias generation, initialized directory growth and end-marker
relocation reuse creation's mechanisms. A same-directory name can reuse the
validated source set when it fits; otherwise the complete destination is
published before source retirement. Source retirement preserves the original
images and separately records confirmed destination predecessors when sectors
are shared. No moved-file or moved-directory allocation is freed by rename.

Directory moves follow validated `..` links with a volume-derived bound and
reject movement into the directory itself or any descendant before mutation.
After destination publication, a retained sector image updates `..` to the new
parent (zero for the root); source retirement follows. Namespace access stays
gated through this intermediate state and resolved-pin handoff. All remaining
name slots, required FSInfo updates and identity-bound flushes must complete
before success. Failed or torn writes, flush failures and interrupted cleanup
keep the transaction and pin discoverable and the mount quarantined. A possibly
linked growth cluster cannot be cancelled as private storage.

A crash can leave prepared or orphaned LFNs, duplicate source/destination
references, a `..` relationship inconsistent with a surviving name, unreclaimed
growth allocations, or neighboring metadata damaged by a torn sector. External
repair may be required; neither ordered flushes nor runtime pins provide atomic
rename, post-reboot recovery or power-loss durability. Independent checker
acceptance of successful moves establishes interoperability only.

Rename has no file-content or mapped-file deletion ceiling. Its proof retains
the independent 8 MiB memory budget. Directory placement still has the documented
4,096-cluster walk and 65,536-slot growth limits; alias selection has 99
candidates, unsupported OEM aliases refuse namespace mutation, and strict
end-tail validation remains required. These restrictions need later scalability
and imported-layout work, not silent weakening of ownership validation.
Requested transaction memory and verification boundaries are recorded in
[FAT metadata performance](../development/fat-metadata-performance.md#fat32-rename-resources).

## FAT32 ordinary writing and resizing

All ordinary VFS dispatch selects the pinned FAT32 write/resize callbacks.
Legacy direct write returns zero and direct truncate returns unsupported without
mutation. The native truncate interface still means truncate-to-empty;
`vfs_resize_handle` is internal and supports arbitrary shrink/grow lengths
without changing that ABI or the handle offset. Ordinary FAT32 contents are
bounded by its 32-bit file-size field and available allocation, not the separate
2 MiB safe replacement/creation contract.

Admission validates the retained live identity, short-entry location, current
mapping/size, read-only attribute, device identity and version confidence under
operation_lock. A complete scalable namespace ownership proof, or a still-valid
ordinary-operation certificate, must establish unique ownership before mutation.
The complete allocation is counted, including clusters past logical EOF. A
confirmed per-file count/tail summary is reusable only with that certificate and
matching generation. No partial proof can authorize an operation.

The transaction and recovery pin exist before the first persistent mutation.
It retains original/proposed directory images, the pending data and FAT/FSInfo
sector images, FAT-copy progress, the old head/count/tail, a private fresh chain,
and a separate allocation candidate recorded before its FAT update. Fresh
clusters are zero-initialized before linkage; hole bytes and newly exposed bytes
are zero-filled. Initialization and data writes are flushed before the old tail
is linked or a new directory mapping/size is published. An empty file acquires
its first cluster through directory publication. Every FAT update checks all
configured authoritative copies and preserves unrelated entries and high bits.
The publication image is refreshed and checked against the admitted directory
sector after earlier writes.

Shrinking publishes and flushes the smaller size/mapping before cutting the
chain and reclaiming its tail. Truncate-to-zero publishes first-cluster zero
before freeing the former allocation. Streaming reclamation retains each
validated successor before clearing FAT copies, advancing only after all copies
confirm. No whole-file buffer or file-sized allocation ledger is retained.

A read failure or allocation exhaustion can yield a confirmed write prefix only
when its data, mapping, metadata and flushes are all confirmed. VFS returns that
prefix and advances the offset by its count. At most one fully initialized extra
cluster can remain reachable beyond the prefix's EOF; later resizing reclaims it.
Failure before any user byte is confirmed cancels only private fresh storage;
hidden zero-fill beyond the original EOF does not expose new contents. A failed
or torn data/FAT/directory write, failed flush, device loss after mutation, or
uncertain cancellation/cleanup reports uncertainty with no confirmed count.
The transaction, candidate, cleanup successor, pending images and pin remain
discoverable; no ambiguous write is retried. Mutation admission remains closed
until resolved pin handoff outside operation_lock. Handle serialization uses
`offset_lock` before operation_lock, and no VFS metadata spinlock spans I/O.
Confirmed ordinary changes advance the file generation once and publish cached
size/mapping coherently; they do not refresh save expectations.

On 64-bit builds, ordinary recovery requests 2,248 common bytes plus a 1,152-byte
operation record: 3,400 bytes per pin, or 108,800 for 32 unresolved operations.
The registry adds a 56-byte pin per operation, and retained nodes/superblocks and
allocator overhead are separate. No new proportional transaction allocation is
needed; the ownership proof has its independent 8 MiB budget and is freed before
transaction allocation. Each node's mapping summary adds 16 bytes. Small writes
reuse a valid proof and allocate only the transaction records and registry pin;
they do not allocate a maximum-volume bitmap each time.

These are runtime containment rules. In-place writes can overwrite old bytes
before a later failure. Power loss can leave orphan fresh chains, allocation
past EOF, unequal FAT copies, or a shortened file with unreclaimed storage;
a torn directory sector can damage neighboring entries. Flushes do not make
those changes atomic, and runtime pins provide neither post-reboot repair nor
power-loss durability. Proof budgets, directory walk limits, per-sector data
I/O and per-cluster cleanup remain explicit scalability limitations.

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

## Device snapshot publication

Native device inspection has one sleepable FIFO gate covering refresh, disk
presentation-number assignment, generation publication and page copy. No VFS or
block/management lock is held on entry. Filesystem identification can wait for
block I/O; short registry/VFS locks are released before blocking, and discovery,
removal, management and event publication do not acquire the inspection gate.
Only the native snapshot API calls its private refresh helper. Native syscall
lifetime protection prevents external termination from abandoning an owning
kernel frame; failed mutex admission returns BUSY without changing outputs.

A generation is published only after its refresh completes. Another caller's
refresh invalidates old pagination tokens with RETRY. Block enumeration is
captured before probes, and retired/replaced instances or dead parents are
omitted after probing. An inconsistent capture returns RETRY without modifying
the previous generation. An ordinary unrecognized volume remains present without a detected filesystem;
system-role hints retain their existing meaning and never authorize a mount. Hardware itself
is not frozen by this lock: later operations still revalidate live immutable IDs.
The bounded presentation map never reuses an old number; exhaustion reports zero.

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

All native formatter selections (`fat32`, `mgfs`, `exfat`) and the exported
exFAT formatting entry enforce shared, identity-bound admission. Native requests
pass their saved registration ID into `storage_format_filesystem_for_id()`.
Direct exFAT callers use `exfat_format_device_for_id()`. Both require the
caller's saved ID; pointer-only destructive entries have been removed because
a reusable descriptor alone cannot identify an earlier selection.
The raw implementations are private callbacks, not exported destructive entries.

Admission takes the nonblocking management guard and block quiescence gate,
revalidates the selected registration, checks logical read-only policy, and
reserves an exclusive private VFS recovery owner before any formatter I/O.
The saved physical range excludes mounted devices, mounted overlapping partitions,
private owners and unresolved pins. Private admission tries the mount mutex
without waiting in the reverse order of mount -> block I/O. Registry, partition
and VFS metadata locks never span formatter I/O. The private operation mutex
protects formatting state; it is released before recovery-pin handoff.

A shared range preflight runs under management admission before formatter I/O.
Whole-device formatting is refused while child partitions remain registered,
including retained retiring registrations. Partition formatting allows its
containing parent registration and disjoint siblings, but refuses overlapping
siblings. Unknown or invalid topology is refused conservatively. Conflicts
return `BUSY`, stale selected identities return `DEVICE_GONE`, read-only policy
returns `ACCESS_DENIED`, and unsupported physical mappings return `UNSUPPORTED`.
Rejected requests do not write media, retire registrations, invalidate caches,
or alter recovery ownership. FAT32/exFAT private label admission checks the same
volume scope; MGFS checks its planned header range. All label paths acquire a
private owner covering the selected volume before loading mutable metadata.
MGFS retains before/proposed/readback superblocks and confirms its one write
through identity-bound flush and exact readback, including clearing. See
[MGFS label recovery](../filesystem/mgfs/labels.md).

The block cache keys bytes by the physical parent's immutable registration ID,
sector size and checked absolute sector range. Parent/child and descriptor-copy
reads can reuse those bytes. Every attempted write invalidates all overlapping
physical cached ranges before the callback, including failed or partial writes;
disjoint ranges remain cached. Resolution, lookup, insertion and invalidation
run under the existing recursive block gate, with registry and partition locks
released before callbacks. Device loss rejects accesses even on cache hits.
GPT children retain the selected parent ID; USB discovery before parent
publication binds pending children once, during parent registration under the
gate. Reuse of the parent descriptor cannot retarget an old child.
Registration rejects an already registered descriptor or a second physical
root with the same non-null driver backing, including a retiring root. Current
physical drivers provide that stable backing identity. Arbitrary opaque alias
backends without declared topology are outside the supported registration model.

GPT initialization checks the MBR and both complete metadata-copy plans before
its first write. Creation/deletion check both arrays and headers before either
copy is changed. Overlap with another registered range is refused, including
compact imported tables whose old partition data occupies the expanded array
locations. The pinned baseline fault regression demonstrates the old partial
write and stale child cache; the current path returns `BUSY` without writing.
Native GPT mutations preserve both selected parent and child IDs through their
identity-bound kernel entries. Pointer conveniences capture IDs before admission.

These changes add 512 static cache bytes (64 entries of 4,144 bytes, previously
4,136) and 128 static partition-mapping bytes (16 mappings of 136 bytes,
previously 128). Physical resolution and range preflight allocate no heap,
perform no media I/O, and use bounded registry scans. The formatter measurements
below retain the same driver request counts and peak heap after admission.
Coordinated registration retirement remains separate future storage engineering;
this contract refuses overlapping management writes rather than providing
unrestricted repartitioning. Guest-level aliasing tests remain pending.

Geometry comes from the admitted snapshot. Reads, writes and final flush use
the original device pointer with the immutable expected ID. Identity is checked
again before the writer and around completion probes; probe iteration stops
when a registration disappears. Rejected stale selections return `DEVICE_GONE`
without touching a replacement. Native automount suppression also uses the
saved ID, rather than a reusable pointer's current ID. Native authorization and
the request ABI are unchanged; trusted kernel callers do not acquire a native
user authorization token through these APIs.

Before any attempted write, allocation, geometry or admission failure cancels
the private owner without media mutation. Once a destructive write is attempted,
a false callback may have copied bytes. Failed writes, subsequent reads, final
flush, verification or ownership handoff must not report successful formatting.
Uncertain operations retain the saved scope pin and private state, preventing
conflicting mount, label, GPT and format work. The retained state records the
filesystem, admitted geometry/ID, progress phase, underlying failure, attempted
write count and the complete last attempted write image (up to 12 sectors).
There is no blind retry or automatic rollback. Confirmed completion requires the
final identity-bound flush, matching filesystem probe and resolved owner handoff.

At 70,000 sectors of 512 bytes, controlled production fixtures measured:

| Formatter | Read requests | Write requests | Flushes | Allocations | Peak requested heap |
| --- | ---: | ---: | ---: | ---: | ---: |
| FAT32 | 2 | 144 | 1 | 3 | 6,536 bytes |
| MGFS | 1 | 31 | 1 | 3 | 6,536 bytes |
| exFAT | 59 | 42 | 1 | 8 | 18,516 bytes |

These are driver callback requests, not sector counts or measured physical I/O.
The fixture's MGFS completion probe is a signature/CRC dependency; FAT32/exFAT
probes execute production code. An unresolved owner retains 6,536 requested
bytes including its context, superblock and pin, excluding allocator overhead
and block/controller state. At the 32-pin limit this is 209,152 bytes. The
successful path adds three bounded ownership allocations and copies each write
into its retained image, without a file-ownership scan or full-volume readback.
The exFAT entry no longer performs a redundant second flush/probe outside shared
completion. Native signing/build verification does not establish device durability.

Formatting is intentionally destructive. The last-image evidence does not
contain the previous volume or a rollback journal. Runtime exclusion and flush
ordering do not establish crash atomicity, power-loss durability, or persistent
post-reboot recovery. Integer formatting results distinguish admission failure,
confirmed no-write failure and uncertainty after destructive attempts.

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
`block_unregister_by_id()` retires first, then tries quiescence. Active or
unresolved recovery pins and pending mount reservations retain their selected
registration and physical parent. Final unregister checks these saved identities
under the VFS metadata lock, outside the block registry lock. On false, callers
must preserve all backing resources. On true, the drained, unowned descriptor is
removed and its cache invalidated. Pin publication checks both selected and
physical-parent liveness under that same VFS lock: once retirement marks either
identity offline, no late pin can cross final teardown. Completion can remove an
owner concurrently; a conservative refusal can then be retried.

Retained offline registrations occupy finite slots and preserve the existing
duplicate-physical-backing check. Known GPT aliases resolve to the saved parent
identity, so overlapping views cannot acquire a new identity by removing a child
or parent. Disjoint sibling ranges remain independent. This uses declared
partition topology and stable non-null physical driver backing; arbitrary
unrelated backends are not assumed to have detectable media equivalence.
Exhaustion never frees unresolved owners automatically.

Raw registry pointers are not lifetime leases; I/O
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

## Pending mount admission

VFS serializes mount candidates and reserves the candidate's immutable device
identity and physical range before driver validation begins. Reservation admission
coordinates with the block quiescence gate; conflicting management or an active
owner fails admission. Management preflight includes pending ranges alongside
active mounts and retained recovery pins. Publication transfers exclusion to the
active mount under the metadata lock; every failure releases the reservation.
A device identity change aborts publication rather than redirecting admission I/O.

The internal history mutation hook may return a resolved recovery pin using the
same explicit ownership contract as ordinary pinned writes. VFS releases the
operation lock before completing handoff, retains the handle's superblock reference
through completion, and preserves uncertain results. The native history ABI is
unchanged. This mechanism does not add persistent recovery or atomic history writes.
