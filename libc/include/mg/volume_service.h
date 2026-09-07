#pragma once

#include <mg/error.h>
#include <mg/ipc.h>

/* Volume inspection and lifecycle requests are exposed through this service
 * protocol.  Clients never receive kernel pointers or private VFS state. */
#define MG_VOLUME_PROTOCOL_VERSION 1U
#define MG_VOLUME_REQUEST          ((u16)1U)
#define MG_VOLUME_RESPONSE         ((u16)2U)
#define MG_VOLUME_RESPONSE_MAX     3U
#define MG_VOLUME_MAX_RECORDS      32U

#define MG_VOLUME_NAME_MAX         48U
#define MG_VOLUME_FILESYSTEM_MAX   16U
#define MG_VOLUME_LABEL_MAX        64U
#define MG_VOLUME_MOUNT_MAX        128U
#define MG_VOLUME_ROLE_MAX         24U

typedef enum {
    MG_VOLUME_OP_SNAPSHOT = 1,
    MG_VOLUME_OP_MOUNT = 2,
    MG_VOLUME_OP_UNMOUNT = 3,
    MG_VOLUME_OP_EJECT = 4,
} mg_volume_operation_t;

#define MG_VOLUME_MOUNT_FLAG_READ_ONLY ((u32)1U << 0)
#define MG_VOLUME_UNMOUNT_FLAG_ALL_CHILDREN ((u32)1U << 0)
#define MG_VOLUME_UNMOUNT_FLAG_PREFLIGHT ((u32)1U << 1)
#define MG_VOLUME_RESULT_ALREADY_MOUNTED ((u32)1U << 0)

typedef enum {
    MG_VOLUME_STATE_PRESENT = 1,
} mg_volume_state_t;

#define MG_VOLUME_FLAG_MOUNTABLE      ((u32)1U << 0)
#define MG_VOLUME_FLAG_MOUNTED        ((u32)1U << 1)
#define MG_VOLUME_FLAG_REMOVABLE      ((u32)1U << 2)
#define MG_VOLUME_FLAG_SYSTEM_MANAGED ((u32)1U << 3)
#define MG_VOLUME_FLAG_READ_ONLY      ((u32)1U << 4)

typedef struct PACKED {
    u16 version;
    u16 operation;
    u32 offset;
    u32 limit;
    u64 instance_id;
    char target[MG_VOLUME_MOUNT_MAX];
} mg_volume_request_t;

typedef struct PACKED {
    u64 instance_id;
    u64 parent_instance_id;
    u64 size_bytes;
    u32 state;
    u32 flags;
    u8 disk_number;
    u8 partition_number;
    u8 reserved[2];
    char name[MG_VOLUME_NAME_MAX];
    char filesystem[MG_VOLUME_FILESYSTEM_MAX];
    char label[MG_VOLUME_LABEL_MAX];
    char mount_point[MG_VOLUME_MOUNT_MAX];
    char role[MG_VOLUME_ROLE_MAX];
} mg_volume_info_t;

typedef struct PACKED {
    i32 result;
    u32 count;
    u32 total;
    u32 next_offset;
    mg_volume_info_t volumes[MG_VOLUME_RESPONSE_MAX];
} mg_volume_response_t;

/* Kernel syscall payload for volumed's policy-owned automount operation.
 * The kernel validates the caller, device instance, filesystem, and /vol
 * mount path before resolving the current block-device object. */
typedef struct PACKED {
    u64 instance_id;
    u64 parent_instance_id;
    u32 flags;
    u32 reserved;
    char filesystem[MG_VOLUME_FILESYSTEM_MAX];
    char mount_point[MG_VOLUME_MOUNT_MAX];
} mg_volume_mount_request_t;

typedef struct PACKED {
    u64 instance_id;
    u64 parent_instance_id;
    u32 flags;
    u32 reserved;
} mg_volume_unmount_request_t;

typedef struct PACKED {
    u64 instance_id;
    u64 parent_instance_id;
    u32 suppressed;
    u32 reserved;
} mg_volume_suppression_request_t;

typedef struct PACKED {
    i32 result;
    u32 flags;
    u64 instance_id;
    char mount_point[MG_VOLUME_MOUNT_MAX];
} mg_volume_operation_response_t;

typedef char mg_volume_request_size_check[
    sizeof(mg_volume_request_t) <= MG_IPC_MESSAGE_PAYLOAD_MAX ? 1 : -1];
typedef char mg_volume_response_size_check[
    sizeof(mg_volume_response_t) <= MG_IPC_MESSAGE_PAYLOAD_MAX ? 1 : -1];
typedef char mg_volume_mount_request_size_check[
    sizeof(mg_volume_mount_request_t) <= MG_IPC_MESSAGE_PAYLOAD_MAX ? 1 : -1];
typedef char mg_volume_unmount_request_size_check[
    sizeof(mg_volume_unmount_request_t) <= MG_IPC_MESSAGE_PAYLOAD_MAX ? 1 : -1];
typedef char mg_volume_suppression_request_size_check[
    sizeof(mg_volume_suppression_request_t) <= MG_IPC_MESSAGE_PAYLOAD_MAX ? 1 : -1];
typedef char mg_volume_operation_response_size_check[
    sizeof(mg_volume_operation_response_t) <= MG_IPC_MESSAGE_PAYLOAD_MAX ? 1 : -1];

/* Restricted to the kernel-created volumed service identity. */
mg_result_t volume_mount(const mg_volume_mount_request_t *request);
mg_result_t volume_unmount(const mg_volume_unmount_request_t *request);
mg_result_t volume_set_suppressed(
    const mg_volume_suppression_request_t *request);
mg_result_t pass_authorize_volume_request(mg_handle_t request, u32 operation);
