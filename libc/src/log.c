/* SPDX-License-Identifier: GPL-3.0-only */
#include <mangrove.h>

#include <mg/log_service.h>
#include <string.h>

mg_result_t mg_log_submit(mg_log_severity_t severity, const char *message)
{
    mg_handle_t endpoint = 0;
    mg_ipc_message_t request = {0};
    mg_ipc_message_t reply = {0};
    mg_log_request_t payload = {0};
    mg_log_response_t response;
    mg_result_t result;

    if (severity > MG_LOG_ERROR || !message || !message[0] ||
        strlen(message) >= MG_LOG_MESSAGE_MAX)
        return MG_ERR_BAD_ARGUMENT;
    result = service_lookup("log", &endpoint);
    if (result != MG_OK) return result;
    payload.version = MG_LOG_PROTOCOL_VERSION;
    payload.operation = MG_LOG_OP_SUBMIT;
    payload.severity = (u16)severity;
    memcpy(payload.message, message, strlen(message) + 1U);
    request.version = MG_IPC_PROTOCOL_VERSION;
    request.type = MG_LOG_REQUEST;
    request.payload_length = sizeof(payload);
    memcpy(request.payload, &payload, sizeof(payload));
    result = ipc_request(endpoint, &request, &reply);
    (void)handle_close(endpoint);
    if (result != MG_OK) return result;
    if (reply.version != MG_IPC_PROTOCOL_VERSION ||
        reply.type != MG_LOG_RESPONSE ||
        reply.payload_length != sizeof(response))
        return MG_ERR_PROTOCOL;
    memcpy(&response, reply.payload, sizeof(response));
    return response.result;
}

mg_result_t mg_log_debug(const char *message)
{
    return mg_log_submit(MG_LOG_DEBUG, message);
}

mg_result_t mg_log_info(const char *message)
{
    return mg_log_submit(MG_LOG_INFO, message);
}

mg_result_t mg_log_warning(const char *message)
{
    return mg_log_submit(MG_LOG_WARNING, message);
}

mg_result_t mg_log_error(const char *message)
{
    return mg_log_submit(MG_LOG_ERROR, message);
}
