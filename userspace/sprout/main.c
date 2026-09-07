/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/error.h>
#include <mg/ipc.h>
#include <mg/log_service.h>
#include <mg/process.h>
#include <mg/service.h>
#include <mangrove.h>
#include <stdio.h>
#include <string.h>

#define SERVICE_RESTART_DELAY_MS   500U
#define SERVICE_RESTART_BACKOFF_MS 5000U
#define SERVICE_FAILURE_LIMIT      3U

typedef enum {
    SERVICE_RESTART_NEVER = 0,
    SERVICE_RESTART_ALWAYS,
} service_restart_policy_t;

typedef struct {
    mg_service_id_t id;
    const char *name;
    bool essential;
    bool supports_reload;
    service_restart_policy_t restart_policy;
} service_definition_t;

typedef struct {
    const service_definition_t *definition;
    mg_service_state_t state;
    mg_service_wanted_t wanted;
    mg_handle_t process;
    u64 pid;
    u32 restart_count;
    u32 failures;
    u64 restart_deadline;
    bool restart_after_stop;
} service_runtime_t;

/* Policy is static until a service-definition format is justified.  The
 * executable, identity, and capability mapping remain kernel-owned. */
static const service_definition_t service_definitions[] = {
    {MG_SERVICE_SESSIOND, "sessiond", true, false, SERVICE_RESTART_ALWAYS},
    {MG_SERVICE_LOGIND, "logind", true, false, SERVICE_RESTART_ALWAYS},
    {MG_SERVICE_LOGD, "logd", false, false, SERVICE_RESTART_ALWAYS},
    {MG_SERVICE_NETWORKD, "networkd", false, true, SERVICE_RESTART_ALWAYS},
    {MG_SERVICE_DEVICED, "deviced", false, false, SERVICE_RESTART_ALWAYS},
    {MG_SERVICE_VOLUMED, "volumed", false, false, SERVICE_RESTART_ALWAYS},
};

static service_runtime_t service_runtime[
    sizeof(service_definitions) / sizeof(service_definitions[0])];
static bool supervisor_bootstrap = true;

static const service_definition_t *definition_for_name(const char *name)
{
    for (usize index = 0;
         index < sizeof(service_definitions) / sizeof(service_definitions[0]);
         index++) {
        if (!strcmp(service_definitions[index].name, name))
            return &service_definitions[index];
    }
    return NULL;
}

static service_runtime_t *runtime_for_definition(
    const service_definition_t *definition)
{
    for (usize index = 0;
         index < sizeof(service_definitions) / sizeof(service_definitions[0]);
         index++) {
        if (service_runtime[index].definition == definition)
            return &service_runtime[index];
    }
    return NULL;
}

static void schedule_restart(service_runtime_t *runtime)
{
    u64 delay;

    if (!runtime || runtime->wanted != MG_SERVICE_WANTED_RUNNING ||
        runtime->definition->restart_policy == SERVICE_RESTART_NEVER)
        return;
    runtime->failures++;
    delay = runtime->failures >= SERVICE_FAILURE_LIMIT
        ? SERVICE_RESTART_BACKOFF_MS : SERVICE_RESTART_DELAY_MS;
    if (runtime->failures >= SERVICE_FAILURE_LIMIT) runtime->failures = 0;
    runtime->restart_deadline = uptime_ms() + delay;
}

static void log_supervisor_event(const service_runtime_t *runtime,
                                 const char *event)
{
    char message[64];

    if (supervisor_bootstrap) return;
    if (!runtime || !runtime->definition || !event) return;
    snprintf(message, sizeof(message), "service %s %s",
             runtime->definition->name, event);
    (void)mg_log_submit(MG_LOG_INFO, message);
}

static mg_result_t start_runtime(service_runtime_t *runtime, bool automatic)
{
    mg_handle_t process = 0;
    mg_result_t result;

    if (!runtime || !runtime->definition) return MG_ERR_BAD_ARGUMENT;
    if (runtime->state == MG_SERVICE_STATE_RUNNING ||
        runtime->state == MG_SERVICE_STATE_STARTING)
        return MG_OK;
    runtime->wanted = MG_SERVICE_WANTED_RUNNING;
    runtime->state = MG_SERVICE_STATE_STARTING;
    result = service_start(runtime->definition->id, &process);
    if (result_is_error(result)) {
        runtime->state = MG_SERVICE_STATE_FAILED;
        schedule_restart(runtime);
        return result;
    }
    runtime->pid = process_handle_pid(process);
    if (!runtime->pid) {
        (void)handle_close(process);
        runtime->state = MG_SERVICE_STATE_FAILED;
        schedule_restart(runtime);
        return MG_ERR_SERVICE_UNAVAILABLE;
    }
    runtime->process = process;
    runtime->restart_count += automatic ? 1U : 0U;
    runtime->state = MG_SERVICE_STATE_RUNNING;
    runtime->restart_deadline = 0;
    if (runtime->definition->id != MG_SERVICE_LOGD)
        log_supervisor_event(runtime, "started");
    return MG_OK;
}

