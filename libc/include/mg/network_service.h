#pragma once

#include <mg/ipc.h>
#include <mg/net.h>

/* The network service protocol is intentionally compact and binary.  The
 * generic IPC envelope supplies the transport/version; this protocol version
 * remains available for network-specific extensions. */
#define MG_NETWORK_PROTOCOL_VERSION 1U
#define MG_NETWORK_REQUEST  ((u16)1U)
#define MG_NETWORK_RESPONSE ((u16)2U)

#define MG_NETWORK_MAX_INTERFACES 4U
#define MG_NETWORK_RESULT_MAX     768U

typedef enum {
    MG_NETWORK_OP_STATUS = 1,
    MG_NETWORK_OP_STATUS_INTERFACE,
    MG_NETWORK_OP_INTERFACES,
    MG_NETWORK_OP_ROUTES,
    MG_NETWORK_OP_NEIGHBORS,
    MG_NETWORK_OP_CONNECTIONS,
    MG_NETWORK_OP_SET_AUTOMATIC,
    MG_NETWORK_OP_SET_MANUAL,
    MG_NETWORK_OP_ENABLE,
    MG_NETWORK_OP_DISABLE,
    MG_NETWORK_OP_RENEW,
    MG_NETWORK_OP_RELOAD,
} mg_network_operation_t;

typedef struct PACKED {
    u16 version;
    u16 operation;
    u32 flags;
    u32 timeout_ms;
    char interface_name[MG_NET_NAME_MAX];
    mg_net_manual_config_t manual;
} mg_network_request_t;

typedef struct PACKED {
    mg_net_info_t info;
    mg_net_interface_info_t interfaces[MG_NETWORK_MAX_INTERFACES];
} mg_network_status_t;

typedef struct PACKED {
    i32 result;
    u32 count;
    u8 data[MG_NETWORK_RESULT_MAX];
} mg_network_response_t;

typedef char mg_network_request_size_check[
    sizeof(mg_network_request_t) <= MG_IPC_MESSAGE_PAYLOAD_MAX ? 1 : -1];
typedef char mg_network_response_size_check[
    sizeof(mg_network_response_t) <= MG_IPC_MESSAGE_PAYLOAD_MAX ? 1 : -1];

/* Used only by the kernel-defined networkd service to enter the kernel-owned
 * PASS boundary for the already-delivered IPC request. */
mg_result_t pass_authorize_network_request(mg_handle_t request, u32 operation);
