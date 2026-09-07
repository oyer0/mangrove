/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "storage_snapshot.h"

#include <mangrove.h>
#include <mg/ipc.h>
#include <string.h>

#define STORAGE_SNAPSHOT_RETRIES 4U

static mg_result_t device_call(const mg_device_request_t *request,
                               mg_device_response_t *response)
{
    mg_handle_t endpoint;
    mg_ipc_message_t message = {0};
    mg_ipc_message_t reply = {0};
    mg_result_t result;

    if (!request || !response) return MG_ERR_BAD_ARGUMENT;
    result = service_lookup("device", &endpoint);
    if (result != MG_OK) return result;
    message.version = MG_IPC_PROTOCOL_VERSION;
    message.type = MG_DEVICE_REQUEST;
    message.payload_length = sizeof(*request);
    memcpy(message.payload, request, sizeof(*request));
    result = ipc_request(endpoint, &message, &reply);
    (void)handle_close(endpoint);
    if (result != MG_OK) return result;
    if (reply.version != MG_IPC_PROTOCOL_VERSION ||
        reply.type != MG_DEVICE_RESPONSE ||
        reply.payload_length != sizeof(*response)) return MG_ERR_PROTOCOL;
    memcpy(response, reply.payload, sizeof(*response));
    return response->result;
}

mg_result_t storage_snapshot_read(mg_device_info_t *devices, u32 capacity,
                                  u32 *out_count)
{
    if (!devices || !out_count || !capacity) return MG_ERR_BAD_ARGUMENT;
    for (u32 attempt = 0; attempt < STORAGE_SNAPSHOT_RETRIES; attempt++) {
        u32 count = 0;
        u32 offset = 0;
        u32 total = 0;
        u64 snapshot_generation = 0;
        bool first_page = true;
        bool retry = false;

        for (;;) {
            mg_device_request_t request = {0};
            mg_device_response_t response = {0};
            mg_result_t result;

            request.version = MG_DEVICE_PROTOCOL_VERSION;
            request.operation = MG_DEVICE_OP_LIST_BLOCK;
            request.offset = offset;
            request.limit = MG_DEVICE_RESPONSE_MAX;
            request.snapshot_generation = snapshot_generation;
            result = device_call(&request, &response);
            if (result == MG_ERR_RETRY) {
                retry = true;
                break;
            }
            if (result != MG_OK) return result;
            /* Replies are service data, not trusted local state. */
            if (response.count > MG_DEVICE_RESPONSE_MAX ||
                !response.snapshot_generation ||
                (!first_page && (response.total != total ||
                    response.snapshot_generation != snapshot_generation)) ||
                offset > response.total ||
                response.count > response.total - offset ||
                (response.next_offset &&
                 (response.next_offset <= offset ||
                  response.next_offset > response.total)) ||
                (!response.next_offset && response.count &&
                 offset + response.count < response.total) ||
                (response.count == 0U && response.next_offset != 0U))
                return MG_ERR_PROTOCOL;
            if (first_page) {
                total = response.total;
                snapshot_generation = response.snapshot_generation;
                first_page = false;
            }
            for (u32 index = 0; index < response.count; index++) {
                if (count == capacity) return MG_ERR_BUFFER_TOO_SMALL;
                devices[count++] = response.devices[index];
            }
            if (!response.next_offset) break;
            offset = response.next_offset;
        }
        if (!retry) {
            *out_count = count;
            return MG_OK;
        }
    }
    *out_count = 0;
    return MG_ERR_RETRY;
}
