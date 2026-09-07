/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/ipc.h>
#include <mg/time.h>

/* The log protocol is deliberately small.  Records are fixed-width, all
 * text is bounded, and the storage format is private to logd. */
#define MG_LOG_PROTOCOL_VERSION 2U
#define MG_LOG_SOURCE_MAX       32U
#define MG_LOG_MESSAGE_MAX      160U
#define MG_LOG_QUERY_MAX        3U
#define MG_LOG_RECORD_MAGIC     0x4d474c31U /* "MGL1" */
#define MG_LOG_RECORD_VERSION   1U

#define MG_LOG_REQUEST  ((u16)1U)
#define MG_LOG_RESPONSE ((u16)2U)

typedef enum {
    MG_LOG_OP_SUBMIT = 1,
    MG_LOG_OP_QUERY,
    MG_LOG_OP_QUERY_BOOT,
} mg_log_operation_t;

typedef enum {
    MG_LOG_DEBUG = 0,
    MG_LOG_INFO,
    MG_LOG_WARNING,
    MG_LOG_ERROR,
} mg_log_severity_t;

#define MG_LOG_SEVERITY_ANY ((u16)0xffffU)

typedef struct PACKED {
    u32 version;
    u16 operation;
    u16 severity;
    u32 offset;
    char source[MG_LOG_SOURCE_MAX];
    char message[MG_LOG_MESSAGE_MAX];
} mg_log_request_t;

/* This is the structured logical record returned by logd.  The fixed size is
 * intentional: several records plus the response header must fit in one IPC
 * message, without exposing storage offsets or pointers. */
typedef struct PACKED {
    u64 sequence;
    u64 boot_id;
    u8 realtime_available;
    u8 reserved[7];
    mg_mangrove_time_t realtime;
    u64 monotonic_ms;
    u32 severity;
    u16 source_length;
    u16 message_length;
    u64 pid;
    char source[MG_LOG_SOURCE_MAX];
    char message[MG_LOG_MESSAGE_MAX];
} mg_log_record_t;

typedef struct PACKED {
    u32 magic;
    u16 version;
    u16 header_length;
    u32 record_length;
    u32 checksum;
    mg_log_record_t record;
} mg_log_persistent_record_t;

typedef struct PACKED {
    i32 result;
    u32 count;
    u32 total;
    u32 next_offset;
    mg_log_record_t records[MG_LOG_QUERY_MAX];
} mg_log_response_t;

typedef char mg_log_request_size_check[
    sizeof(mg_log_request_t) <= MG_IPC_MESSAGE_PAYLOAD_MAX ? 1 : -1];
typedef char mg_log_response_size_check[
    sizeof(mg_log_response_t) <= MG_IPC_MESSAGE_PAYLOAD_MAX ? 1 : -1];
typedef char mg_log_record_size_check[
    sizeof(mg_log_record_t) == 256U ? 1 : -1];

/* Best-effort submission for services and future application logging.  The
 * source identity is attached by logd from the kernel-authenticated caller,
 * not taken from a caller-controlled source string. */
mg_result_t mg_log_submit(mg_log_severity_t severity, const char *message);
mg_result_t mg_log_debug(const char *message);
mg_result_t mg_log_info(const char *message);
mg_result_t mg_log_warning(const char *message);
mg_result_t mg_log_error(const char *message);
