/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <mg/ipc.h>

#define MG_CLIPBOARD_ENDPOINT "clipboard"
#define MG_CLIPBOARD_TEXT "text/plain;charset=utf-8"
#define MG_CLIPBOARD_FORMAT_MAX 64U
#define MG_CLIPBOARD_CHUNK 896U
#define MG_CLIPBOARD_VERSION 1U
#define MG_CLIPBOARD_REQUEST 1U
#define MG_CLIPBOARD_RESPONSE 2U

typedef enum {
    MG_CLIPBOARD_QUERY = 1, MG_CLIPBOARD_WRITE_BEGIN, MG_CLIPBOARD_WRITE,
    MG_CLIPBOARD_COMMIT, MG_CLIPBOARD_CANCEL, MG_CLIPBOARD_READ_BEGIN,
    MG_CLIPBOARD_READ, MG_CLIPBOARD_READ_END, MG_CLIPBOARD_CLEAR
} mg_clipboard_operation_t;

/* Control and bounded data messages share the existing IPC envelope. Offsets
 * and lengths count bytes, never cells. No payload contains caller identity. */
typedef struct PACKED {
    u32 version, operation;
    u64 token, offset, size;
    u32 count, reserved;
    char format[MG_CLIPBOARD_FORMAT_MAX];
    u8 data[MG_CLIPBOARD_CHUNK];
} mg_clipboard_request_t;
typedef struct PACKED {
    i32 result;
    u32 version;
    u64 token, size, generation;
    u32 count, formats;
    char format[MG_CLIPBOARD_FORMAT_MAX];
    u8 data[MG_CLIPBOARD_CHUNK];
} mg_clipboard_response_t;
#define MG_CLIPBOARD_CONTROL_BYTES 104U
typedef char mg_clipboard_header_size_check[
    sizeof(mg_clipboard_request_t) - MG_CLIPBOARD_CHUNK == MG_CLIPBOARD_CONTROL_BYTES &&
    sizeof(mg_clipboard_response_t) - MG_CLIPBOARD_CHUNK == MG_CLIPBOARD_CONTROL_BYTES ? 1 : -1];
typedef char mg_clipboard_request_size_check[
    sizeof(mg_clipboard_request_t) <= MG_IPC_MESSAGE_PAYLOAD_MAX ? 1 : -1];
typedef char mg_clipboard_response_size_check[
    sizeof(mg_clipboard_response_t) <= MG_IPC_MESSAGE_PAYLOAD_MAX ? 1 : -1];

typedef struct {
    u64 size, generation;
    u32 formats;
    char format[MG_CLIPBOARD_FORMAT_MAX];
} mg_clipboard_info_t;
/* Transfers own their endpoint handle; they must be closed by finish/abort.
 * An endpoint incarnation never changes during a transfer. Not thread-safe. */
typedef struct {
    mg_handle_t endpoint;
    u64 token, size, offset;
    bool writing;
} mg_clipboard_transfer_t;

mg_result_t clipboard_query_format(u32 index, mg_clipboard_info_t *info);
mg_result_t clipboard_query(mg_clipboard_info_t *info);
mg_result_t clipboard_clear(void);
mg_result_t clipboard_publish_begin(const char *format, u64 size,
                                     mg_clipboard_transfer_t *transfer);
mg_result_t clipboard_write(mg_clipboard_transfer_t *transfer,
                            const void *bytes, usize size);
mg_result_t clipboard_publish_commit(mg_clipboard_transfer_t *transfer);
mg_result_t clipboard_read_begin(const char *format,
                                  mg_clipboard_transfer_t *transfer);
mg_result_t clipboard_read(mg_clipboard_transfer_t *transfer, void *bytes,
                           usize capacity, usize *out_read);
/* Abort a publication or close a read snapshot. Always releases local handle. */
mg_result_t clipboard_transfer_close(mg_clipboard_transfer_t *transfer);
mg_result_t clipboard_set_text(const char *text, usize size);
/* Allocates exact bytes plus a convenience NUL; size excludes the NUL. A
 * caller-provided limit is its own capacity, not a system clipboard limit.
 * Failure leaves output parameters untouched. Release text with free(). */
mg_result_t clipboard_get_text(usize limit, char **text, usize *size);
