# MGFS v1 Specification Addendum: Binary Format and Formatter Rules

## Status

This addendum is normative and is read together with [MGFS v1](mgfs.md). It resolves binary-format and formatter ambiguities without changing the MGFS architecture.

All integers are unsigned little-endian 64-bit values unless marked as a byte array. Every structure is packed: no implicit compiler padding is permitted. Every structure begins at an 8-byte-aligned offset. A filesystem block is exactly 4096 bytes.

## 1. Formatter inputs and deterministic output

A formatter invocation has exactly these format parameters:

    mkmgfs --blocks <total_blocks> --uuid <canonical-uuid> --format-time-ns <u64> <image-path>

- total_blocks is the exact output size divided by 4096.
- canonical-uuid is an RFC 4122 UUID written as 36 ASCII characters. The formatter stores its 16 raw bytes in normal RFC 4122 field order.
- format-time-ns is the format_time_ns superblock value.
- last_mount_time_ns is always zero in a newly formatted image.

The formatter must not use the wall clock, random data, host endianness, or uninitialized bytes. Therefore two independent formatters given identical parameters produce byte-identical images. Callers that require distinct volumes must provide distinct UUIDs.

The formatter rejects total_blocks less than 64 and rejects an output path whose existing or newly created size cannot be exactly total_blocks times 4096 bytes.

## 2. Constants

All unspecified flag bits and all unspecified enumeration values are reserved and must be zero.

| Name | Value |
| --- | ---: |
| MGFS_BLOCK_BYTES | 4096 |
| MGFS_FORMAT_MAJOR | 1 |
| MGFS_FORMAT_MINOR | 0 |
| MGFS_HEADER_BYTES | 200 |
| MGFS_RECORD_BYTES | 192 |
| MGFS_EXTENT_BYTES | 32 |
| MGFS_RECORDS_PER_TABLE_BLOCK | 21 |
| MGFS_BITMAP_HEADER_BYTES | 24 |
| MGFS_BITMAP_BITS_PER_BLOCK | 32576 |
| MGFS_MIN_TOTAL_BLOCKS | 64 |

### State flags

| Name | Value |
| --- | ---: |
| MGFS_STATE_CLEAN | 0x0000000000000001 |
| MGFS_STATE_NEEDS_FSCK | 0x0000000000000002 |

A newly formatted filesystem has state_flags equal to MGFS_STATE_CLEAN.

### Metadata kinds

| Name | Value |
| --- | ---: |
| MGFS_METADATA_ALLOCATION_BITMAP | 1 |
| MGFS_METADATA_RECORD_BITMAP | 2 |
| MGFS_METADATA_RECORD_TABLE | 3 |

### Record types and flags

| Name | Value |
| --- | ---: |
| MGFS_RECORD_FILE | 1 |
| MGFS_RECORD_DIRECTORY | 2 |
| MGFS_RECORD_INLINE_DATA | 0x0000000000000001 |

MGFS_RECORD_INLINE_DATA is valid only for a MGFS_RECORD_FILE with zero extents and logical_size_bytes at most 56.

### Extent flags

| Name | Value |
| --- | ---: |
| MGFS_EXTENT_DATA | 0x0000000000000001 |
| MGFS_EXTENT_DIRECTORY_METADATA | 0x0000000000000002 |
| MGFS_EXTENT_LIST_METADATA | 0x0000000000000004 |

Exactly one defined extent-type bit must be set in each v1 extent.

### Directory-entry flags

| Name | Value |
| --- | ---: |
| MGFS_DIRENT_IN_USE | 0x0000000000000001 |
| MGFS_DIRENT_TOMBSTONE | 0x0000000000000002 |

Exactly one defined directory-entry state bit must be set in each directory entry.

## 3. CRC-64/ECMA-182

MGFS uses the direct, non-reflected CRC-64/ECMA-182 algorithm:

