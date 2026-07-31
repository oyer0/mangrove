# Mangrove File System (MGFS) v1

## Status and goals

This is the proposed on-disk specification for MGFS v1, not kernel code. MGFS will become the root filesystem on TestDisk.img. Mangrove.img remains the rebuilt FAT32 EFI System Partition.

MGFS is a modern, extent-based filesystem with UTF-8 names, 64-bit addresses and counters, permanent Record IDs, and checksummed metadata. Its identity model is:

    path component -> directory entry (name, Record ID) -> File Record -> extents -> data blocks

A pathname is only lookup. A Record ID is a file's permanent internal identity within one MGFS volume. Rename and move change directory entries only. Applications use paths and never see Record IDs.

The filesystem UUID is the only UUID in MGFS v1. It identifies the filesystem instance, not individual files.

v1 excludes journaling, copy-on-write, compression, snapshots, hard links, symbolic links, permissions, ACLs, extended attributes, encryption, and deduplication.

## 1. Disk layout

MGFS occupies a complete block device; v1 defines no partition scheme. Every on-disk block address is a 64-bit filesystem-block number relative to the device start. The filesystem block size is fixed at 4096 bytes. The block layer translates this to native sectors (eight 512-byte sectors on the current AHCI test disk).

The formatter creates these contiguous, immutable regions:

| Region | Purpose |
| --- | --- |
| Block 0: superblock | Identifies the filesystem and describes every region. |
| Allocation bitmap | One bit per data-area block; authoritative free-space map. |
| File Record bitmap | One bit per fixed File Record slot; authoritative record allocator. |
| File Record table | Durable records for files and directories. |
| Data area | File data, directory-entry blocks, and chained extent-list blocks. |

Static regions make mount and inspection simple: only the superblock has a fixed location, and it describes all variable-sized regions. The root directory is an ordinary File Record whose ID is stored in the superblock.

Only data-area blocks occur in the allocation bitmap. File Record slots occur only in the File Record bitmap. A set bit means allocated. Empty files and directories have no extents and allocate no data blocks.

## 2. On-disk structures

All multi-byte numeric fields are unsigned 64-bit little-endian unless explicitly a byte array. This includes addresses, sizes, counts, versions, timestamps, flags, checksums, and Record IDs. The filesystem UUID is stored as 16 raw RFC 4122 bytes.

MGFS v1 uses CRC-64/ECMA-182. For calculation, the protected structure's checksum field is zero. A metadata checksum mismatch is corruption and must fail mount or operation; v1 has no repair-in-place behavior. Reserved bytes are written as zero and ignored by readers.

### Superblock

The superblock occupies block 0. It has a declared header length; the rest of the 4096-byte block is reserved and zeroed.

| Field | Meaning |
| --- | --- |
| magic[8] | ASCII MGFSv1 followed by two NUL bytes. |
| format_major | 1. A different major version is incompatible. |
| format_minor | 0. A higher minor version is allowed only if all incompatible features are understood. |
| header_bytes | Defined header length. |
| filesystem_block_bytes | Must be 4096 in v1. |
| total_blocks | Number of filesystem blocks on the device. |
| feature_compat | Safely ignored feature bits; zero in v1. |
| feature_incompat | Required-understanding feature bits; zero in v1. |
| state_flags | CLEAN and NEEDS_FSCK state bits. |
| filesystem_uuid[16] | UUID assigned when formatted; the sole UUID in MGFS v1. |
| root_record_id | Record ID of the root directory. |
| next_record_id | Next never-before-used ID. It is incremented for every new record and IDs are never reused. |
| allocation_bitmap_start_block, allocation_bitmap_block_count | Allocation bitmap region. |
| file_record_bitmap_start_block, file_record_bitmap_block_count | File Record bitmap region. |
| file_record_table_start_block, file_record_table_block_count | File Record table region. |
| file_record_count | Number of fixed File Record slots. |
| data_start_block, data_block_count | Allocatable data-area region. |
| format_time_ns | Format time in UTC nanoseconds since Unix epoch; written by the formatter and read by diagnostic tools. |
| last_mount_time_ns | Latest successful mount time. |
| superblock_checksum | CRC-64 of the declared header. |

