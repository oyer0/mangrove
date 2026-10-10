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

MGFS saves its registration ID at mount and supplies it for every filesystem
block read and write, including grouped file reads. The block layer checks the
expected ID again after acquiring its I/O gate, so a retired or reused registry
slot cannot redirect an old MGFS instance to replacement media. A callback
admitted before retirement may finish while removal begins; unregister cannot
reclaim backing until the gate drains. An operation that observes retirement
reports failure rather than a successful read or write.

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

VFS also has internal recovery pins for future MGFS replacement work. Admission
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
state, and future MGFS recovery payloads may have much larger independent
memory costs. Capacity can remain occupied until an explicit retirement path
is approved.

MGFS has private staging-and-cancellation and publication exercises for the
future conditional replacement operation. They accept an exact-length,
block-streamed source for an existing MGFS 1.2 regular file, validate its
generation and physical ownership
under the mount operation lock, and obtain a recovery pin before persistent
mutation. The initial replacement limit is 128 data blocks (512 KiB): even one
extent per new block fits the two inline extent slots and one 126-entry list.
The transaction reserves a fixed 129-entry allocation ledger (data plus list)
and two complete 4 KiB table images before mutation. Allocation candidates are
recorded before bitmap writes; staged data and list blocks are always fresh.
The original record and data remain untouched during staging. The cancellation
exercise reclaims all staged allocations and restores the original superblock
state. The publication exercise first inventories at most 128 old mapped data
blocks and at most 128 old extent-list blocks; a valid 128-extent record may
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
The ledger bounds block bookkeeping,
while the mount-owned transaction is bounded by 32 KiB and remains in memory
with an unresolved pin. Staging scans up to the entire data-area bitmap for
free blocks and adds one bitmap write per allocation and reclamation; the
ownership preflight also scans allocated records and their mappings. This is
runtime containment, not crash atomicity or power-loss durability. Host tests
exercise the private boundaries; neither is a VFS save API. Existing VFS node
size caches are not refreshed by the private publication exercise, so it must
not be exposed to ordinary callers before that integration is designed.

Mangrove will standardize on MGFS 1.2. Removing legacy 1.1 support is a
separate cleanup requiring an audit of bootloader, kernel, filesystem tools,
development images, tests, and documentation. Development images must be
recreated safely for 1.2 instead of silently reused.

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

The private `mgfs_ownership_preflight` is prepared for the future replacement
path; no current filesystem operation calls it. Its caller must hold the mounted
superblock's operation lock for the entire check and subsequent staging. It
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
