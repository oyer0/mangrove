/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/clipboard.h>
#include <mg/object.h>
#include <stdlib.h>
#include <string.h>

static mg_result_t exchange(mg_handle_t endpoint, mg_clipboard_request_t *request,
                             mg_clipboard_response_t *response)
{
    mg_ipc_message_t message = {0}, reply;
    request->version = MG_CLIPBOARD_VERSION;
    message.version = MG_IPC_PROTOCOL_VERSION;
    message.type = MG_CLIPBOARD_REQUEST;
    message.payload_length = MG_CLIPBOARD_CONTROL_BYTES +
        (request->operation == MG_CLIPBOARD_WRITE ? request->count : 0);
    memcpy(message.payload, request, message.payload_length);
    mg_result_t result = ipc_request(endpoint, &message, &reply);
    if (result != MG_OK) return result;
    if (reply.version != MG_IPC_PROTOCOL_VERSION ||
        reply.type != MG_CLIPBOARD_RESPONSE ||
        reply.payload_length < MG_CLIPBOARD_CONTROL_BYTES ||
        reply.payload_length > sizeof(*response)) return MG_ERR_PROTOCOL;
    memset(response, 0, sizeof(*response));
    memcpy(response, reply.payload, reply.payload_length);
    if (response->version != MG_CLIPBOARD_VERSION ||
        response->count > MG_CLIPBOARD_CHUNK ||
        reply.payload_length != MG_CLIPBOARD_CONTROL_BYTES + response->count ||
        !memchr(response->format, 0, sizeof(response->format)))
        return MG_ERR_PROTOCOL;
    return response->result;
}

static mg_result_t once(mg_clipboard_request_t *request,
                         mg_clipboard_response_t *response)
{
    mg_handle_t endpoint;
    mg_result_t result = service_lookup(MG_CLIPBOARD_ENDPOINT, &endpoint);
    if (result != MG_OK) return result;
    result = exchange(endpoint, request, response);
    (void)handle_close(endpoint);
    return result;
}

mg_result_t clipboard_query_format(u32 index, mg_clipboard_info_t *info)
{
    mg_clipboard_request_t request = {.operation = MG_CLIPBOARD_QUERY, .offset = index};
    mg_clipboard_response_t response;
    if (!info) return MG_ERR_BAD_ARGUMENT;
    mg_result_t result = once(&request, &response);
    if (result == MG_OK) {
        info->size = response.size;
        info->generation = response.generation;
        info->formats = response.formats;
        memcpy(info->format, response.format, sizeof(info->format));
    }
    return result;
}

mg_result_t clipboard_query(mg_clipboard_info_t *info)
{ return clipboard_query_format(0, info); }

mg_result_t clipboard_clear(void)
{
    mg_clipboard_request_t request = {.operation = MG_CLIPBOARD_CLEAR};
    mg_clipboard_response_t response;
    return once(&request, &response);
}

static mg_result_t begin(const char *format, u64 size, bool writing,
                         mg_clipboard_transfer_t *transfer)
{
    mg_clipboard_request_t request = {0};
    mg_clipboard_response_t response;
    mg_handle_t endpoint;
    if (!format || !transfer || !*format ||
        strlen(format) >= sizeof(request.format)) return MG_ERR_BAD_ARGUMENT;
    request.operation = writing ? MG_CLIPBOARD_WRITE_BEGIN : MG_CLIPBOARD_READ_BEGIN;
    request.size = size;
    memcpy(request.format, format, strlen(format) + 1U);
    mg_result_t result = service_lookup(MG_CLIPBOARD_ENDPOINT, &endpoint);
    if (result != MG_OK) return result;
    result = exchange(endpoint, &request, &response);
    if (result == MG_OK && (!response.token ||
        (writing && response.size != size))) result = MG_ERR_PROTOCOL;
    if (result != MG_OK) { (void)handle_close(endpoint); return result; }
    *transfer = (mg_clipboard_transfer_t){endpoint, response.token,
                                         response.size, 0, writing};
    return MG_OK;
}
mg_result_t clipboard_publish_begin(const char *format, u64 size,
                                     mg_clipboard_transfer_t *transfer)
{ return begin(format, size, true, transfer); }
mg_result_t clipboard_read_begin(const char *format,
                                  mg_clipboard_transfer_t *transfer)
{ return begin(format, 0, false, transfer); }