Record ID zero is invalid. The formatter assigns the root a nonzero ID and sets next_record_id above it. If next_record_id reaches its 64-bit maximum, further record creation fails rather than reusing an ID.

CLEAN means cleanly unmounted or synced. NEEDS_FSCK means an unrecoverable metadata-write error occurred. With no journal, an unclean volume must be reported as requiring offline checking before writable use.

### Bitmap and File Record table blocks

Each allocation or File Record bitmap block has a 24-byte header and 4072 bytes of bits.

| Field | Meaning |
| --- | --- |
| metadata_kind | ALLOCATION_BITMAP or FILE_RECORD_BITMAP. |
| region_block_index | Zero-based index within that bitmap region. |
| checksum | CRC-64 of the whole bitmap block. |
| bits[] | Allocation bits. |

Allocation bitmap bits map ascending data-area blocks. File Record bitmap bits map ascending table slots. Bits beyond each declared capacity are permanently set.

Each File Record table block also begins with a 24-byte metadata header containing metadata_kind, region_block_index, and checksum, using metadata_kind FILE_RECORD_TABLE. The remaining 4072 bytes contain 21 File Records of 192 bytes, followed by 40 zeroed reserved bytes. The File Record bitmap maps the records in table-block order.

### File Record

A File Record is exactly 192 bytes. It is valid only when its File Record bitmap bit is set and its checksum is valid.

| Field | Meaning |
| --- | --- |
| record_type | FILE or DIRECTORY. No link, device, or symlink types exist in v1. |
| record_flags | INLINE_DATA or zero; other bits are reserved. |
| record_id | Nonzero, immutable, filesystem-local identifier. |
| generation | Incremented on record modification; detects stale caches. |
| logical_size_bytes | File length; directory-entry-stream length for directories. |
| extent_count | Total inline plus chained extents. |
| inline_extent_count | Valid inline extent count, from zero to two. |
| extent_list_head_block | First chained extent-list block, or zero. |
| inline_extents[2] | First two extents. |
| inline_data[56] | File bytes stored directly in the record. Must be zero for directories. |
| record_checksum | CRC-64 of this record. |

The fixed layout above totals 192 bytes exactly: 64 bytes of required scalar fields, 64 bytes of inline extents, 56 bytes of inline data, and an 8-byte checksum. This is why the inline-data limit is 56 bytes. It gains 32 bytes over the previous design without growing the record, wasting padding, or reducing the 21-record table-block density. A 64-byte payload would require a larger record for only eight further bytes of capacity.

Inline data is only for regular files with no extents; because there are no holes, logical_size_bytes is its inline-data length. A write growing beyond 56 bytes allocates normal data blocks, clears INLINE_DATA, and moves the existing bytes into the first data block. This keeps a one-byte file below 1 KiB of file-specific metadata plus data.

An empty file has zero size and extents. A directory has no inline data.

### Extents and extent-list blocks

An extent is 32 bytes:

| Field | Meaning |
| --- | --- |
| logical_start_block | First logical block in the owner. |
| physical_start_block | First data-area filesystem block. |
| block_count | Nonzero number of contiguous blocks. |
| extent_flags | DATA, DIRECTORY_METADATA, or EXTENT_LIST_METADATA. |

Extents are sorted by logical start, do not overlap, and point only into the data area. Adjacent compatible extents are coalesced.

The first two extents are inline. Further extents use chained extent-list blocks from the data area. A list block has a 64-byte header and up to 126 extents:

| Field | Meaning |
| --- | --- |
| list_magic | Identifies an MGFS extent-list block. |
| owner_record_id | Record ID owning the list. |
| next_list_block | Next list block, or zero. |
| entry_count | Extents in this list block. |
| checksum | CRC-64 of the complete block. |
| reserved[24] | Zero. |
| extents[] | Sorted extent records. |