| Parameter | Value |
| --- | --- |
| Width | 64 |
| Polynomial | 0x42F0E1EBA9EA3693 |
| Initial remainder | 0x0000000000000000 |
| Input reflection | false |
| Output reflection | false |
| Final XOR | 0x0000000000000000 |

Bytes are processed in increasing byte-offset order. For each input byte, XOR it into bits 63:56 of the running remainder; then shift left eight times, XORing the polynomial whenever the previous bit 63 was one. The checksum value is stored as a little-endian 64-bit integer.

The CRC of the nine ASCII bytes 123456789 is `0x6C40DF5F0B497347`.

For every checksum, its own eight-byte checksum field is treated as zero during calculation.

| Protected object | Covered bytes |
| --- | --- |
| Superblock | Byte offsets 0 through 199 only; bytes 200 through 4095 are not covered. |
| Allocation bitmap block | All 4096 bytes. |
| Record bitmap block | All 4096 bytes. |
| Record table block | All 4096 bytes. |
| File Record | Its exact 192 bytes. |
| Extent-list block | All 4096 bytes. |
| Directory entry | Its exact aligned entry length, from the first header byte through its final zero padding byte. |

## 4. Exact binary layouts

### 4.1 Superblock

The superblock is block zero. Its header is 200 bytes, has 8-byte alignment, and has header_bytes equal to 200. Bytes 200 through 4095 are zero.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | magic[8]: bytes 4D 47 46 53 76 31 00 00, or MGFSv1 followed by two NUL bytes |
| 8 | 8 | format_major |
| 16 | 8 | format_minor |
| 24 | 8 | header_bytes |
| 32 | 8 | filesystem_block_bytes |
| 40 | 8 | total_blocks |
| 48 | 8 | feature_compat |
| 56 | 8 | feature_incompat |
| 64 | 8 | state_flags |
| 72 | 16 | filesystem_uuid[16] |
| 88 | 8 | root_record_id |
| 96 | 8 | next_record_id |
| 104 | 8 | allocation_bitmap_start_block |
| 112 | 8 | allocation_bitmap_block_count |
| 120 | 8 | file_record_bitmap_start_block |
| 128 | 8 | file_record_bitmap_block_count |
| 136 | 8 | file_record_table_start_block |
| 144 | 8 | file_record_table_block_count |
| 152 | 8 | file_record_count |
| 160 | 8 | data_start_block |
| 168 | 8 | data_block_count |
| 176 | 8 | format_time_ns |
| 184 | 8 | last_mount_time_ns |
| 192 | 8 | superblock_checksum |

### 4.2 Metadata block header

Every allocation bitmap, Record bitmap, and Record table block begins with this 24-byte, 8-byte-aligned header.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | metadata_kind |
| 8 | 8 | region_block_index |
| 16 | 8 | checksum |

For a bitmap block, bits begin at offset 24. For a Record table block, File Records begin at offset 24.

### 4.3 File Record

A File Record is 192 bytes and 8-byte aligned.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | record_type |
| 8 | 8 | record_flags |
| 16 | 8 | record_id |
| 24 | 8 | generation |
| 32 | 8 | logical_size_bytes |
| 40 | 8 | extent_count |
| 48 | 8 | inline_extent_count |
| 56 | 8 | extent_list_head_block |
| 64 | 32 | inline_extents[0] |
| 96 | 32 | inline_extents[1] |
| 128 | 56 | inline_data[56] |
| 184 | 8 | record_checksum |

### 4.4 Extent

An extent is 32 bytes and 8-byte aligned.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | logical_start_block |
| 8 | 8 | physical_start_block |
| 16 | 8 | block_count |
| 24 | 8 | extent_flags |

### 4.5 Extent-list block

