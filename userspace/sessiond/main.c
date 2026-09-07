/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/error.h>
#include <mg/ipc.h>
#include <mg/log_service.h>
#include <mg/object.h>
#include <mg/process.h>
#include <mg/session.h>
#include <mg/session_service.h>
#include <mg/service.h>
#include <stdio.h>
#include <string.h>

static bool service_request_from_logind(const mg_ipc_received_t *received)
{
    return received && received->requester.system_service &&
           received->requester.service_id == MG_SERVICE_LOGIND;
}

static bool bounded_string(const char *value, usize capacity)
{
    if (!value || capacity == 0) return false;
    for (usize index = 0; index < capacity; index++) {
        if (value[index] == '\0') return index != 0;
    }
    return false;
}

static void log_session_event(mg_log_severity_t severity, const char *event,
                              const char *username)
{
    char message[MG_LOG_MESSAGE_MAX];

    if (!event) return;
    if (username) {
        if (snprintf(message, sizeof(message), "%s for account %s", event,
                     username) < 0) return;
        (void)mg_log_submit(severity, message);
    } else {
        (void)mg_log_submit(severity, event);
    }
}

static bool send_response(const mg_ipc_received_t *received,
                          const mg_session_response_t *response)
{
    mg_ipc_message_t message = {0};

    if (!received || !response) return false;
    message.version = MG_IPC_PROTOCOL_VERSION;
    message.type = MG_SESSION_RESPONSE;
    message.payload_length = sizeof(*response);
    memcpy(message.payload, response, sizeof(*response));
    return ipc_reply(received->request, &message) == MG_OK;
}

static void handle_request(const mg_ipc_received_t *received)
{
    mg_session_request_t request;
    mg_session_response_t response = {0};
    mg_session_info_t session = {0};
    mg_session_status_t status;
    u32 count = 0;
    u32 total = 0;
    mg_result_t result;

    if (!received || received->message.type != MG_SESSION_REQUEST ||
        received->message.payload_length != sizeof(request)) {
        response.result = MG_ERR_PROTOCOL;
        (void)send_response(received, &response);
        return;
    }
    memcpy(&request, received->message.payload, sizeof(request));
    if (!service_request_from_logind(received) ||
        request.version != MG_SESSION_PROTOCOL_VERSION) {
        response.result = MG_ERR_PRIVILEGE_REQUIRED;
        (void)send_response(received, &response);
        return;
    }

    switch (request.operation) {
        case MG_SESSION_OP_CREATE:
            if (!bounded_string(request.username,
                                sizeof(request.username))) {
                result = MG_ERR_BAD_ARGUMENT;
                break;
            }
            result = session_create_from_request(received->request,
                                                 request.username, &session);
            if (result == MG_OK) {
                log_session_event(MG_LOG_INFO, "session created",
                                  request.username);
                response.count = 1;
                response.total = 1;
                response.sessions[0].id = session.id;
            } else
                log_session_event(MG_LOG_WARNING, "session creation failed",
                                  request.username);
            break;
        case MG_SESSION_OP_END:
            if (request.session_id == 0) {
                result = MG_ERR_BAD_ARGUMENT;
                break;
            }
            result = session_end(request.session_id);
            if (result == MG_OK)
                log_session_event(MG_LOG_INFO, "session ended", NULL);
            else
                log_session_event(MG_LOG_ERROR, "session teardown failed",
                                  NULL);
            break;
        case MG_SESSION_OP_QUERY:
            result = session_query(request.session_id, &status);
            if (result == MG_OK) {
                response.count = 1;
                response.total = 1;
                response.sessions[0] = status;
            }
            break;
        case MG_SESSION_OP_LIST:
            if (request.limit == 0 || request.limit > MG_SESSION_MAX_RESULTS) {
                result = MG_ERR_BAD_ARGUMENT;
                break;
            }
            result = session_list(request.offset, response.sessions,
                                  request.limit, &count, &total);
            if (result == MG_OK) {
                response.count = count;
                response.total = total;
            }
            break;
        default:
            result = MG_ERR_BAD_ARGUMENT;
            break;
    }
    response.result = result;
    (void)send_response(received, &response);
}

int main(void)
{
    mg_handle_t endpoint = 0;
    mg_ipc_received_t received;
    mg_result_t result = service_register("session", &endpoint);

    if (result != MG_OK) {
        printf("Sessiond: endpoint registration failed: %s\n",
               error_string(result));
        process_exit(1);
    }
    for (;;) {
        result = ipc_receive(endpoint, &received);
        if (result != MG_OK) {
            (void)handle_close(endpoint);
            process_exit(1);
        }
        if (received.delivery_kind == MG_IPC_DELIVERY_REQUEST) {
            handle_request(&received);
            (void)handle_close(received.request);
        }
    }
}
