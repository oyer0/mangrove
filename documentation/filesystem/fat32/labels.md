# FAT32 volume-label transactions

FAT32 labels use an ASCII root-directory Volume ID entry and the boot record's
11-byte space-padded label field. Mangrove accepts its documented printable
ASCII subset, up to 11 bytes, preserving case. Trailing-space and all-space
requests are rejected because padding cannot preserve that spelling. The
case-insensitive `NO NAME` sentinel is reserved; empty input explicitly clears
the label. Invalid input returns BAD_ARGUMENT before loading or changing media.

The public kernel label function returns a VFS status, including UNCERTAIN.
Native syscall signatures are unchanged. The native request's immutable public
instance ID is decoded and passed through identity-bound storage/driver entry
points; slot reuse between authorization and admission fails before filesystem
loading. Trusted kernel wrappers capture the current instance, while callers
retaining an earlier identity use the explicit `_for_id` form. Storage management delegates FAT32
label operations directly to this function and performs no generic flush or
readback after its recovery owner has been released.

## Admission and ownership

Every call acquires the storage-management quiescence guard, then an exclusive
private VFS recovery owner before reading filesystem metadata. This is not a
namespace mount. Its saved registration identity and physical range exclude
conflicting mounts, formats, partition operations and other owners, including
overlapping partition views. A retained unresolved pin continues that exclusion
after the caller releases its reference. I/O always uses the original device
identity; removal or re-registration cannot redirect it. Private admission
tries the mount lock without waiting while holding I/O quiescence: a competing
mount may already hold that lock while waiting for I/O. Contention returns BUSY
before filesystem loading; it cannot form an inverted blocking lock order.

Admission validates geometry, the complete reachable allocation ownership,
mirrored FAT values, the root namespace and optional backup boot record.
Duplicate/malformed label records, orphan LFNs, hidden records after an end
marker and incomplete scans fail before mutation. Existing file LFNs and short
aliases are protected by the normal Unicode comparison and conservative OEM
ambiguity policy. A colliding requested label or its portable short spelling
returns ALREADY_EXISTS. Read-only devices and mounted owners are refused.

The primary boot sector is always retained. A declared backup must lie in the
reserved area and have a valid, consistent BPB; its unrelated bytes are
preserved. Absent backup declarations do not require a fabricated copy.
`fsck.fat` recommends adding a backup to the tested absent-backup layout;
Mangrove preserves that declaration and the tests apply no repair.

## Publication and failure handling

Before mutation the operation prepares the directory sector, both applicable
boot-sector image pairs, all growth/allocation evidence, optional FSInfo state
and verification scratch storage. Only the affected existing directory sector
is written. Replacing a label preserves its other record fields; clearing it
retires the entry. Creating a label reuses a validated free slot or the existing
FAT32 directory-growth mechanism.

Growth records candidates before mirrored FAT writes, initializes fresh storage,
flushes it, links it and confirms the link's flush. A possibly successful tail
link makes the appended cluster directory-owned; cancellation cannot free it.
Earlier end markers retain their original/proposed images and publication order.
The label directory write and any marker relocation are flushed before boot
labels are updated. Growth invalidates FSInfo hints through the shared recovery
mechanism. A final identity-bound flush confirms boot and FSInfo completion.

Readback verifies the complete root label state and each complete proposed boot
sector. Clearing requires absence of every live root label and the unlabeled
boot fields; it never skips verification because the requested string is empty.
Only confirmed verification permits resolved-pin handoff outside operation_lock.
The callback checks completion evidence, frees the transaction, then VFS drops
the pin reference. Private filesystem state is freed after the caller reference.

A definite failure before publication can cancel only a confirmed, unlinked
fresh allocation, with cleanup and flush confirmation. Failed/torn writes,
failed flushes, readback failure after mutation, removal and failed handoff
retain images, exact attempted/completed progress, allocation ownership and the
registry pin. They return UNCERTAIN and block conflicting operations. No
ambiguous mutation is retried, restored or treated as proof of label absence.

## Resources and persistence limits

On x86-64 the label context requests 4,368 bytes, including the shared 2,264-byte
transaction. Root growth adds 9,368 bytes and optional FSInfo image pairs add
2,048 bytes, with attempt progress retained by the shared FSInfo cursor/pending
record. Including the private superblock,
registry pin, filesystem state, root node and identity, retained memory is at
most 16,336 bytes per pin, or 522,752 bytes for 32 pins. Allocator overhead and
stack are excluded. Scratch is at most one 64 KiB cluster and is released before
return or handoff, including uncertainty. The independent ownership proof has
its existing 8 MiB temporary budget and is freed before growth preparation.

A 512-byte-cluster fixture measured 13,656 peak requested bytes without growth,
18 sector reads, three writes and two flushes for replacement. Full-root growth
measured 16,848 peak bytes, 33–41 reads, ten writes and four flushes. These are
host-driver requests, not physical-device latency. Metadata replacement adds
no full-volume allocation count scan: one namespace ownership proof and bounded
root placement/verification walks remain necessary. Existing root traversal and
growth limits apply. The measured optimized label frame is 6,080 bytes; that is
not a complete syscall/IRQ call-stack measurement.

Runtime evidence is not a persistent journal. Power loss can leave differing
boot/directory labels, fresh orphan allocation or interrupted directory growth.
Torn writes can damage neighboring entries or FAT links. Independent checking
and repair may be required. Ordered flushes and successful checker/readback
results do not establish atomicity, post-reboot recovery or power-loss durability.