Extent-list blocks are represented by EXTENT_LIST_METADATA extents, are allocated in the allocation bitmap, and are never file data.

### Directory entries

A directory is a File Record whose stream is variable-length, 8-byte-aligned directory entries. This avoids reserving 320 bytes for a short filename while retaining a straightforward sequential parser.

| Field | Meaning |
| --- | --- |
| target_record_id | Referenced Record ID. |
| name_byte_length | Valid UTF-8 byte count, from 1 through 255. |
| entry_flags | IN_USE or TOMBSTONE; other bits reserved. |
| entry_checksum | CRC-64 of the header, name, and zero alignment padding. |
| name[] | UTF-8 bytes followed by zero padding to an 8-byte boundary. |

The header is 32 bytes. An entry consumes 32 + name_byte_length bytes, rounded up to 8 bytes: a one-byte name consumes 40 bytes rather than 320. A tombstone retains its original length so scanning remains unambiguous. It may be reused only when the replacement fits; otherwise the new entry is appended and the old one remains a tombstone.

Names are case-sensitive UTF-8 byte strings. v1 performs no Unicode normalization, so distinct valid UTF-8 byte sequences are distinct names. Names cannot be empty, contain NUL or slash, equal dot or dot-dot, or exceed 255 bytes. A directory has at most one in-use entry of any name.

Directories do not store dot or dot-dot entries. Path resolution tracks its current parent in memory; root is its own parent for dot-dot behavior. The first entry in an otherwise empty directory requires a shared 4 KiB directory block, but each additional short entry consumes only its compact entry size.

## 3. Core algorithms

### Mount

1. Read block 0. Validate magic, header size, block size, version, feature masks, region bounds, and checksum.
2. Validate every bitmap and File Record table block header.
3. Scan allocated File Record slots. Validate every record checksum and verify every nonzero Record ID is unique.
4. Locate root_record_id in the validated File Record table. It must name an allocated DIRECTORY record.
5. Update last_mount_time_ns, then mark the superblock dirty before writes. A clean unmount or sync clears dirty only after all metadata is durable.

The implementation may build an in-memory Record ID cache from this scan. A cached record is accepted only when its on-disk generation still matches. MGFS v1 requires no on-disk ID index; the File Record table is authoritative.

### Record ID lookup and traversal

A Record ID lookup scans the validated table or uses the in-memory cache built from it. The record's bitmap bit, checksum, and generation remain authoritative.

To resolve /docs/notes.txt, start with root_record_id. For each component, read the current directory stream via its extents, validate each entry, compare UTF-8 name bytes, locate the entry's target Record ID, and require intermediate records to be directories.

### Allocate blocks and File Records

For data blocks, scan the allocation bitmap from an allocator hint for a clear contiguous run. Prefer the first adequate run; otherwise allocate several runs and create multiple extents. Persist bitmap changes before publishing metadata that references those blocks.

For a File Record, find a clear File Record bitmap bit and persist it. Reserve next_record_id by incrementing and persisting the superblock, then write a fully initialized record carrying the reserved ID. The ID is never reused, even when its record is deleted.

### Create files and directories

touch resolves and validates its parent, confirms the name is absent, allocates a File Record and unique Record ID, writes an empty FILE record, then appends or reuses a tombstoned directory entry containing that ID. The directory entry is published last.

mkdir follows the same process with a DIRECTORY record. It remains extent-free until its first child requires a directory-entry block.

### Read and write files

Read validates the record, including its record_type, INLINE_DATA state, inline_extent_count, and total extent_count. It then either copies INLINE_DATA for a small file or clamps to logical_size_bytes and maps requested logical blocks through sorted extents. It validates each referenced extent-list block's magic, owner_record_id, entry_count, and checksum before using its extents, and requires DATA extents for regular-file bytes and DIRECTORY_METADATA extents for directory streams. Holes are not defined in v1: every byte below logical size must map to an extent unless INLINE_DATA is set.

