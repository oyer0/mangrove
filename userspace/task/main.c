/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include <string.h>
#include "../common/help.h"
#include "../common/table.h"

static const mg_table_column_t TASK_PID_COLUMN = {
    MG_TABLE_ALIGN_RIGHT
};
static const mg_table_column_t TASK_NAME_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t TASK_USER_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t TASK_STATE_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t TASK_ROLE_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t TASK_SESSION_COLUMN = {
    MG_TABLE_ALIGN_RIGHT
};
static const mg_table_column_t TASK_SERVICE_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};

static mg_table_row_t task_rows[MG_TABLE_MAX_ROWS];

static const char *state_name(u32 state)
{
    switch (state) {
        case MG_PROCESS_INSPECTION_RUNNING: return "running";
        case MG_PROCESS_INSPECTION_READY: return "ready";
        case MG_PROCESS_INSPECTION_BLOCKED: return "blocked";
        case MG_PROCESS_INSPECTION_EXITED: return "exited";
        default: return "unknown";
    }
}

static const char *role_name(u32 role)
{
    if (role == MG_INSPECTION_ROLE_SYSTEM) return "system";
    if (role == MG_IDENTITY_ROLE_REGULAR) return "regular";
    if (role == MG_IDENTITY_ROLE_ADMIN) return "admin";
    return "unknown";
}

static bool append_process(mg_table_t *table,
                           const mg_process_info_t *process)
{
    mg_table_row_t *row = table_row_begin(table);
    char user[24];
    char session[32];
    const char *service;

    if (!row) return false;
    table_row_u64_column(row, &TASK_PID_COLUMN, process->pid);
    if (process->username[0]) {
        table_row_column(row, &TASK_NAME_COLUMN, process->name);
        table_row_column(row, &TASK_USER_COLUMN, process->username);
    } else {
        (void)snprintf(user, sizeof(user), "uid:%u", process->uid);
        table_row_column(row, &TASK_NAME_COLUMN, process->name);
        table_row_column(row, &TASK_USER_COLUMN, user);
    }
    table_row_column(row, &TASK_STATE_COLUMN, state_name(process->state));
    table_row_column(row, &TASK_ROLE_COLUMN, role_name(process->role));
    if (process->session_id) {
        (void)snprintf(session, sizeof(session), "%llu",
                       process->session_id);
    } else {
        strcpy(session, "-");
    }
    table_row_column(row, &TASK_SESSION_COLUMN, session);
    service = process->flags & MG_PROCESS_FLAG_SYSTEM_SERVICE
        ? (process->service[0] ? process->service : "-") : "-";
    table_row_column(row, &TASK_SERVICE_COLUMN, service);
    return true;
}

static bool append_header(mg_table_t *table)
{
    mg_table_row_t *row = table_row_begin(table);

    if (!row) return false;
    table_row_column(row, &TASK_PID_COLUMN, "PID");
    table_row_column(row, &TASK_NAME_COLUMN, "NAME");
    table_row_column(row, &TASK_USER_COLUMN, "USER");
    table_row_column(row, &TASK_STATE_COLUMN, "STATE");
    table_row_column(row, &TASK_ROLE_COLUMN, "ROLE");
    table_row_column(row, &TASK_SESSION_COLUMN, "SESSION");
    table_row_column(row, &TASK_SERVICE_COLUMN, "SERVICE");
    return true;
}

static int list_tasks(void)
{
    mg_process_info_t processes[MG_PROCESS_SNAPSHOT_PAGE_MAX];
    u32 offset = 0;
    u32 total = 0;
    bool printed_header = false;
    mg_table_t table;

    table_init(&table, task_rows, MG_TABLE_MAX_ROWS);

    for (;;) {
        mg_process_snapshot_request_t request = {0};
        u32 count = 0;
        mg_result_t result;

        request.offset = offset;
        request.result_capacity = MG_PROCESS_SNAPSHOT_PAGE_MAX;
        request.result = processes;
        request.out_count = &count;
        request.out_total = &total;
        result = process_snapshot(&request);
        if (result != MG_OK) {
            printf("task: %s\n", error_string(result));
            return 1;
        }
        if (count && !printed_header) {
            if (!append_header(&table)) {
                printf("task: table is too large.\n");
                return 1;
            }
            printed_header = true;
        }
        for (u32 index = 0; index < count; index++) {
            if (!append_process(&table, &processes[index])) {
                printf("task: table is too large.\n");
                return 1;
            }
        }
        if (count == 0 || offset >= total || count > total - offset) break;
        offset += count;
    }
    if (!printed_header) printf("No processes.\n");
    else if (!table_render(&table)) {
        printf("task: invalid table data.\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 1) {
        command_usage_error(argv[0], "task", argc > 1 ? argv[1] : NULL);
        return 1;
    }
    return list_tasks();
}
