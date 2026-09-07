/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/error.h>
#include <mg/ipc.h>
#include <mg/log_service.h>
#include <mg/pass.h>
#include <mg/process.h>
#include <mg/session.h>
#include <mg/session_service.h>
#include <mangrove.h>
#include <mangrove_version.h>
#include <stdio.h>
#include <string.h>
#include "../common/secret_input.h"

#define LOGIN_TEXT_CAPACITY 129U
#define LOGIN_RETRY_DELAY_MS 500U

static void log_login_event(mg_log_severity_t severity, const char *event,
                            const char *username)
{
    char message[MG_LOG_MESSAGE_MAX];

    if (!event || !username) return;
    if (snprintf(message, sizeof(message), "%s for account %s", event,
                 username) < 0) return;
    (void)mg_log_submit(severity, message);
}

static void print_system_welcome(void)
{
    printf("%s %s\n\nType 'help' for commands.\n\n",
           MANGROVE_NAME, MANGROVE_VERSION);
}

static void login_retry_delay(void)
{
    u64 deadline = uptime_ms() + LOGIN_RETRY_DELAY_MS;
    while (uptime_ms() < deadline) (void)process_yield();
}

static mg_result_t sessiond_create(const char *username,
                                   mg_session_info_t *session)
{
    mg_handle_t endpoint = 0;
    mg_session_request_t session_request = {0};
    mg_session_response_t session_response = {0};
    mg_ipc_message_t request = {0};
    mg_ipc_message_t reply = {0};
    mg_result_t result;

    if (!username || !session || strlen(username) >=
        sizeof(session_request.username)) return MG_ERR_BAD_ARGUMENT;
    result = service_lookup("session", &endpoint);
    if (result != MG_OK) return result;
    session_request.version = MG_SESSION_PROTOCOL_VERSION;
    session_request.operation = MG_SESSION_OP_CREATE;
    strncpy(session_request.username, username,
            sizeof(session_request.username) - 1U);
    request.version = MG_IPC_PROTOCOL_VERSION;
    request.type = MG_SESSION_REQUEST;
    request.payload_length = sizeof(session_request);
    memcpy(request.payload, &session_request, sizeof(session_request));
    result = ipc_request(endpoint, &request, &reply);
    (void)handle_close(endpoint);
    if (result != MG_OK) return result;
    if (reply.version != MG_IPC_PROTOCOL_VERSION ||
        reply.type != MG_SESSION_RESPONSE ||
        reply.payload_length != sizeof(session_response))
        return MG_ERR_PROTOCOL;
    memcpy(&session_response, reply.payload, sizeof(session_response));
    if (session_response.result != MG_OK || session_response.count != 1U ||
        session_response.sessions[0].id == 0)
        return session_response.result != MG_OK ? session_response.result :
               MG_ERR_PROTOCOL;
    session->id = session_response.sessions[0].id;
    session->shell = 0;
    return MG_OK;
}

static mg_result_t sessiond_end(mg_session_id_t session_id)
{
    mg_handle_t endpoint = 0;
    mg_session_request_t session_request = {0};
    mg_session_response_t session_response = {0};
    mg_ipc_message_t request = {0};
    mg_ipc_message_t reply = {0};
    mg_result_t result;

    if (!session_id) return MG_ERR_BAD_ARGUMENT;
    result = service_lookup("session", &endpoint);
    if (result != MG_OK) return result;
    session_request.version = MG_SESSION_PROTOCOL_VERSION;
    session_request.operation = MG_SESSION_OP_END;
    session_request.session_id = session_id;
    request.version = MG_IPC_PROTOCOL_VERSION;
    request.type = MG_SESSION_REQUEST;
    request.payload_length = sizeof(session_request);
    memcpy(request.payload, &session_request, sizeof(session_request));
    result = ipc_request(endpoint, &request, &reply);
    (void)handle_close(endpoint);
    if (result != MG_OK) return result;
    if (reply.version != MG_IPC_PROTOCOL_VERSION ||
        reply.type != MG_SESSION_RESPONSE ||
        reply.payload_length != sizeof(session_response))
        return MG_ERR_PROTOCOL;
    memcpy(&session_response, reply.payload, sizeof(session_response));
    return session_response.result;
}

static mg_result_t authenticate_manual(mg_identity_t *identity)
{
    char username[LOGIN_TEXT_CAPACITY];
    char password[LOGIN_TEXT_CAPACITY];

    if (!identity) return MG_ERR_BAD_ARGUMENT;
    for (;;) {
        mg_result_t result;

        memset(username, 0, sizeof(username));
        memset(password, 0, sizeof(password));
        printf("Username: ");
        result = read_console_line(username, sizeof(username), true);
        if (result_is_error(result)) {
            clear_secret(username, sizeof(username));
            clear_secret(password, sizeof(password));
            return result;
        }
        printf("Password: ");
        result = read_hidden_line(password, sizeof(password));
        if (result_is_error(result)) {
            clear_secret(username, sizeof(username));
            clear_secret(password, sizeof(password));
            return result;
        }
        result = pass_authenticate_account(username, password, identity);
        clear_secret(password, sizeof(password));
        if (!result_is_error(result)) {
            log_login_event(MG_LOG_INFO, "login authentication succeeded",
                            identity->username);
            clear_secret(username, sizeof(username));
            return MG_OK;
        }
        log_login_event(MG_LOG_WARNING, "login authentication failed",
                        username);
        clear_secret(username, sizeof(username));
        printf("Authentication failed.\n");
        login_retry_delay();
    }
}

static mg_result_t begin_session(const mg_identity_t *identity,
                                 mg_session_info_t *session)
{
    mg_result_t result;

    if (!identity || !session || !identity->username[0])
        return MG_ERR_BAD_ARGUMENT;
    result = sessiond_create(identity->username, session);
    if (result != MG_OK) {
        log_login_event(MG_LOG_ERROR, "session creation failed",
                        identity->username);
        return result;
    }
    result = session_launch_shell(session->id, &session->shell);
    if (result != MG_OK) {
        log_login_event(MG_LOG_ERROR, "session shell launch failed",
                        identity->username);
        (void)sessiond_end(session->id);
        memset(session, 0, sizeof(*session));
    }
    return result;
}

static void run_login_loop(void)
{
    print_system_welcome();
    for (;;) {
        mg_identity_t identity = {0};
        mg_session_info_t session = {0};
        mg_result_t result;
        i32 status = 0;

        result = session_autologin_identity(&identity);
        if (result_is_error(result))
            result = authenticate_manual(&identity);
        else
            log_login_event(MG_LOG_INFO, "autologin selected", identity.username);
        if (result_is_error(result)) {
            printf("Login unavailable.\n");
            login_retry_delay();
            continue;
        }
        result = begin_session(&identity, &session);
        if (result_is_error(result)) {
            printf("Session login unavailable.\n");
            login_retry_delay();
            continue;
        }

        result = process_wait(session.shell, &status);
        (void)handle_close(session.shell);
        (void)sessiond_end(session.id);
        if (result_is_error(result) || status != 0)
            printf("Session ended unexpectedly.\n");
    }
}

int main(void)
{
    run_login_loop();
    process_exit(1);
}
