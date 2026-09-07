/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <mg/error.h>
#include <mg/types.h>

#define MG_STORAGE_PROTOCOL_VERSION 1U
#define MG_STORAGE_FILESYSTEM_MAX    16U
#define MG_STORAGE_LABEL_MAX         64U
#define MG_STORAGE_FILESYSTEM_FAT32  1U
#define MG_STORAGE_FILESYSTEM_MGFS   2U
#define MG_STORAGE_FILESYSTEM_EXFAT  3U

typedef enum {
    MG_STORAGE_OP_FORMAT = 1,
    MG_STORAGE_OP_LABEL = 2,
    MG_STORAGE_OP_GPT_INITIALIZE = 3,
    MG_STORAGE_OP_GPT_CREATE = 4,
    MG_STORAGE_OP_GPT_DELETE = 5,
} mg_storage_operation_t;

typedef enum {
    MG_STORAGE_GPT_TABLE_NONE = 0,
    MG_STORAGE_GPT_TABLE_GPT = 1,
    MG_STORAGE_GPT_TABLE_MBR = 2,
    MG_STORAGE_GPT_TABLE_CORRUPT = 3,
} mg_storage_gpt_table_t;

typedef struct PACKED {
    u64 instance_id;
    u64 parent_instance_id;
    u32 filesystem;
    u32 reserved;
} mg_storage_format_request_t;

typedef struct PACKED {
    u64 instance_id;
    u64 parent_instance_id;
    u32 reserved;
    u32 reserved2;
    char filesystem[MG_STORAGE_FILESYSTEM_MAX];
    char label[MG_STORAGE_LABEL_MAX];
} mg_storage_label_request_t;

typedef struct PACKED {
    u64 instance_id;
    u32 reserved;
    u32 reserved2;
} mg_storage_gpt_info_request_t;

typedef struct PACKED {
    u32 table_type;
    u32 partition_count;
    u32 entry_count;
    u32 entry_size;
    u64 first_usable_lba;
    u64 last_usable_lba;
} mg_storage_gpt_info_t;

typedef struct PACKED {
    u64 instance_id;
    u64 requested_bytes;
    u32 flags;
    u32 reserved;
} mg_storage_gpt_plan_request_t;

#define MG_STORAGE_GPT_PLAN_FLAG_REST 1U

typedef struct PACKED {
    u64 first_lba;
    u64 last_lba;
    u64 size_bytes;
    u32 partition_number;
    u32 reserved;
} mg_storage_gpt_plan_t;

typedef struct PACKED {
    u64 instance_id;
    u64 first_lba;
    u64 last_lba;
    u32 partition_number;
    u32 reserved;
} mg_storage_gpt_create_request_t;

typedef struct PACKED {
    u64 instance_id;
    u64 partition_instance_id;
    u32 partition_number;
    u32 reserved;
} mg_storage_gpt_delete_request_t;

mg_result_t storage_authorize(u32 operation);
mg_result_t storage_session_authorize(void);
mg_result_t storage_authorize_cancel(void);
mg_result_t storage_format(const mg_storage_format_request_t *request);
mg_result_t storage_set_label(const mg_storage_label_request_t *request);
mg_result_t storage_gpt_info(u64 instance_id, mg_storage_gpt_info_t *info);
mg_result_t storage_gpt_plan(const mg_storage_gpt_plan_request_t *request,
                             mg_storage_gpt_plan_t *plan);
mg_result_t storage_gpt_initialize(u64 instance_id);
mg_result_t storage_gpt_create(const mg_storage_gpt_create_request_t *request);
mg_result_t storage_gpt_delete(const mg_storage_gpt_delete_request_t *request);
