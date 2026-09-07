#pragma once

#include <mg/error.h>
#include <mg/ipc.h>

/* Device inspection is deliberately a snapshot protocol.  Replies contain
 * copied metadata only; they never contain kernel pointers or live objects. */
#define MG_DEVICE_PROTOCOL_VERSION 2U
#define MG_DEVICE_REQUEST          ((u16)1U)
#define MG_DEVICE_RESPONSE         ((u16)2U)

#define MG_DEVICE_NAME_MAX         48U
#define MG_DEVICE_DRIVER_MAX       24U
#define MG_DEVICE_CONNECTION_MAX   16U
#define MG_DEVICE_MODEL_MAX        64U
#define MG_DEVICE_FILESYSTEM_MAX   16U
#define MG_DEVICE_LABEL_MAX        64U
#define MG_DEVICE_MOUNT_MAX        128U
#define MG_DEVICE_ROLE_MAX         24U
/* Keep the fixed 16-byte response header plus copied snapshot records within
 * the 1008-byte IPC payload after mountpoint paths were widened enough for
 * the full bounded MGFS label-derived /vol path. */
#define MG_DEVICE_RESPONSE_MAX     2U

typedef enum {
    MG_DEVICE_CATEGORY_ALL = 0,
    MG_DEVICE_CATEGORY_PCI = 1,
    MG_DEVICE_CATEGORY_USB = 2,
    MG_DEVICE_CATEGORY_BLOCK = 3,
    MG_DEVICE_CATEGORY_PARTITION = 4,
    MG_DEVICE_CATEGORY_NETWORK = 5,
    MG_DEVICE_CATEGORY_INPUT = 6,
} mg_device_category_t;

/* This filter is used only by LIST_BLOCK and includes raw devices and
 * partitions.  It is not a user-visible device category. */
#define MG_DEVICE_FILTER_BLOCKS ((u32)0x100U)

typedef enum {
    MG_DEVICE_STATE_PRESENT = 1,
    MG_DEVICE_STATE_REMOVED = 2,
} mg_device_state_t;

#define MG_DEVICE_FLAG_MOUNTED ((u32)1U << 0)
#define MG_DEVICE_FLAG_ROOT    ((u32)1U << 1)
#define MG_DEVICE_FLAG_BOOT    ((u32)1U << 2)
#define MG_DEVICE_FLAG_REMOVABLE ((u32)1U << 3)
#define MG_DEVICE_FLAG_SYSTEM_MANAGED ((u32)1U << 4)
#define MG_DEVICE_FLAG_AUTOMOUNT_SUPPRESSED ((u32)1U << 5)
#define MG_DEVICE_FLAG_READ_ONLY ((u32)1U << 6)

typedef struct PACKED {
    u64 id;
    u64 parent_id;
    u64 size_bytes;
    u64 first_lba;
    u64 last_lba;
    u32 category;
    u32 state;
    u32 flags;
    u32 block_size;
    u16 vendor_id;
    u16 device_id;
    u16 bus;
    u16 slot;
    u16 function;
    u8 class_code;
    u8 subclass;
    u8 prog_if;
    u8 revision;
    u8 usb_speed;
    /* Boot-local presentation metadata for storage hierarchy clients. */
    u8 disk_number;
    u8 partition_number;
    char name[MG_DEVICE_NAME_MAX];
    char driver[MG_DEVICE_DRIVER_MAX];
    /* Storage-origin metadata is copied from the authoritative block
     * object.  It is presentation data, not a persistent device name. */
    char connection[MG_DEVICE_CONNECTION_MAX];
    char model[MG_DEVICE_MODEL_MAX];
    char filesystem[MG_DEVICE_FILESYSTEM_MAX];
    char label[MG_DEVICE_LABEL_MAX];
    char mount_point[MG_DEVICE_MOUNT_MAX];
    char role[MG_DEVICE_ROLE_MAX];
} mg_device_info_t;

typedef struct PACKED {
    u16 version;
    u16 operation;
    u32 category;
    u64 device_id;
    u32 offset;
    u32 limit;
    u64 snapshot_generation;
} mg_device_request_t;

typedef struct PACKED {
    i32 result;
    u32 count;
    u32 total;
    u32 next_offset;
    u64 snapshot_generation;
    mg_device_info_t devices[MG_DEVICE_RESPONSE_MAX];
} mg_device_response_t;

/* Low-level kernel discovery query used by the trusted deviced service.  The
 * pointers are syscall arguments only and are never carried in IPC. */
typedef struct {
    u32 filter;
    u32 reserved;
    u64 device_id;
    u32 offset;
    u32 result_capacity;
    mg_device_info_t *result;
    u32 *out_total;
    /* Optional coherent-snapshot transaction.  A zero request starts a new
     * snapshot; later pages must return the same generation or be retried. */
    u64 snapshot_generation;
    u64 *out_snapshot_generation;
} mg_device_snapshot_request_t;

enum {
    MG_DEVICE_OP_LIST_ALL = 1,
    MG_DEVICE_OP_LIST_CATEGORY = 2,
    MG_DEVICE_OP_GET = 3,
    MG_DEVICE_OP_LIST_BLOCK = 4,
};

typedef char mg_device_request_size_check[
    sizeof(mg_device_request_t) <= MG_IPC_MESSAGE_PAYLOAD_MAX ? 1 : -1];
typedef char mg_device_response_size_check[
    sizeof(mg_device_response_t) <= MG_IPC_MESSAGE_PAYLOAD_MAX ? 1 : -1];

mg_result_t device_snapshot(mg_device_snapshot_request_t *request);