static void finish_runtime(service_runtime_t *runtime, i32 status)
{
    mg_result_t result;

    if (!runtime) return;
    result = process_wait(runtime->process, &status);
    (void)handle_close(runtime->process);
    runtime->process = 0;
    runtime->pid = 0;
    if (result_is_error(result)) {
        runtime->state = MG_SERVICE_STATE_FAILED;
        if (runtime->wanted == MG_SERVICE_WANTED_RUNNING)
            schedule_restart(runtime);
        return;
    }
    if (runtime->restart_after_stop) {
        runtime->restart_after_stop = false;
        runtime->state = MG_SERVICE_STATE_STOPPED;
        (void)start_runtime(runtime, true);
    } else if (runtime->wanted == MG_SERVICE_WANTED_STOPPED) {
        runtime->state = MG_SERVICE_STATE_STOPPED;
        runtime->restart_deadline = 0;
    } else {
        runtime->state = MG_SERVICE_STATE_FAILED;
        schedule_restart(runtime);
    }
}

static bool poll_runtime(service_runtime_t *runtime)
{
    i32 status = 0;
    mg_result_t result;

    if (!runtime || (runtime->state != MG_SERVICE_STATE_RUNNING &&
                     runtime->state != MG_SERVICE_STATE_STOPPING))
        return false;
    result = process_poll(runtime->process, &status);
    if (result == MG_ERR_WOULD_BLOCK) return false;
    if (result_is_error(result)) {
        (void)handle_close(runtime->process);
        runtime->process = 0;
        runtime->pid = 0;
        runtime->state = MG_SERVICE_STATE_FAILED;
        if (runtime->wanted == MG_SERVICE_WANTED_RUNNING)
            schedule_restart(runtime);
        return true;
    }
    finish_runtime(runtime, status);
    return true;
}

static bool service_tick(service_runtime_t *runtime)
{
    bool changed = poll_runtime(runtime);

    if (runtime && runtime->state == MG_SERVICE_STATE_FAILED &&
        runtime->wanted == MG_SERVICE_WANTED_RUNNING &&
        runtime->restart_deadline != 0 &&
        uptime_ms() >= runtime->restart_deadline) {
        mg_result_t result = start_runtime(runtime, true);
        /* The runtime state/backoff records the failure.  Do not write a
         * repeated diagnostic into the interactive session's input stream;
         * a concise status query remains the diagnostic surface. */
        (void)result;
        changed = true;
    }
    return changed;
}

static mg_result_t authorize_operation(const mg_ipc_received_t *received,
                                       u32 operation,
                                       const service_definition_t *definition)
{
    return pass_authorize_service_request(received->request, operation,
                                           definition->id);
}

static mg_result_t request_network_reload(void)
{
    mg_handle_t endpoint = 0;
    mg_network_request_t network_request = {
        .version = MG_NETWORK_PROTOCOL_VERSION,
        .operation = MG_NETWORK_OP_RELOAD,
    };
    mg_network_response_t network_response;
    mg_ipc_message_t request = {0};
    mg_ipc_message_t reply = {0};
    mg_result_t result;

    result = service_lookup("network", &endpoint);
    if (result != MG_OK) return result;
    request.version = MG_IPC_PROTOCOL_VERSION;
    request.type = MG_NETWORK_REQUEST;
    request.payload_length = sizeof(network_request);
    memcpy(request.payload, &network_request, sizeof(network_request));
    result = ipc_request(endpoint, &request, &reply);
    (void)handle_close(endpoint);
    if (result != MG_OK) return result;
    if (reply.version != MG_IPC_PROTOCOL_VERSION ||
        reply.type != MG_NETWORK_RESPONSE ||
        reply.payload_length != sizeof(network_response))
        return MG_ERR_PROTOCOL;
    memcpy(&network_response, reply.payload, sizeof(network_response));
    return network_response.result;
}

