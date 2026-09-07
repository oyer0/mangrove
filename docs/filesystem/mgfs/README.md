# Mangrove File System (MGFS)

MGFS is Mangrove's native writable filesystem. System images place it in the
GPT partition named `MANGROVE_ROOT`; the UEFI loader locates that role
partition and loads `/boot/pith.elf` from its MGFS root. MGFS can also be used
on removable partitions and on unpartitioned whole devices.

The current on-disk format is major version 1, minor version 1. Its magic is
`MGFSv1\0\0`, its filesystem block size is 4096 bytes, and it currently
requires a block device with 512-byte logical sectors. The filesystem uses
UTF-8 names, permanent filesystem-local Record IDs, extent-based file data,
and CRC-64 checksums for metadata.

## Object model

Path lookup follows this chain:

```text
path component -> directory entry -> Record ID -> File Record -> extents
```

A Record ID is immutable and is not reused within a filesystem. Renaming or
moving an object changes directory entries while retaining the same Record ID
and File Record. Paths are the application-facing identity; Record IDs remain
an internal filesystem mechanism.

Files and directories carry a 32-bit owner UID and owner/other read/write
permissions in their File Record flags. Regular files up to 56 bytes may keep
their contents inline in the File Record. Larger files, directory streams, and
extent-list metadata occupy blocks in the data area.

Names are case-sensitive UTF-8 byte strings. MGFS performs no Unicode
normalization. A name cannot be empty, contain NUL or `/`, equal `.` or `..`,
or exceed 255 bytes. Directories do not store `.` or `..` entries.

## Supported operations

The kernel driver supports mounting, lookup, directory enumeration, file
reads and writes, extension, truncation, file and directory creation, file
deletion, empty-directory deletion, and rename or move within one MGFS
filesystem. The VFS applies owner/other access checks before filesystem
operations.

MGFS has no journal. Metadata mutations set `NEEDS_FSCK` before publishing
changes and restore `CLEAN` only after the operation's required writes
complete. Metadata write ordering is designed to leave incomplete operations
detectable, but compound operations are not power-loss atomic. `mgfsck`
validates checksums, allocation ownership, Record IDs, directory structure,
and reachability; it is not an automatic runtime repair path.

## Format and tools

The exact binary layout, checksums, formatter state, and write ordering are
specified in [format.md](format.md). Volume-label storage and validation are
specified in [labels.md](labels.md).

The host tools are:

- `mkmgfs`, which creates a deterministic filesystem from explicit size,
  UUID, timestamp, and optional label inputs;
- `mgfsck`, which checks an existing MGFS image;
- `populate_mgfs.py` and `update_mgfs.py`, which populate or update Mangrove
  image payloads without redefining the on-disk format.

The runtime formatter in `kernel/src/storage/format.c`, the kernel driver in
`kernel/src/storage/mgfs.c`, and the host tools use the same current layout
constants and invariants.
