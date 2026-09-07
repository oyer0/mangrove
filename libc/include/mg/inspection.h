#pragma once

#include <mg/error.h>
#include <mg/identity.h>
#include <mg/session.h>

/* Read-only system inspection ABI.  Snapshot records contain copied values
 * only; no kernel pointers or live object references cross the syscall. */
#define MG_INSPECTION_NAME_MAX       32U
#define MG_INSPECTION_SERVICE_MAX    32U
#define MG_PROCESS_SNAPSHOT_PAGE_MAX 32U

typedef enum {
    MG_PROCESS_INSPECTION_RUNNING = 1,
    MG_PROCESS_INSPECTION_READY,
    MG_PROCESS_INSPECTION_BLOCKED,
    MG_PROCESS_INSPECTION_EXITED,
} mg_process_inspection_state_t;

#define MG_PROCESS_FLAG_SYSTEM_SERVICE ((u32)1U << 0)
#define MG_PROCESS_FLAG_SESSION_SHELL  ((u32)1U << 1)

/* The SYSTEM role is intentionally separate from human regular/admin roles. */
#define MG_INSPECTION_ROLE_SYSTEM ((u32)2U)

typedef struct PACKED {
    u64 pid;
    u64 parent_pid;
    u64 session_id;
    mg_uid_t uid;
    u32 role;
    u32 state;
    u32 flags;
    char name[MG_INSPECTION_NAME_MAX];
    char username[MG_INSPECTION_NAME_MAX];
    char service[MG_INSPECTION_SERVICE_MAX];
} mg_process_info_t;

/* Pointer members are syscall arguments only and are never retained. */
typedef struct {
    u32 offset;
    u32 result_capacity;
    mg_process_info_t *result;
    u32 *out_count;
    u32 *out_total;
} mg_process_snapshot_request_t;

typedef struct PACKED {
    u64 physical_total_bytes;
    u64 physical_used_bytes;
    u64 physical_free_bytes;
    u64 kernel_heap_total_bytes;
    u64 kernel_heap_used_bytes;
    u64 kernel_heap_free_bytes;
} mg_system_memory_info_t;

typedef char mg_process_info_size_check[
    sizeof(mg_process_info_t) <= 256U ? 1 : -1];

mg_result_t process_snapshot(mg_process_snapshot_request_t *request);
mg_result_t memory_info(mg_system_memory_info_t *info);