An extent-list block is exactly one filesystem block. Its 64-byte header is 8-byte aligned, and its 126 extents begin at offset 64.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | list_magic: 0x315458455346474D, representing MGFSEXT1 as little-endian bytes |
| 8 | 8 | owner_record_id |
| 16 | 8 | next_list_block |
| 24 | 8 | entry_count |
| 32 | 8 | checksum |
| 40 | 24 | reserved[24] |
| 64 | 4032 | extents[126] |

### 4.6 Directory entry

A directory entry is packed and has 8-byte alignment. It has a 32-byte header and a variable byte-array name. Its exact length is:

    align_up(32 + name_byte_length, 8)

All alignment-padding bytes are zero.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | target_record_id |
| 8 | 8 | name_byte_length |
| 16 | 8 | entry_flags |
| 24 | 8 | entry_checksum |
| 32 | name_byte_length | name[] |
| 32 + name_byte_length | 0 through 7 | zero padding |

## 5. Deterministic layout algorithm

Let N equal total_blocks.

1. Set T equal to max(1, floor(N / 320)), the File Record table block count.
2. Set R equal to 21 times T, the File Record count.
3. Set RB equal to ceil(R / 32576), the Record bitmap block count.
4. Set AB equal to ceil(N / 32576), the allocation bitmap block count.
5. Set the regions:

| Region | Start block | Block count |
| --- | ---: | ---: |
| Superblock | 0 | 1 |
| Allocation bitmap | 1 | AB |
| Record bitmap | 1 + AB | RB |
| Record table | 1 + AB + RB | T |
| Data area | 1 + AB + RB + T | N - (1 + AB + RB + T) |

The formatter rejects a result with an empty data area. For the required minimum of 64 blocks, the data area is nonempty.

AB is deliberately calculated from N, rather than from the final data-area count. It guarantees sufficient bitmap capacity without a recursive sizing rule. Every bit that does not represent a data-area block is set permanently.

## 6. Initial filesystem state

All image bytes are zero before structures are written.

### Superblock

The formatter writes the layout values above, format_major equal to 1, format_minor equal to 0, header_bytes equal to 200, filesystem_block_bytes equal to 4096, zero feature masks, state_flags equal to MGFS_STATE_CLEAN, the supplied filesystem UUID and format time, last_mount_time_ns equal to zero, root_record_id equal to 1, and next_record_id equal to 2. It then writes the superblock checksum.

### Allocation bitmap

Each allocation bitmap block has its required header and CRC. Every bit representing a data-area block is clear: the empty root directory owns no data block. Every remaining bitmap-capacity bit is set.

The first free data-area block has data-area index zero and physical filesystem-block number data_start_block.

### Record bitmap

Each Record bitmap block has its required header and CRC. Bit zero is set because it represents the root record's table slot. Bits one through file_record_count minus one are clear. Every remaining bitmap-capacity bit is set.

Record ID zero is reserved by value; it does not consume a File Record table slot. The root uses table slot zero and Record ID one. The first free Record table slot is one.

### Record table

Every table block has metadata_kind equal to MGFS_METADATA_RECORD_TABLE, its zero-based table-region index, zeroed reserved bytes, and a CRC over the final 4096-byte block.

All unallocated File Record slots are zero. Table slot zero contains the root record:

| Field | Value |
| --- | ---: |
| record_type | MGFS_RECORD_DIRECTORY |
| record_flags | 0 |
| record_id | 1 |
| generation | 1 |
| logical_size_bytes | 0 |
| extent_count | 0 |
| inline_extent_count | 0 |
| extent_list_head_block | 0 |
| inline extents and inline data | all zero |

The formatter computes the root record checksum before computing the enclosing table-block checksum.

## 7. Required write order

For each metadata block, populate its fields and payload, set its checksum field to zero, compute and write its CRC, then write the block. For a Record table block this includes computing each allocated File Record checksum first.

Write all bitmap and Record table blocks before writing the superblock. Write the superblock last. A complete successfully returned image therefore always has a valid superblock that refers only to initialized metadata.
