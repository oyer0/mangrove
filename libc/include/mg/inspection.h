/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/identity.h>
#include <mg/session.h>

/* Read-only system inspection ABI.  Snapshot records contain copied values
 * only; no kernel pointers or live object references cross the syscall. */
#define MG_INSPECTION_NAME_MAX       32U
#define MG_INSPECTION_SERVICE_MAX    32U
#define MG_INSPECTION_PATH_MAX       128U
#define MG_INSPECTION_CPU_MODEL_MAX  64U
#define MG_INSPECTION_GPU_NAME_MAX   64U
#define MG_INSPECTION_GPU_MAX        8U
#define MG_PROCESS_SNAPSHOT_PAGE_MAX 32U
#define MG_CPU_SNAPSHOT_PAGE_MAX     32U
#define MG_PROCESS_CPU_NONE           (~(u32)0)
/* At the scheduler snapshot instant, process-owned threads were executing on
 * distinct CPUs. This is a point-in-time observation, not a reservation. */
#define MG_PROCESS_CPU_MULTIPLE       (~(u32)1)

typedef enum {
    MG_PROCESS_INSPECTION_RUNNING = 1,
    MG_PROCESS_INSPECTION_READY,
    MG_PROCESS_INSPECTION_BLOCKED,
    MG_PROCESS_INSPECTION_EXITED,
} mg_process_inspection_state_t;

#define MG_PROCESS_FLAG_SYSTEM_SERVICE ((u32)1U << 0)
#define MG_PROCESS_FLAG_SESSION_SHELL  ((u32)1U << 1)
/* Transient cleanup state in a copied process snapshot. Region-reap pending
 * means revoked mappings remain reserved until existing syscall leases drain. */
#define MG_PROCESS_FLAG_WORKER_REAP_PENDING       ((u32)1U << 2)
#define MG_PROCESS_FLAG_USER_REGION_REAP_PENDING  ((u32)1U << 3)
#define MG_PROCESS_FLAG_WORKER_REAPER_ACTIVE      ((u32)1U << 4)
#define MG_PROCESS_FLAG_REAP_STATE_MASK \
    (MG_PROCESS_FLAG_WORKER_REAP_PENDING | \
     MG_PROCESS_FLAG_USER_REGION_REAP_PENDING | \
     MG_PROCESS_FLAG_WORKER_REAPER_ACTIVE)

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
    u32 running_cpu;
    /* Scheduler execution time, in milliseconds of actual CPU time. */
    u64 cpu_time_ms;
    /* User-mapped physical pages owned by this address space. */
    u64 memory_bytes;
    char executable_path[MG_INSPECTION_PATH_MAX];
} mg_process_info_t;

/* Pointer members are syscall arguments only and are never retained. */
typedef struct {
    u32 offset;
    u32 result_capacity;
    mg_process_info_t *result;
    u32 *out_count;
    u32 *out_total;
} mg_process_snapshot_request_t;

#define MG_CPU_FLAG_ONLINE ((u32)1U << 0)
#define MG_CPU_FLAG_BSP    ((u32)1U << 1)

typedef struct PACKED {
    u32 index;
    u32 apic_id;
    u32 flags;
    u32 reserved;
    u64 total_ticks;
    u64 busy_ticks;
} mg_cpu_info_t;

typedef struct {
    u32 offset;
    u32 result_capacity;
    mg_cpu_info_t *result;
    u32 *out_count;
    u32 *out_total;
} mg_cpu_snapshot_request_t;

typedef struct PACKED {
    u64 physical_total_bytes;
    u64 physical_used_bytes;
    u64 physical_free_bytes;
    u64 kernel_heap_total_bytes;
    u64 kernel_heap_used_bytes;
    u64 kernel_heap_free_bytes;
} mg_system_memory_info_t;

/* Bounded machine-description data copied from kernel-owned discovery state.
 * Strings are always NUL-terminated when returned successfully. */
typedef struct PACKED {
    u16 vendor_id;
    u16 device_id;
    u8 class_code;
    u8 subclass;
    u8 prog_if;
    u8 reserved;
    char name[MG_INSPECTION_GPU_NAME_MAX];
} mg_gpu_info_t;

typedef struct PACKED {
    char cpu_model[MG_INSPECTION_CPU_MODEL_MAX];
    u32 logical_cpu_count;
    u32 display_width;
    u32 display_height;
    u32 gpu_count;
    u32 gpu_total;
    mg_gpu_info_t gpus[MG_INSPECTION_GPU_MAX];
} mg_system_info_t;

typedef char mg_process_info_size_check[
    sizeof(mg_process_info_t) <= 512U ? 1 : -1];
typedef char mg_cpu_info_size_check[
    sizeof(mg_cpu_info_t) <= 128U ? 1 : -1];
typedef char mg_system_info_size_check[
    sizeof(mg_system_info_t) <= 1024U ? 1 : -1];

mg_result_t process_snapshot(mg_process_snapshot_request_t *request);
mg_result_t memory_info(mg_system_memory_info_t *info);
mg_result_t cpu_snapshot(mg_cpu_snapshot_request_t *request);
mg_result_t system_info(mg_system_info_t *info);

/* Authorized read-only recovery inspection. One complete registry snapshot,
 * never paginated: insufficient capacity returns BUFFER_TOO_SMALL and the
 * required count without changing result records. Output count/entry ranges
 * must not overlap. Liveness is sampled while
 * copying; it is diagnostic evidence, not admission authority. */
#define MG_RECOVERY_OWNER_MAX 32U
#define MG_RECOVERY_OWNER_ACTIVE 1U
#define MG_RECOVERY_OWNER_UNRESOLVED 2U
#define MG_RECOVERY_DEVICE_ONLINE (1U << 0)
#define MG_RECOVERY_PHYSICAL_ONLINE (1U << 1)
#define MG_RECOVERY_PRIVATE (1U << 2)
#define MG_RECOVERY_OFFLINE_RETAINED (1U << 3)
typedef struct {
    u64 owner_id;
    u64 instance_id;
    u64 physical_instance_id;
    u32 state;
    u32 flags;
} mg_recovery_owner_info_t;
typedef struct {
    u32 result_capacity;
    mg_recovery_owner_info_t *result;
    u32 *out_count;
} mg_recovery_owner_snapshot_request_t;
mg_result_t recovery_owners_snapshot(mg_recovery_owner_snapshot_request_t *request);
