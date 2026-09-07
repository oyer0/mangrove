/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <block.h>

#define GPT_GUID_BYTES 16U
#define GPT_PARTITION_NAME_MAX 64U
#define GPT_MAX_PARTITION_ENTRIES 128U
#define GPT_PARTITION_ENTRY_BYTES 128U

typedef enum {
    GPT_TABLE_NONE = 0,
    GPT_TABLE_VALID = 1,
    GPT_TABLE_MBR = 2,
    GPT_TABLE_CORRUPT = 3,
} gpt_table_type_t;

typedef struct {
    gpt_table_type_t type;
    u32 partition_count;
    u32 entry_count;
    u32 entry_size;
    u64 first_usable_lba;
    u64 last_usable_lba;
} gpt_table_info_t;

typedef struct {
    u64 first_lba;
    u64 last_lba;
    u64 size_bytes;
    u32 partition_number;
} gpt_partition_plan_t;

/* Stable GPT role markers written by Mangrove image creation/update tools. */
#define GPT_MANGROVE_ROOT_NAME "MANGROVE_ROOT"
#define GPT_MANGROVE_BOOT_NAME "MANGROVE_ESP"

typedef struct {
    block_device_t *parent;
    u64 first_lba;
    u64 last_lba;
    u32 number;
    u8 type_guid[GPT_GUID_BYTES];
    u8 unique_guid[GPT_GUID_BYTES];
    char name[GPT_PARTITION_NAME_MAX];
} gpt_partition_info_t;

/* Scan a 512-byte block device and register its GPT partitions. */
bool gpt_scan_device(block_device_t *device);

/* Read and validate both GPT copies without changing the device. */
int gpt_get_table_info(block_device_t *device, gpt_table_info_t *out_info);

/* Plan/perform bounded GPT mutations against this exact live disk instance. */
int gpt_plan_partition(block_device_t *device, u64 requested_bytes,
                       bool use_rest, gpt_partition_plan_t *out_plan);
int gpt_initialize_device(block_device_t *device);
int gpt_create_partition(block_device_t *device, u32 number,
                         u64 first_lba, u64 last_lba);
int gpt_delete_partition(block_device_t *device,
                         block_device_t *partition, u32 number);

/* Return firmware/image-provided metadata for a registered GPT partition. */
bool gpt_get_partition_info(block_device_t *device,
                            gpt_partition_info_t *out_info);

/* Partition discovery can run before a parent is copied into the published
 * block registry slot.  Compare the immutable instance identity, not the
 * address of either representation. */
bool gpt_partition_parent_matches(const gpt_partition_info_t *partition,
                                  const block_device_t *parent);

/* The standard EFI System Partition type GUID in GPT byte order. */
bool gpt_partition_is_esp(const gpt_partition_info_t *info);

/* Remove partition devices belonging to a detached parent block device. */
void gpt_remove_partitions_for_parent(block_device_t *parent);

/* Mark partition instances unavailable before deferred parent teardown. */
void gpt_mark_partitions_gone_for_parent(block_device_t *parent);
