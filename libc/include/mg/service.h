#pragma once

#include <mg/error.h>
#include <mg/ipc.h>

typedef u32 mg_service_id_t;

#define MG_SERVICE_SESSIOND ((mg_service_id_t)1U)
#define MG_SERVICE_SPROUT   ((mg_service_id_t)2U)
#define MG_SERVICE_NETWORKD ((mg_service_id_t)3U)
#define MG_SERVICE_DEVICED  ((mg_service_id_t)4U)
#define MG_SERVICE_LOGIND   ((mg_service_id_t)5U)
#define MG_SERVICE_LOGD     ((mg_service_id_t)6U)
#define MG_SERVICE_VOLUMED  ((mg_service_id_t)7U)

#define MG_SERVICE_CONTROL_REQUEST  ((u16)1U)
#define MG_SERVICE_CONTROL_RESPONSE ((u16)2U)

typedef enum {
    MG_SERVICE_OP_STATUS = 1,
    MG_SERVICE_OP_START,
    MG_SERVICE_OP_STOP,
    MG_SERVICE_OP_RESTART,
    MG_SERVICE_OP_RELOAD,
} mg_service_operation_t;

typedef enum {
    MG_SERVICE_STATE_STOPPED = 0,
    MG_SERVICE_STATE_STARTING,
    MG_SERVICE_STATE_RUNNING,
    MG_SERVICE_STATE_STOPPING,
    MG_SERVICE_STATE_FAILED,
} mg_service_state_t;

typedef enum {
    MG_SERVICE_WANTED_STOPPED = 0,
    MG_SERVICE_WANTED_RUNNING = 1,
} mg_service_wanted_t;

#define MG_SERVICE_STATUS_ESSENTIAL ((u32)1U << 0)
#define MG_SERVICE_STATUS_RELOAD    ((u32)1U << 1)
#define MG_SERVICE_STATUS_MAX       8U

typedef struct PACKED {
    u16 operation;
    u16 reserved;
    char service[MG_IPC_SERVICE_NAME_MAX];
} mg_service_control_request_t;

typedef struct PACKED {
    u32 id;
    u32 state;
    u32 wanted;
    u32 flags;
    u64 pid;
    u32 restart_count;
    char name[MG_IPC_SERVICE_NAME_MAX];
} mg_service_status_t;

typedef struct PACKED {
    i32 result;
    u32 count;
    mg_service_status_t services[MG_SERVICE_STATUS_MAX];
} mg_service_control_response_t;

typedef char mg_service_request_size_check[
    sizeof(mg_service_control_request_t) <= MG_IPC_MESSAGE_PAYLOAD_MAX ? 1 : -1];
typedef char mg_service_response_size_check[
    sizeof(mg_service_control_response_t) <= MG_IPC_MESSAGE_PAYLOAD_MAX ? 1 : -1];

/* Starts a kernel-defined system service.  Only PID 1 may request this. */
mg_result_t service_start(mg_service_id_t service_id, mg_handle_t *out_handle);

/* Restricted by the kernel to the real Sprout service.  This is the
 * service's internal handoff into the kernel-owned PASS boundary. */
mg_result_t pass_authorize_service_request(mg_handle_t request, u32 operation,
                                           mg_service_id_t service_id);