An in-range write performs read-modify-write for partial filesystem blocks and direct writes for whole blocks. An inline-data write that remains at most 56 bytes rewrites only the File Record. A larger write allocates blocks, persists their bitmap, writes the prior inline bytes plus new data, clears INLINE_DATA, updates or appends extents, then writes the File Record with updated size, generation, and checksum. New extent-list blocks are durable before a record references them. v1 provides no copy-on-write or multi-block atomicity.

### Delete

There are no hard links, so a reachable file has one directory entry. Delete first writes its entry as TOMBSTONE, then frees data and extent-list blocks in the allocation bitmap, clears the File Record bitmap bit, and may zero the slot. The Record ID is not reused. rmdir requires no in-use entries. Hiding the entry before freeing storage prevents new traversal from reaching a partly deleted record.

### Rename and move

Rename validates that its destination name is absent and changes only the existing entry name and checksum. The target Record ID, File Record, and extents do not change.

Move validates the destination directory and name, writes a destination entry with the same target Record ID, then tombstones the source entry. It leaves the File Record and data untouched. Without a journal, cross-directory move is not crash atomic; an offline checker resolves incomplete or duplicate entries.

## 4. Versioning and integrity

Magic is MGFSv1 followed by two NUL bytes. Major changes are incompatible. An implementation rejects unknown major versions, unsupported block size, invalid header size, and unknown feature_incompat bits. It may ignore unknown feature_compat bits.

Every superblock, bitmap block, File Record table block, File Record, extent-list block, and directory entry has a CRC-64 checksum. User data has no checksum in v1. Checksums detect corruption but do not make compound operations atomic.

Metadata updates use publish-last ordering: allocate and persist storage first, write initialized dependent metadata, and finally publish a directory reference. Dirty state reports unclean shutdown. A future mgfsck must verify bounds, bitmap consistency, Record ID uniqueness, extent overlap, directory-name uniqueness, checksums, and reachability.

## 5. Future compatibility

Feature masks, declared header size, reserved bytes, record flags, extent flags, entry flags, and static region descriptors leave extension points without changing the VFS-facing model: directory entry to Record ID, Record ID to File Record, File Record to extents.

- Journaling: add a journal extent or region and an incompatible feature bit.
- Snapshots and copy-on-write: add reference-count or snapshot metadata regions through appended superblock fields.
- Compression and encryption: add File Record and extent feature flags; unaware readers reject them.
- Permissions, ACLs, and extended attributes: add an optional File Record extension object.
- Persistent Record ID indexing: add an optional ID-tree region if table scans become expensive; directory entries remain unchanged.
- Data checksums and deduplication: add data-descriptor metadata rather than changing names, Record IDs, or basic extents.

No future revision may reinterpret an existing Record ID within its filesystem, change existing extent meaning, or expose Record IDs to applications.

## 6. Implementation roadmap

1. Freeze this document and add host binary-layout and checksum-vector tests.
2. Build mkmgfs, a host formatter for a blank image with valid regions, bitmaps, root record, and inspector output.
3. Add kernel probe and read-only mount; test a blank image mounts as root and ls succeeds.
4. Add read-only directory traversal and Record ID resolution; test nested formatter fixtures.
5. Add read-only inline then chained extent reads; test cat for inline files and files crossing block and extent boundaries.
6. Add block and File Record allocation/free primitives; test persistent Record ID allocation across remount.
7. Add touch and mkdir; test create, reboot/remount, and ls.
8. Add file inline writes, conversion to extents, and extension; test one-byte files, 56-byte boundary conversion, partial blocks, multiple extents, remount, and cat.
9. Add rm, empty-directory removal, rename, and move; verify Record ID stability with the host inspector.
10. Build mgfsck and failure tests for bad checksums, interrupted operation ordering, and bitmap/extent inconsistency.
11. Reformat TestDisk.img as MGFS and mount the MGFS driver as root. Leave Mangrove.img rebuilt FAT32 for EFI only.
