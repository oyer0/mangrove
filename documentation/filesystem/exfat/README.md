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
cluster fields, and lengths. Allocation-bitmap, up-case-table, and volume-label
entries are filesystem metadata and are not exposed as ordinary files.

## Names, attributes, and labels

Path components are converted between UTF-8 and exFAT UTF-16, including valid
surrogate pairs. Invalid encodings and exFAT-forbidden characters are
rejected. Lookup is case-insensitive through the volume's validated up-case
table, while directory enumeration preserves stored casing. Names that are
equivalent under the up-case mapping cannot coexist.

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
