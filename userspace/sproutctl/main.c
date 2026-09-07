#include <mg/ipc.h>
#include <mg/service.h>
#include <mangrove.h>
#include <stdio.h>
#include <string.h>
#include "../common/help.h"
#include "../common/table.h"

static const mg_table_column_t SERVICE_NAME_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t SERVICE_STATE_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};

static mg_table_row_t service_rows[MG_TABLE_MAX_ROWS];

static const char *state_name(u32 state)
{
    switch (state) {
        case MG_SERVICE_STATE_STOPPED: return "stopped";
        case MG_SERVICE_STATE_STARTING: return "starting";
        case MG_SERVICE_STATE_RUNNING: return "running";
        case MG_SERVICE_STATE_STOPPING: return "stopping";
        case MG_SERVICE_STATE_FAILED: return "failed";
        default: return "unknown";
    }
}

static const char *operation_name(const char *operation)
{
    if (!strcmp(operation, "status")) return NULL;
    return operation;
}

static u32 operation_value(const char *operation)
{
    if (!strcmp(operation, "status")) return MG_SERVICE_OP_STATUS;
    if (!strcmp(operation, "start")) return MG_SERVICE_OP_START;
    if (!strcmp(operation, "stop")) return MG_SERVICE_OP_STOP;
    if (!strcmp(operation, "restart")) return MG_SERVICE_OP_RESTART;
    if (!strcmp(operation, "reload")) return MG_SERVICE_OP_RELOAD;
    return 0;
}

static void print_status(const mg_service_status_t *status, bool detailed)
{
    if (!detailed) return;
    printf("Service: %s\n", status->name);
    printf("State: %s\n", state_name(status->state));
    printf("Wanted: %s\n", status->wanted == MG_SERVICE_WANTED_RUNNING
           ? "running" : "stopped");
    if (status->pid) printf("PID: %llu\n", status->pid);
    printf("Restart count: %u\n", status->restart_count);
    printf("Essential: %s\n",
           (status->flags & MG_SERVICE_STATUS_ESSENTIAL) ? "yes" : "no");
    printf("Reload: %s\n",
           (status->flags & MG_SERVICE_STATUS_RELOAD) ? "yes" : "no");
}

static bool append_status(mg_table_t *table,
                          const mg_service_status_t *status)
{
    mg_table_row_t *row = table_row_begin(table);

    if (!row) return false;
    table_row_column(row, &SERVICE_NAME_COLUMN, status->name);
    table_row_column(row, &SERVICE_STATE_COLUMN, state_name(status->state));
    return true;
}

static int print_failure(mg_result_t result, const char *operation)
{
    if (result == MG_ERR_PRIVILEGE_REQUIRED)
        printf("This action requires administrator privileges.\n");
    else if (result == MG_ERR_ACCESS_DENIED &&
             (!strcmp(operation, "stop") || !strcmp(operation, "restart")))
        printf("This service is essential and cannot be %sed.\n",
               !strcmp(operation, "stop") ? "stopp" : "restart");
    else if (result == MG_ERR_UNSUPPORTED && !strcmp(operation, "reload"))
        printf("This service does not support reload.\n");
    else
        printf("sprout: %s\n", error_string(result));
    return 1;
}

int main(int argc, char **argv)
{
    mg_handle_t endpoint;
    mg_ipc_message_t request = {0};
    mg_ipc_message_t reply = {0};
    mg_service_control_request_t control = {0};
    mg_service_control_response_t response;
    mg_result_t result;
    u32 operation;
    const char *service = "";
    bool detailed = false;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc < 2 || argc > 3) {
        command_usage_error(argv[0], "sprout <status|start|stop|restart|reload> [service]",
                            argc > 1 ? argv[1] : NULL);
        return 1;
    }
    operation = operation_value(argv[1]);
    if (!operation) {
        command_usage_error(argv[0],
                            "sprout <status|start|stop|restart|reload> [service]",
                            argv[1]);
        return 1;
    }
    if (argc == 3) {
        service = argv[2];
        detailed = true;
    } else if (operation != MG_SERVICE_OP_STATUS) {
        command_usage_error(argv[0], "sprout <operation> <service>", NULL);
        return 1;
    }
    if (operation_name(argv[1]) && !service[0]) {
        command_usage_error(argv[0], "sprout <operation> <service>", NULL);
        return 1;
    }
    strncpy(control.service, service, sizeof(control.service) - 1U);
    control.operation = (u16)operation;
    request.version = MG_IPC_PROTOCOL_VERSION;
    request.type = MG_SERVICE_CONTROL_REQUEST;
    request.payload_length = sizeof(control);
    memcpy(request.payload, &control, sizeof(control));

    result = service_lookup("sprout", &endpoint);
    if (result != MG_OK) {
        printf("sprout: %s\n", error_string(result));
        return 1;
    }
    result = ipc_request(endpoint, &request, &reply);
    (void)handle_close(endpoint);
    if (result != MG_OK) {
        printf("sprout: %s\n", error_string(result));
        return 1;
    }
    if (reply.version != MG_IPC_PROTOCOL_VERSION ||
        reply.type != MG_SERVICE_CONTROL_RESPONSE ||
        reply.payload_length != sizeof(response)) {
        printf("sprout: invalid service response.\n");
        return 1;
    }
    memcpy(&response, reply.payload, sizeof(response));
    if (response.result != MG_OK)
        return print_failure(response.result, argv[1]);
    if (operation == MG_SERVICE_OP_STATUS) {
        if (detailed) {
            for (u32 index = 0; index < response.count &&
                 index < MG_SERVICE_STATUS_MAX; index++)
                print_status(&response.services[index], true);
        } else {
            mg_table_t table;

            table_init(&table, service_rows, MG_TABLE_MAX_ROWS);
            for (u32 index = 0; index < response.count &&
                 index < MG_SERVICE_STATUS_MAX; index++)
                if (!append_status(&table, &response.services[index])) {
                    printf("sprout: table is too large.\n");
                    return 1;
                }
            if (!table_render(&table)) {
                printf("sprout: invalid table data.\n");
                return 1;
            }
        }
    } else {
        printf("%s %s.\n", argv[1], service);
    }
    return 0;
}
