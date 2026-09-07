/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/event.h>
#include <mg/types.h>

/* IPC messages are fixed-size, bounded, and contain no pointers.  The
 * request ID is ignored on submission and filled by the kernel on receive. */
#define MG_IPC_PROTOCOL_VERSION       1U
#define MG_IPC_SERVICE_NAME_MAX       32U
#define MG_IPC_MESSAGE_SIZE            1024U
#define MG_IPC_MESSAGE_PAYLOAD_MAX     1008U
#define MG_IPC_EVENT_MESSAGE            ((u16)0xfff0U)

enum {
    MG_IPC_DELIVERY_REQUEST = 0,
    MG_IPC_DELIVERY_EVENT = 1,
    MG_IPC_DELIVERY_EVENT_OVERFLOW = 2,
};

typedef struct PACKED {
    u16 version;
    u16 type;
    u32 payload_length;
    u64 request_id;
    u8 payload[MG_IPC_MESSAGE_PAYLOAD_MAX];
} mg_ipc_message_t;

/* This is kernel-authenticated metadata attached to a received request. */
typedef struct PACKED {
    u64 pid;
    u32 uid;
    u32 role;
    u64 session_id;
    u32 service_id;
    u32 service_privileges;
    u8 system_service;
    u8 reserved[7];
    char process_name[32];
} mg_ipc_requester_t;

typedef struct PACKED {
    mg_ipc_message_t message;
    mg_ipc_requester_t requester;
    mg_handle_t request;
    u32 delivery_kind;
} mg_ipc_received_t;

typedef char mg_ipc_message_size_check[
    sizeof(mg_ipc_message_t) == MG_IPC_MESSAGE_SIZE ? 1 : -1];

/* Resolve/register a named endpoint.  Registration is restricted to the
 * kernel-defined system service that owns the name. */
mg_result_t service_register(const char *name, mg_handle_t *out_endpoint);
mg_result_t service_lookup(const char *name, mg_handle_t *out_endpoint);

/* One synchronous request is allowed per calling process. */
mg_result_t ipc_request(mg_handle_t endpoint,
                        const mg_ipc_message_t *request,
                        mg_ipc_message_t *reply);
mg_result_t ipc_receive(mg_handle_t endpoint, mg_ipc_received_t *received);
/* Block until a request/event arrives or timeout_ms elapses. */
mg_result_t ipc_receive_timed(mg_handle_t endpoint,
                              mg_ipc_received_t *received,
                              u32 timeout_ms);
mg_result_t ipc_try_receive(mg_handle_t endpoint, mg_ipc_received_t *received);
mg_result_t ipc_reply(mg_handle_t request,
                      const mg_ipc_message_t *reply);

/* Kernel-originated event subscriptions are restricted to the owning
 * system services.  Events are delivered through ipc_receive(). */
mg_result_t ipc_event_subscribe(mg_handle_t endpoint, u32 event_classes);
mg_result_t ipc_event_unsubscribe(mg_handle_t endpoint);
