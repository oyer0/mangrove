/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <mg/process.h>
#include <stdio.h>
#include <string.h>
#include "../common/help.h"
#include "../common/process_format.h"
#include "../common/table.h"

static const mg_table_column_t CREW_PID_COLUMN = {
    MG_TABLE_ALIGN_RIGHT
};
static const mg_table_column_t CREW_TIME_COLUMN = {
    MG_TABLE_ALIGN_RIGHT
};
static const mg_table_column_t CREW_COMMAND_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};

static mg_process_info_t process_records[MG_TABLE_MAX_ROWS];
static mg_process_info_t process_page[MG_PROCESS_SNAPSHOT_PAGE_MAX];
static mg_table_row_t process_rows[MG_TABLE_MAX_ROWS];

static const mg_process_info_t *find_process(const mg_process_info_t *processes,
                                             u32 count, u64 pid)
{
    if (!processes || !pid) return NULL;
    for (u32 index = 0; index < count; index++)
        if (processes[index].pid == pid) return &processes[index];
    return NULL;
}

static u64 shell_root_pid(const mg_process_info_t *processes, u32 count,
                          u64 current_pid)
{
    u64 pid = current_pid;

    for (u32 depth = 0; depth < count; depth++) {
        const mg_process_info_t *process = find_process(processes, count, pid);

        if (!process || (process->flags & MG_PROCESS_FLAG_SESSION_SHELL) ||
            !process->parent_pid || process->parent_pid == process->pid)
            return pid;
        pid = process->parent_pid;
    }
    return current_pid;
}

static bool process_in_family(const mg_process_info_t *processes, u32 count,
                              const mg_process_info_t *candidate,
                              u64 root_pid)
{
    u64 pid;

    if (!processes || !candidate || !root_pid) return false;
    pid = candidate->pid;
    for (u32 depth = 0; depth < count; depth++) {
        const mg_process_info_t *process;

        if (pid == root_pid) return true;
        process = find_process(processes, count, pid);
        if (!process || !process->parent_pid ||
            process->parent_pid == process->pid) return false;
        pid = process->parent_pid;
    }
    return false;
}

static bool collect_processes(u32 *out_count, u32 *out_total)
{
    u32 offset = 0;
    u32 stored = 0;
    u32 total = 0;

    if (!out_count || !out_total) return false;
    for (;;) {
        mg_process_snapshot_request_t request = {0};
        u32 count = 0;
        mg_result_t result;

        request.offset = offset;
        request.result_capacity = MG_PROCESS_SNAPSHOT_PAGE_MAX;
        request.result = process_page;
        request.out_count = &count;
        request.out_total = &total;
        result = process_snapshot(&request);
        if (result != MG_OK) return false;
        for (u32 index = 0; index < count && stored < MG_TABLE_MAX_ROWS;
             index++)
            process_records[stored++] = process_page[index];
        if (count == 0 || offset >= total || count > total - offset)
            break;
        offset += count;
    }
    *out_count = stored;
    *out_total = total;
    return true;
}

static const char *short_command_name(const mg_process_info_t *process)
{
    const char *name;
    const char *cursor;

    if (!process) return "?";
    if (process->name[0]) return process->name;
    name = process->executable_path;
    for (cursor = process->executable_path; *cursor; cursor++)
        if (*cursor == '/' && cursor[1]) name = cursor + 1;
    return name[0] ? name : "?";
}

static bool append_process(mg_table_t *table,
                           const mg_process_info_t *process)
{
    mg_table_row_t *row;
    char time[32];

    if (!table || !process || !process_format_cpu_time(process->cpu_time_ms,
                                                        time, sizeof(time)))
        return false;
    row = table_row_begin(table);
    if (!row) return false;
    table_row_u64_column(row, &CREW_PID_COLUMN, process->pid);
    table_row_column(row, &CREW_TIME_COLUMN, time);
    table_row_column(row, &CREW_COMMAND_COLUMN, short_command_name(process));
    return true;
}

static bool append_header(mg_table_t *table)
{
    mg_table_row_t *row = table_row_begin(table);

    if (!row) return false;
    table_row_column(row, &CREW_PID_COLUMN, "PID");
    table_row_column(row, &CREW_TIME_COLUMN, "TIME");
    table_row_column(row, &CREW_COMMAND_COLUMN, "CMD");
    return true;
}

static int list_processes(void)
{
    u64 current_pid;
    u64 root_pid;
    u32 process_count;
    u32 process_total;
    mg_table_t table;
    bool printed_header = false;

    current_pid = process_current_pid();
    if (!current_pid || !collect_processes(&process_count, &process_total)) {
        printf("crew: process context unavailable.\n");
        return 1;
    }
    (void)process_total;
    root_pid = shell_root_pid(process_records, process_count, current_pid);
    table_init(&table, process_rows, MG_TABLE_MAX_ROWS);

    for (u32 index = 0; index < process_count; index++) {
        if (!process_in_family(process_records, process_count,
                               &process_records[index], root_pid))
            continue;
        if (!printed_header) {
            if (!append_header(&table)) {
                printf("crew: process table is too large.\n");
                return 1;
            }
            printed_header = true;
        }
        if (!append_process(&table, &process_records[index])) {
            printf("crew: process table is too large.\n");
            return 1;
        }
    }
    if (!printed_header) printf("No processes.\n");
    else if (!table_render(&table)) {
        printf("crew: invalid process table.\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 1) {
        command_usage_error(argv[0], "crew", argc > 1 ? argv[1] : NULL);
        return 1;
    }
    return list_processes();
}
