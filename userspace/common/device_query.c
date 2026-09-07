/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "device_query.h"

#include <mangrove.h>
#include <mg/ipc.h>
#include <string.h>

#define DEVICE_SNAPSHOT_RETRIES 4U

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

mg_result_t device_query_category(u32 category, mg_device_info_t *devices,
                                  u32 capacity, u32 *out_count)
{
    if (!devices || !out_count || !capacity) return MG_ERR_BAD_ARGUMENT;
    for (u32 attempt = 0; attempt < DEVICE_SNAPSHOT_RETRIES; attempt++) {
        u32 offset = 0;
        u64 snapshot_generation = 0;
        bool retry = false;

        for (;;) {
            mg_device_request_t request = {0};
            mg_device_response_t response = {0};
            u32 limit = capacity - offset;
            mg_result_t result;

            if (!limit) return MG_ERR_BUFFER_TOO_SMALL;
            if (limit > MG_DEVICE_RESPONSE_MAX) limit = MG_DEVICE_RESPONSE_MAX;
            request.version = MG_DEVICE_PROTOCOL_VERSION;
            request.operation = MG_DEVICE_OP_LIST_CATEGORY;
            request.category = category;
            request.offset = offset;
            request.limit = limit;
            request.snapshot_generation = snapshot_generation;
            result = device_call(&request, &response);
            if (result == MG_ERR_RETRY) {
                retry = true;
                break;
            }
            if (result != MG_OK) return result;
            if (response.count > limit || response.total < response.count ||
                !response.snapshot_generation ||
                (offset && response.snapshot_generation !=
                    snapshot_generation) ||
                response.next_offset && response.next_offset <= offset)
                return MG_ERR_PROTOCOL;
            if (!offset) snapshot_generation = response.snapshot_generation;
            if (response.count) {
                memcpy(devices + offset, response.devices,
                       response.count * sizeof(devices[0]));
                offset += response.count;
            }
            if (!response.next_offset) break;
            if (offset >= capacity || response.next_offset != offset)
                return MG_ERR_BUFFER_TOO_SMALL;
        }
        if (!retry) {
            *out_count = offset;
            return MG_OK;
        }
    }
    *out_count = 0;
    return MG_ERR_RETRY;
}
