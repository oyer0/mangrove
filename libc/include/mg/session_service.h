/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <mg/ipc.h>
#include <mg/session.h>

#define MG_SESSION_PROTOCOL_VERSION 1U
#define MG_SESSION_REQUEST           ((u16)1U)
#define MG_SESSION_RESPONSE          ((u16)2U)
#define MG_SESSION_MAX_RESULTS       8U

typedef enum {
    MG_SESSION_OP_CREATE = 1,
    MG_SESSION_OP_END,
    MG_SESSION_OP_QUERY,
    MG_SESSION_OP_LIST,
} mg_session_operation_t;

typedef enum {
    MG_SESSION_STATE_CREATING = 0,
    MG_SESSION_STATE_ACTIVE,
    MG_SESSION_STATE_ENDING,
    MG_SESSION_STATE_ENDED,
} mg_session_state_t;

typedef enum {
    MG_SESSION_ACTIVITY_INACTIVE = 0,
    MG_SESSION_ACTIVITY_ACTIVE = 1,
} mg_session_activity_t;

typedef struct PACKED {
    u32 version;
    u16 operation;
    u16 reserved;
    mg_session_id_t session_id;
    u32 offset;
    u32 limit;
    char username[MG_IDENTITY_USERNAME_CAPACITY];
} mg_session_request_t;

typedef struct PACKED mg_session_status {
    mg_session_id_t id;
    u32 uid;
    u32 role;
    u32 state;
    u32 activity;
    char username[MG_IDENTITY_USERNAME_CAPACITY];
} mg_session_status_t;

typedef struct PACKED {
    i32 result;
    u32 count;
    u32 total;
    mg_session_status_t sessions[MG_SESSION_MAX_RESULTS];
} mg_session_response_t;

typedef char mg_session_request_size_check[
    sizeof(mg_session_request_t) <= MG_IPC_MESSAGE_PAYLOAD_MAX ? 1 : -1];
typedef char mg_session_response_size_check[
    sizeof(mg_session_response_t) <= MG_IPC_MESSAGE_PAYLOAD_MAX ? 1 : -1];
