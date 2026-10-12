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
layouts. Replacement requires the primary and stream entries to share one
512-byte directory sector; unchanged filename entries may occupy later sectors.
Exclusive creation requires its complete entry set to fit in one 512-byte
directory sector. If the FAT-linked root directory has no suitable existing
run, creation can relocate a zero-filled end marker into the next sector and
append one initialized root cluster when necessary. The new entry sector is
staged and flushed before the root tail link; the old end marker is retired
last. Root growth retains the candidate allocation, FAT and directory-sector
images, write progress, and a recovery pin across uncertain outcomes. This
path currently requires a following end-marker slot in the entry sector and
rejects nonzero bytes after the old end marker. Non-root FAT-linked and
NoFatChain directory growth remain unsupported: they also require a
checksummed parent stream-length update or a bounded mapping conversion.
Entry sets spanning directory sectors remain unsupported. Recovery pins retain
uncertain transaction state, and version-confidence loss prevents unsafe
follow-up saves. These mechanisms
do not establish crash atomicity or power-loss durability.

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
