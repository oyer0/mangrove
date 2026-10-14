# Mangrove File System (MGFS)

MGFS is Mangrove's native writable filesystem. System images place it in the
GPT partition named `MANGROVE_ROOT`; the UEFI loader locates that role
partition and loads `/boot/pith.elf` from its MGFS root. MGFS can also be used
on removable partitions and on unpartitioned whole devices.

The current on-disk format is major version 1, minor version 2. Its magic is
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
permissions in their File Record flags. Directories additionally carry a
native child-mutation policy. Regular files up to 56 bytes may keep
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

Ordinary positional writes preserve every unaffected existing byte and set
length to the larger of the previous length and the written end offset. They
do not truncate, convert existing extent-backed files to inline storage, or
release owned trailing data/list blocks merely because the supplied bytes end
earlier. Zero-length writes leave length and generation unchanged. Writes may
start at or before EOF; seeking farther is supported by VFS, but a subsequent
MGFS sparse write is rejected with no confirmed bytes. Nonzero arbitrary
`vfs_resize_handle()` is unsupported; explicit truncate-to-empty and staged
conditional replacement remain separate operations.

An overwrite reuses its current mapping after the existing Record/extent-list
checks, including linked lists and allocation past EOF. Only data blocks
intersecting the requested range are read/modified; suffix data and unchanged mapping blocks are not
rewritten or reclaimed. Existing checksum and mapping reads remain necessary.
Ordinary growth preserves the regular-file physical-order rule enforced by
kernel/boot readers and ownership checks: new data blocks must be at or above
the previous physical tail. Lower free holes cannot extend that mapping, and
there is no automatic relocation. Growth requiring a mapping rewrite remains
bounded by the existing two inline extents plus one extent-list block;
overwrites of larger already valid linked mappings are supported. If a growth
attempt exceeds these bounds after allocating storage, mutation quarantine
retains the affected filesystem state; it is not a successful write.

Shoot's locked history merge/reset/trim uses a private, explicit exact-length
whole-content update. It retains its existing in-place failure/quarantine
contract and never obtains trimming by truncating before a write. This callback
cannot use staged conditional replacement directly: it has no recovery-pin
output for the required post-unlock handoff. Native safe-file replacement keeps
its separate staging, generation and recovery-pin interface. No ABI or disk
format change is required by these distinctions.

Ordinary legacy writes still return byte counts, with zero confirmed bytes on
failure. Possible data, allocation or metadata mutation followed by failure
quarantines further mutations, including failed reads after a data write and
before generation publication. The VFS byte-count interface does not provide
the pinned write hook's detailed error classification. Neither ordinary nor
private history writes promise rollback, crash atomicity or power-loss durability.

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

## Directory creation and mutation policy

Creation defaults are fixed VFS policy, not inherited directory metadata. A
normal file is owned by the creating UID and starts with `rw:r-`; a normal
directory is owned by the creating UID and starts with `rw:r-`. Explicit
trusted creation paths may choose other values, such as system-owned image
objects or an administrator creating on behalf of a regular user. Existing
children are never changed by a parent's policy.

MGFS v1.2 stores one directory-only child-mutation policy bit. `open` permits
any caller who can write the directory to delete or rename an existing child.
`owner-restricted` additionally requires ownership of the child or containing
directory. This is a native mutation rule, not a Unix mode, umask, ACL, or
sticky bit.

The system image gives human home directories private `rw:--` directory
permissions. `/temp` is system-owned with shared `rw:rw` directory
permissions and owner-restricted child mutation; children still use the
normal `rw:r-` file default and retain their creating owner. Historically,
v1.1 images were upgraded without renaming or merging existing directory names;
an existing `/temp` received this policy, while other existing directories
retained their names and used open child mutation unless a trusted image policy
set the bit. Current tools require MGFS 1.2 and do not perform that upgrade.