static mg_result_t stop_runtime(service_runtime_t *runtime)
{
    mg_result_t result;

    if (!runtime) return MG_ERR_BAD_ARGUMENT;
    runtime->wanted = MG_SERVICE_WANTED_STOPPED;
    runtime->restart_deadline = 0;
    if (runtime->state == MG_SERVICE_STATE_STOPPED) return MG_OK;
    if (runtime->state == MG_SERVICE_STATE_FAILED) {
        runtime->state = MG_SERVICE_STATE_STOPPED;
        return MG_OK;
    }
    if (runtime->state != MG_SERVICE_STATE_RUNNING)
        return MG_ERR_BUSY;
    runtime->state = MG_SERVICE_STATE_STOPPING;
    result = process_terminate(runtime->process, -1);
    if (result_is_error(result)) {
        runtime->state = MG_SERVICE_STATE_FAILED;
        return result;
    }
    while (runtime->state == MG_SERVICE_STATE_STOPPING) {
        (void)poll_runtime(runtime);
        if (runtime->state == MG_SERVICE_STATE_STOPPING)
            (void)process_yield();
    }
    return runtime->state == MG_SERVICE_STATE_STOPPED
        ? MG_OK : MG_ERR_SERVICE_UNAVAILABLE;
}

static mg_result_t restart_runtime(service_runtime_t *runtime)
{
    mg_result_t result;

    if (!runtime) return MG_ERR_BAD_ARGUMENT;
    if (runtime->state == MG_SERVICE_STATE_STARTING ||
        runtime->state == MG_SERVICE_STATE_STOPPING)
        return MG_ERR_BUSY;
    if (runtime->state != MG_SERVICE_STATE_RUNNING)
        return start_runtime(runtime, false);

    runtime->wanted = MG_SERVICE_WANTED_RUNNING;
    runtime->restart_after_stop = true;
    runtime->state = MG_SERVICE_STATE_STOPPING;
    result = process_terminate(runtime->process, -1);
    if (result_is_error(result)) {
        runtime->restart_after_stop = false;
        runtime->state = MG_SERVICE_STATE_FAILED;
        return result;
    }
    /* The stop is a kernel process transition.  Yield until its single
     * child is reaped, then start the replacement without another
     * authorization decision for this same logical restart request. */
    while (runtime->state == MG_SERVICE_STATE_STOPPING) {
        (void)poll_runtime(runtime);
        if (runtime->state == MG_SERVICE_STATE_STOPPING)
            (void)process_yield();
    }
    return runtime->state == MG_SERVICE_STATE_RUNNING
        ? MG_OK : MG_ERR_SERVICE_UNAVAILABLE;
}

static void fill_status(const service_runtime_t *runtime,
                        mg_service_status_t *status)
{
    memset(status, 0, sizeof(*status));
    status->id = runtime->definition->id;
    status->state = runtime->state;
    status->wanted = runtime->wanted;
    if (runtime->definition->essential)
        status->flags |= MG_SERVICE_STATUS_ESSENTIAL;
    if (runtime->definition->supports_reload)
        status->flags |= MG_SERVICE_STATUS_RELOAD;
    status->pid = runtime->pid;
    status->restart_count = runtime->restart_count;
    strncpy(status->name, runtime->definition->name,
            sizeof(status->name) - 1U);
}

static mg_result_t status_response(const char *name,
                                   mg_service_control_response_t *response)
{
    const service_definition_t *definition;
    service_runtime_t *runtime;

    if (!name[0]) {
        for (usize index = 0;
             index < sizeof(service_definitions) / sizeof(service_definitions[0]);
             index++)
            fill_status(&service_runtime[index], &response->services[index]);
        response->count = sizeof(service_definitions) /
                          sizeof(service_definitions[0]);
        return MG_OK;
    }
    definition = definition_for_name(name);
    runtime = runtime_for_definition(definition);
    if (!definition || !runtime) return MG_ERR_NOT_FOUND;
    fill_status(runtime, &response->services[0]);
    response->count = 1;
    return MG_OK;
}

