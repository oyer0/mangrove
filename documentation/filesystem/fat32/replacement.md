# FAT32 conditional replacement and sync

Conditional replacement operates on a retained mount-local file identity and
generation under the mount operation lock. VFS checks handle write rights and
current permissions; the driver checks read-only mount policy, current entry
and generation, FAT mirrors and complete reachable allocation ownership before
mutation. The shared ownership proof has an independent 8 MiB requested-memory
budget and expires with its protected use. No extra namespace scan is added for
flush ordering. Complete syscall input is secured before the filesystem runs.

## Publication and completion

The driver preallocates its recovery transaction, original/proposed directory
sector, allocation ledgers and scratch storage, validates FSInfo locations,
then acquires a recovery pin before persistent mutation. Fresh allocation
candidates are recorded before any potentially mutating FAT-copy update.
All operations remain bound to the registration captured at mount.

| Boundary | Required confirmation | Storage that remains protected |
| --- | --- | --- |
| Staging | Flush complete fresh contents and every configured FAT update | Entire old allocation and directory entry |
| Publication | One prepared directory-sector write, followed by a flush | Entire old allocation until this flush confirms |
| Cleanup | Reclaim old clusters, invalidate affected FSInfo hints, then flush | Fresh allocation remains file-owned; progress remains pinned |
| Handoff | All required checkpoints and cleanup cursors complete | Registry retains mount and transaction until callback completion |

Every required barrier uses `block_flush_for_id()`. Attempted and confirmed
checkpoint masks are retained separately; failed barriers are not retried.
An empty replacement has no staged storage to flush. If no reclamation or
FSInfo write follows publication, its confirmed publication flush also confirms
final completion. Ordinary nonempty-to-nonempty replacement uses three flushes.
No additional flush is issued by the resolved handoff callback.

The live node's mapping, size and generation change after the publication
write confirms, even if its subsequent flush or cleanup fails. Version
confidence remains gated until successful handoff; the native caller receives
UNCERTAIN and retains its prior save expectation on failure. Old clusters are
never freed before confirmed publication flush. Uncertain publication retains
both old and fresh allocations and directory images; uncertain reclamation
retains ledgers, FAT-mirror progress, pending sector images and cleanup cursors.
The pin and filesystem state survive forced detach/removal. No automatic
retry, rollback of ambiguous writes or recovery by sync is provided.

A definite source/read or no-space failure before publication can cancel only
confirmed fresh allocations. Cancellation must flush its FAT changes before a
resolved pin can be returned. When nothing was allocated or written, it needs
no flush. Failed cancellation writes or flushes retain uncertainty. Recovery
handoff runs after operation_lock is released, and the callback verifies the
recorded checkpoints before freeing evidence. Removal after every required
write/flush has already confirmed does not require new device I/O to retire a
resolved pin; removal during a required barrier cannot complete successfully.

## Sync and backend guarantees

The FAT32 superblock sync callback runs under the retained mount's operation
lock, including clean VFS unmount/eject. It refuses attached transactions or
revoked version confidence, then flushes only the saved device registration.
Missing/replaced devices fail without redirecting I/O. A failed admitted flush
revokes version and ownership confidence and returns an error; VFS aborts clean
detach and releases its temporary references. Sync allocates no heap, performs
no namespace scan and writes no filesystem metadata. Read-only mounts may
flush; doing so does not authorize any data, metadata or allocation mutation.

The block layer serializes flush with admitted I/O and checks registration
identity before and after the backend callback. Backends without an explicit
flush use their synchronous-write completion contract. Success confirms that
backend contract, not atomic directory sectors or verified power-loss durability.
A failed flush may already have completed backend work.

Ordered barriers protect runtime allocation ownership. They do not make FAT
mirrors, directory publication or FSInfo changes crash atomic. Power loss before
publication can leave fresh orphan allocation; interrupted cleanup can leave
old orphan allocation. Torn sectors can damage neighboring entries or FAT
links. In-memory pins do not survive reboot or repair these states; independent
checking/repair may be needed. A checker-clean image after a failed flush is
still an uncertain runtime outcome, not evidence permitting automatic retry.

## Resources and current limits

The shared fixed transaction requests 2,264 bytes on x86-64, including eight
additional bytes for replacement completion evidence. Old/fresh ledgers use
four bytes per cluster. Both inventories are proportional to the complete
mapping and checked against mounted geometry before allocation. Content size
also obeys FAT32's 32-bit on-disk byte-size field. Retained memory therefore
depends on cluster size, fragmentation and file length; it excludes VFS/pin
objects, allocator overhead, stack and syscall input buffers. Scratch is released before
return/handoff, including on uncertainty. The largest scratch allocation is
one 64 KiB cluster; retained recovery metadata does not include it.

A fragmented 2 MiB old/new fixture at 512-byte clusters measured 35,544 peak
requested bytes, 35,032 retained bytes, 16,712 sector reads, 12,353 writes and
three flushes, approximately 10 ms host CPU under ASan/UBSan. A smaller
700-byte replacement with fragmented old/new mappings and FSInfo copies used
2,280 retained bytes, 4,624 peak bytes including preflight, 19–46 reads,
9–21 writes and three flushes across one through four FAT copies. Conditional
cancellation uses one flush after fresh allocation cleanup. Sync uses one
flush and no allocation; unresolved transactions cause no sync I/O.

These are controlled host requests, not guest/hardware latency or durability
bounds. At 64 KiB clusters, transaction plus old/new ledgers and scratch can
request 68,056 bytes; temporary ownership-proof memory is separate and released
before transaction allocation. Smaller operations allocate proportional ledgers.
The ownership-proof budget and directory traversal bounds remain independent
scalability limits; content and mapped-old inventory no longer have a 2 MiB cap. Names and imported
alias policy are documented in [names.md](names.md); verification is described
in [development tests](../../development/tests.md#fat32-replacement-flush-and-sync).
