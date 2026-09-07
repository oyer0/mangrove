/* SPDX-License-Identifier: GPL-3.0-only */
#include "volume_client.h"

#include <mangrove.h>
#include <string.h>

mg_result_t volume_client_call(u16 operation, const char *target,
                               mg_volume_operation_response_t *response)
{
    mg_handle_t endpoint;
    mg_volume_request_t volume_request = {0};
    mg_ipc_message_t request = {0};
    mg_ipc_message_t reply = {0};
    mg_result_t result;

    if (!target || !target[0] || strlen(target) >= MG_VOLUME_MOUNT_MAX ||
        !response || (operation != MG_VOLUME_OP_MOUNT &&
                      operation != MG_VOLUME_OP_UNMOUNT &&
                      operation != MG_VOLUME_OP_EJECT))
        return MG_ERR_BAD_ARGUMENT;
    result = service_lookup("volume", &endpoint);
    if (result != MG_OK) return result;
    volume_request.version = MG_VOLUME_PROTOCOL_VERSION;
    volume_request.operation = operation;
    strncpy(volume_request.target, target,
            sizeof(volume_request.target) - 1U);
    request.version = MG_IPC_PROTOCOL_VERSION;
    request.type = MG_VOLUME_REQUEST;
    request.payload_length = sizeof(volume_request);
    memcpy(request.payload, &volume_request, sizeof(volume_request));
    result = ipc_request(endpoint, &request, &reply);
    (void)handle_close(endpoint);
    if (result != MG_OK) return result;
    if (reply.version != MG_IPC_PROTOCOL_VERSION ||
        reply.type != MG_VOLUME_RESPONSE ||
        reply.payload_length != sizeof(*response)) return MG_ERR_PROTOCOL;
    memcpy(response, reply.payload, sizeof(*response));
    return response->result;
}