static mg_result_t handle_control(const mg_ipc_received_t *received,
                                  const mg_service_control_request_t *request,
                                  mg_service_control_response_t *response)
{
    const service_definition_t *definition;
    service_runtime_t *runtime;
    mg_result_t result;

    if (request->operation == MG_SERVICE_OP_STATUS)
        return status_response(request->service, response);
    definition = definition_for_name(request->service);
    runtime = runtime_for_definition(definition);
    if (!definition || !runtime) return MG_ERR_NOT_FOUND;
    if (definition->essential &&
        (request->operation == MG_SERVICE_OP_STOP ||
         request->operation == MG_SERVICE_OP_RESTART))
        return MG_ERR_ACCESS_DENIED;
    if (request->operation == MG_SERVICE_OP_RELOAD &&
        !definition->supports_reload)
        return MG_ERR_UNSUPPORTED;

    switch (request->operation) {
        case MG_SERVICE_OP_START:
            if (runtime->state == MG_SERVICE_STATE_RUNNING ||
                runtime->state == MG_SERVICE_STATE_STARTING)
                return MG_OK;
            result = authorize_operation(received, request->operation,
                                         definition);
            if (result != MG_OK) return result;
            runtime->failures = 0;
            runtime->restart_deadline = 0;
            return start_runtime(runtime, false);
        case MG_SERVICE_OP_STOP:
            if (runtime->state == MG_SERVICE_STATE_STOPPED) return MG_OK;
            result = authorize_operation(received, request->operation,
                                         definition);
            if (result != MG_OK) return result;
            return stop_runtime(runtime);
        case MG_SERVICE_OP_RESTART:
            result = authorize_operation(received, request->operation,
                                         definition);
            if (result != MG_OK) return result;
            runtime->failures = 0;
            runtime->restart_deadline = 0;
            return restart_runtime(runtime);
        case MG_SERVICE_OP_RELOAD:
            result = authorize_operation(received, request->operation,
                                         definition);
            if (result != MG_OK) return result;
            if (definition->id == MG_SERVICE_NETWORKD)
                return request_network_reload();
            return MG_ERR_UNSUPPORTED;
        default:
            return MG_ERR_BAD_ARGUMENT;
    }
}

static bool request_name_valid(const char *name)
{
    usize length = 0;

    while (length < MG_IPC_SERVICE_NAME_MAX && name[length]) {
        char value = name[length++];
        if ((length == 1U && (value < 'a' || value > 'z')) ||
            (length > 1U && !((value >= 'a' && value <= 'z') ||
                               (value >= '0' && value <= '9') ||
                               value == '_' || value == '-')))
            return false;
    }
    return length < MG_IPC_SERVICE_NAME_MAX;
}

static bool send_response(const mg_ipc_received_t *received,
                          const mg_service_control_response_t *response)
{
    mg_ipc_message_t message = {0};

    message.version = MG_IPC_PROTOCOL_VERSION;
    message.type = MG_SERVICE_CONTROL_RESPONSE;
    message.payload_length = sizeof(*response);
    memcpy(message.payload, response, sizeof(*response));
    return ipc_reply(received->request, &message) == MG_OK;
}

static bool handle_request(const mg_ipc_received_t *received)
{
    mg_service_control_request_t request;
    mg_service_control_response_t response = {0};

    if (received->message.type != MG_SERVICE_CONTROL_REQUEST ||
        received->message.payload_length != sizeof(request)) {
        response.result = MG_ERR_PROTOCOL;
        (void)send_response(received, &response);
        return true;
    }
    memcpy(&request, received->message.payload, sizeof(request));
    if (!request_name_valid(request.service) ||
        (request.operation != MG_SERVICE_OP_STATUS && !request.service[0])) {
        response.result = MG_ERR_BAD_ARGUMENT;
    } else {
        response.result = handle_control(received, &request, &response);
    }
    (void)send_response(received, &response);
    return true;
}

int main(void)
{
    mg_handle_t endpoint = 0;
    mg_ipc_received_t received;
    mg_result_t result;

    result = service_register("sprout", &endpoint);
    if (result_is_error(result)) {
        printf("Sprout: endpoint registration failed: %s\n",
               error_string(result));
        process_exit(1);
    }

    for (usize index = 0;
         index < sizeof(service_definitions) / sizeof(service_definitions[0]);
         index++) {
        service_runtime[index].definition = &service_definitions[index];
        service_runtime[index].state = MG_SERVICE_STATE_STOPPED;
        service_runtime[index].wanted = MG_SERVICE_WANTED_RUNNING;
        result = start_runtime(&service_runtime[index], true);
        if (result_is_error(result))
            printf("Sprout: %s start failed: %s\n",
                   service_definitions[index].name, error_string(result));
    }
    supervisor_bootstrap = false;

    for (;;) {
        bool progressed = false;
        mg_result_t receive_result;

        for (usize index = 0;
             index < sizeof(service_definitions) / sizeof(service_definitions[0]);
             index++)
            progressed |= service_tick(&service_runtime[index]);

        while ((receive_result = ipc_try_receive(endpoint, &received)) == MG_OK) {
            progressed |= handle_request(&received);
            (void)handle_close(received.request);
        }
        if (receive_result != MG_ERR_WOULD_BLOCK) {
            printf("Sprout: control endpoint unavailable: %s\n",
                   error_string(receive_result));
            (void)handle_close(endpoint);
            process_exit(1);
        }
        if (!progressed) (void)process_yield();
    }
}