mg_result_t clipboard_write(mg_clipboard_transfer_t *transfer,
                            const void *bytes, usize size)
{
    if (!transfer || !transfer->endpoint || !transfer->writing ||
        (size && !bytes) || transfer->offset > transfer->size ||
        size > transfer->size - transfer->offset) return MG_ERR_BAD_ARGUMENT;
    const u8 *source = bytes;
    while (size) {
        mg_clipboard_request_t request = {.operation = MG_CLIPBOARD_WRITE,
            .token = transfer->token, .offset = transfer->offset};
        mg_clipboard_response_t response;
        request.count = size < MG_CLIPBOARD_CHUNK ? size : MG_CLIPBOARD_CHUNK;
        memcpy(request.data, source, request.count);
        mg_result_t result = exchange(transfer->endpoint, &request, &response);
        if (result != MG_OK) return result;
        transfer->offset += request.count;
        source += request.count;
        size -= request.count;
    }
    return MG_OK;
}

mg_result_t clipboard_transfer_close(mg_clipboard_transfer_t *transfer)
{
    if (!transfer || !transfer->endpoint) return MG_ERR_BAD_ARGUMENT;
    mg_clipboard_request_t request = {.operation = transfer->writing
        ? MG_CLIPBOARD_CANCEL : MG_CLIPBOARD_READ_END, .token = transfer->token};
    mg_clipboard_response_t response;
    mg_result_t result = exchange(transfer->endpoint, &request, &response);
    (void)handle_close(transfer->endpoint);
    memset(transfer, 0, sizeof(*transfer));
    return result;
}

mg_result_t clipboard_publish_commit(mg_clipboard_transfer_t *transfer)
{
    if (!transfer || !transfer->endpoint || !transfer->writing ||
        transfer->offset != transfer->size) return MG_ERR_BAD_ARGUMENT;
    mg_clipboard_request_t request = {.operation = MG_CLIPBOARD_COMMIT,
                                    .token = transfer->token};
    mg_clipboard_response_t response;
    mg_result_t result = exchange(transfer->endpoint, &request, &response);
    /* Never replay a commit after an ambiguous lost reply. Close/cancel does
     * not withdraw a committed item. */
    if (result != MG_OK) (void)clipboard_transfer_close(transfer);
    else {
        (void)handle_close(transfer->endpoint);
        memset(transfer, 0, sizeof(*transfer));
    }
    return result;
}

mg_result_t clipboard_read(mg_clipboard_transfer_t *transfer, void *bytes,
                           usize capacity, usize *out_read)
{
    if (!transfer || !transfer->endpoint || transfer->writing || !out_read ||
        (capacity && !bytes) || transfer->offset > transfer->size)
        return MG_ERR_BAD_ARGUMENT;
    usize total = 0;
    while (total < capacity && transfer->offset < transfer->size) {
        mg_clipboard_request_t request = {.operation = MG_CLIPBOARD_READ,
            .token = transfer->token, .offset = transfer->offset};
        mg_clipboard_response_t response;
        usize amount = capacity - total;
        request.count = amount < MG_CLIPBOARD_CHUNK ? amount : MG_CLIPBOARD_CHUNK;
        mg_result_t result = exchange(transfer->endpoint, &request, &response);
        if (result != MG_OK) { *out_read = total; return result; }
        u64 remaining = transfer->size - transfer->offset;
        u32 expected = remaining < request.count ? remaining : request.count;
        if (response.count != expected) { *out_read = total; return MG_ERR_PROTOCOL; }
        memcpy((u8 *)bytes + total, response.data, response.count);
        transfer->offset += response.count;
        total += response.count;
    }
    *out_read = total;
    return MG_OK;
}

mg_result_t clipboard_set_text(const char *text, usize size)
{
    if (size && !text) return MG_ERR_BAD_ARGUMENT;
    mg_clipboard_transfer_t transfer;
    mg_result_t result = clipboard_publish_begin(MG_CLIPBOARD_TEXT, size, &transfer);
    if (result != MG_OK) return result;
    result = clipboard_write(&transfer, text, size);
    if (result != MG_OK) { (void)clipboard_transfer_close(&transfer); return result; }
    return clipboard_publish_commit(&transfer);
}

mg_result_t clipboard_get_text(usize limit, char **text, usize *size)
{
    if (!text || !size) return MG_ERR_BAD_ARGUMENT;
    mg_clipboard_transfer_t transfer;
    mg_result_t result = clipboard_read_begin(MG_CLIPBOARD_TEXT, &transfer);
    if (result != MG_OK) return result;
    if (transfer.size > limit || transfer.size >= ~(usize)0) {
        (void)clipboard_transfer_close(&transfer);
        return MG_ERR_BUFFER_TOO_SMALL;
    }
    usize length = (usize)transfer.size, copied;
    char *output = malloc(length + 1U);
    if (!output) { (void)clipboard_transfer_close(&transfer); return MG_ERR_NO_MEMORY; }
    result = clipboard_read(&transfer, output, length, &copied);
    mg_result_t closed = clipboard_transfer_close(&transfer);
    if (result == MG_OK) result = closed;
    if (result != MG_OK) { free(output); return result; }
    output[length] = 0;
    *text = output;
    *size = length;
    return MG_OK;
}
