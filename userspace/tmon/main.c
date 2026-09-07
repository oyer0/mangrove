/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include <string.h>
#include "../common/help.h"
#include "../common/table.h"

#define TMON_HEADER_LINES       4U
#define TMON_RESERVED_LINES     2U
#define TMON_MAX_TASKS          64U
#define TMON_MIN_COLUMNS        46U
#define TMON_MAX_FRAME_ROWS     128U
#define TMON_MAX_FRAME_COLUMNS  256U
#define TMON_FRAME_LINE_CAPACITY (TMON_MAX_FRAME_COLUMNS * 4U + 1U)

static const mg_table_column_t TMON_PID_COLUMN = {
    MG_TABLE_ALIGN_RIGHT
};
static const mg_table_column_t TMON_NAME_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t TMON_USER_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t TMON_STATE_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t TMON_ROLE_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t TMON_SESSION_COLUMN = {
    MG_TABLE_ALIGN_RIGHT
};
static const mg_table_column_t TMON_SERVICE_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};

static mg_process_info_t task_snapshot[TMON_MAX_TASKS];
static mg_process_info_t task_page[MG_PROCESS_SNAPSHOT_PAGE_MAX];
static mg_table_row_t task_rows[TMON_MAX_TASKS + 1U];

typedef struct {
    char lines[TMON_MAX_FRAME_ROWS][TMON_FRAME_LINE_CAPACITY];
    usize lengths[TMON_MAX_FRAME_ROWS];
    usize display_widths[TMON_MAX_FRAME_ROWS];
    u32 row_count;
} tmon_frame_t;

static tmon_frame_t frame_a;
static tmon_frame_t frame_b;
static tmon_frame_t *current_frame = &frame_a;
static tmon_frame_t *previous_frame = &frame_b;

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

static bool append_task(mg_table_t *table, const mg_process_info_t *process)
{
    mg_table_row_t *row;
    char user[24];
    char session[32];
    const char *service;

    if (!table || !process) return false;
    row = table_row_begin(table);
    if (!row) return false;

    table_row_u64_column(row, &TMON_PID_COLUMN, process->pid);
    if (process->username[0]) {
        table_row_column(row, &TMON_NAME_COLUMN, process->name);
        table_row_column(row, &TMON_USER_COLUMN, process->username);
    } else {
        (void)snprintf(user, sizeof(user), "uid:%u", process->uid);
        table_row_column(row, &TMON_NAME_COLUMN, process->name);
        table_row_column(row, &TMON_USER_COLUMN, user);
    }
    table_row_column(row, &TMON_STATE_COLUMN, state_name(process->state));
    table_row_column(row, &TMON_ROLE_COLUMN, role_name(process->role));
    if (process->session_id) {
        (void)snprintf(session, sizeof(session), "%llu",
                       process->session_id);
    } else {
        strcpy(session, "-");
    }
    table_row_column(row, &TMON_SESSION_COLUMN, session);
    service = process->flags & MG_PROCESS_FLAG_SYSTEM_SERVICE
        ? (process->service[0] ? process->service : "-") : "-";
    table_row_column(row, &TMON_SERVICE_COLUMN, service);
    return true;
}

static bool append_task_header(mg_table_t *table)
{
    mg_table_row_t *row = table_row_begin(table);

    if (!row) return false;
    table_row_column(row, &TMON_PID_COLUMN, "PID");
    table_row_column(row, &TMON_NAME_COLUMN, "NAME");
    table_row_column(row, &TMON_USER_COLUMN, "USER");
    table_row_column(row, &TMON_STATE_COLUMN, "STATE");
    table_row_column(row, &TMON_ROLE_COLUMN, "ROLE");
    table_row_column(row, &TMON_SESSION_COLUMN, "SESSION");
    table_row_column(row, &TMON_SERVICE_COLUMN, "SERVICE");
    return true;
}

static bool collect_tasks(u32 capacity, u32 *out_count, u32 *out_total)
{
    u32 offset = 0;
    u32 stored = 0;
    u32 total = 0;

    if (!out_count || !out_total || capacity > TMON_MAX_TASKS)
        return false;

    for (;;) {
        mg_process_snapshot_request_t request = {0};
        u32 count = 0;
        mg_result_t result;

        request.offset = offset;
        request.result_capacity = MG_PROCESS_SNAPSHOT_PAGE_MAX;
        request.result = task_page;
        request.out_count = &count;
        request.out_total = &total;
        result = process_snapshot(&request);
        if (result != MG_OK) return false;

        for (u32 index = 0; index < count && stored < capacity; index++)
            task_snapshot[stored++] = task_page[index];

        if (count == 0 || stored == capacity || offset >= total ||
            count > total - offset) break;
        offset += count;
    }

    *out_count = stored;
    *out_total = total;
    return true;
}

static void frame_prepare(tmon_frame_t *frame, u32 rows)
{
    if (!frame) return;
    frame->row_count = rows > TMON_MAX_FRAME_ROWS ? TMON_MAX_FRAME_ROWS : rows;
    memset(frame->lengths, 0, sizeof(frame->lengths));
    memset(frame->display_widths, 0, sizeof(frame->display_widths));
    for (u32 row = 0; row < TMON_MAX_FRAME_ROWS; row++)
        frame->lines[row][0] = '\0';
}

static bool frame_set_text(tmon_frame_t *frame, u32 row, const char *text)
{
    usize length;
    usize display_width;

    if (!frame || row >= frame->row_count || !text) return false;
    table_text_metrics(text, &length, &display_width);
    if (length >= TMON_FRAME_LINE_CAPACITY) return false;
    memcpy(frame->lines[row], text, length);
    frame->lines[row][length] = '\0';
    frame->lengths[row] = length;
    frame->display_widths[row] = display_width;
    return true;
}

static bool frame_set_table_row(tmon_frame_t *frame, u32 row,
                                const mg_table_t *table, usize table_row,
                                const usize widths[MG_TABLE_MAX_COLUMNS],
                                usize column_count, usize table_width)
{
    usize length;

    if (!frame || row >= frame->row_count ||
        !table_format_row(table, table_row, widths, column_count,
                          frame->lines[row], TMON_FRAME_LINE_CAPACITY,
                          &length)) return false;
    frame->lengths[row] = length;
    frame->display_widths[row] = table_width;
    return true;
}

static bool build_frame(tmon_frame_t *frame, u32 rows, u32 columns)
{
    mg_system_memory_info_t memory;
    mg_monotonic_time_t monotonic;
    mg_table_t table;
    usize widths[MG_TABLE_MAX_COLUMNS] = {0};
    usize column_count = 0;
    usize table_width = 0;
    u32 task_count = 0;
    u32 task_total = 0;
    u32 task_capacity;
    bool have_tasks;
    bool have_memory;

    if (!frame || !rows || rows > TMON_MAX_FRAME_ROWS) return false;
    frame_prepare(frame, rows);

    if (rows < TMON_HEADER_LINES + TMON_RESERVED_LINES ||
        columns < TMON_MIN_COLUMNS) {
        return frame_set_text(frame, 0, "Terminal too small for tmon") &&
               frame_set_text(frame, rows - 1U, "q quit");
    }

    if (!frame_set_text(frame, 0, "Mangrove Task Monitor")) return false;
    if (mg_clock_monotonic(&monotonic) == MG_OK) {
        char line[TMON_FRAME_LINE_CAPACITY];
        u64 seconds = monotonic.milliseconds / 1000ULL;

        if (seconds < 60ULL)
            (void)snprintf(line, sizeof(line), "Uptime: %llus", seconds);
        else if (seconds < 3600ULL)
            (void)snprintf(line, sizeof(line), "Uptime: %llum %llus",
                           seconds / 60ULL, seconds % 60ULL);
        else if (seconds < 86400ULL)
            (void)snprintf(line, sizeof(line), "Uptime: %lluh %llum",
                           seconds / 3600ULL,
                           (seconds / 60ULL) % 60ULL);
        else
            (void)snprintf(line, sizeof(line), "Uptime: %llud %lluh",
                           seconds / 86400ULL,
                           (seconds / 3600ULL) % 24ULL);
        if (!frame_set_text(frame, 1, line)) return false;
    } else if (!frame_set_text(frame, 1, "Uptime: unavailable")) {
        return false;
    }

    have_memory = memory_info(&memory) == MG_OK;
    if (have_memory) {
        char line[TMON_FRAME_LINE_CAPACITY];

        (void)snprintf(line, sizeof(line), "Memory: %llu / %llu MiB",
                       memory.physical_used_bytes / (1024ULL * 1024ULL),
                       memory.physical_total_bytes / (1024ULL * 1024ULL));
        if (!frame_set_text(frame, 2, line)) return false;
    } else if (!frame_set_text(frame, 2, "Memory: unavailable")) {
        return false;
    }

    task_capacity = rows - TMON_HEADER_LINES - TMON_RESERVED_LINES;
    if (task_capacity > TMON_MAX_TASKS) task_capacity = TMON_MAX_TASKS;
    have_tasks = collect_tasks(task_capacity, &task_count, &task_total);
    {
        char line[TMON_FRAME_LINE_CAPACITY];

        (void)snprintf(line, sizeof(line), "Tasks: %u",
                       have_tasks ? task_total : 0U);
        if (!frame_set_text(frame, 3, line)) return false;
    }

    if (!have_tasks) {
        if (!frame_set_text(frame, TMON_HEADER_LINES,
                            "Task snapshot unavailable.")) return false;
    } else if (!task_count) {
        if (!frame_set_text(frame, TMON_HEADER_LINES, "No tasks.")) return false;
    } else {
        table_init(&table, task_rows, task_count + 1U);
        if (!append_task_header(&table)) {
            if (!frame_set_text(frame, TMON_HEADER_LINES,
                                "Task table unavailable.")) return false;
        } else {
            for (u32 index = 0; index < task_count; index++) {
                if (!append_task(&table, &task_snapshot[index])) {
                    if (!frame_set_text(frame, TMON_HEADER_LINES,
                                        "Task table unavailable.")) return false;
                    task_count = 0;
                    break;
                }
            }
            if (task_count &&
                table_calculate_widths(&table, widths, &column_count)) {
                table_width = table_display_width_from_widths(widths,
                                                               column_count);
                if (table_width <= columns) {
                    for (usize index = 0; index < table.row_count; index++) {
                        if (!frame_set_table_row(frame,
                                                 TMON_HEADER_LINES + (u32)index,
                                                 &table, index, widths,
                                                 column_count, table_width))
                            return false;
                    }
                } else if (!frame_set_text(frame, TMON_HEADER_LINES,
                                            "Terminal too small for task table.")) {
                    return false;
                }
            } else if (task_count) {
                if (!frame_set_text(frame, TMON_HEADER_LINES,
                                    "Task table unavailable.")) return false;
            }
        }
    }

    return frame_set_text(frame, rows - 1U, "q quit");
}

static bool write_spaces(usize count)
{
    static const char spaces[] = "                                ";

    while (count) {
        usize chunk = count < sizeof(spaces) - 1U ? count :
                      sizeof(spaces) - 1U;
        if (console_write(spaces, chunk) != (mg_result_t)chunk) return false;
        count -= chunk;
    }
    return true;
}

static bool frame_row_changed(const tmon_frame_t *current,
                             const tmon_frame_t *previous, u32 row)
{
    usize current_length = row < current->row_count ? current->lengths[row] : 0;
    usize previous_length = row < previous->row_count
        ? previous->lengths[row] : 0;

    if (current_length != previous_length) return true;
    if ((row < current->row_count ? current->display_widths[row] : 0) !=
        (row < previous->row_count ? previous->display_widths[row] : 0))
        return true;
    if (!current_length) return false;
    return memcmp(current->lines[row], previous->lines[row], current_length) != 0;
}

static bool render_frame(void)
{
    tmon_frame_t *current = current_frame;
    tmon_frame_t *previous = previous_frame;
    u32 rows = current->row_count > previous->row_count
        ? current->row_count : previous->row_count;

    if (terminal_update_begin() != MG_OK) return false;
    for (u32 row = 0; row < rows; row++) {
        usize current_length = row < current->row_count
            ? current->lengths[row] : 0;
        usize current_width = row < current->row_count
            ? current->display_widths[row] : 0;
        usize previous_width = row < previous->row_count
            ? previous->display_widths[row] : 0;

        if (!frame_row_changed(current, previous, row)) continue;
        if (terminal_cursor_move(row, 0) != MG_OK ||
            (current_length &&
             console_write(current->lines[row], current_length) !=
                 (mg_result_t)current_length) ||
            (previous_width > current_width &&
             !write_spaces(previous_width - current_width))) {
            (void)terminal_update_end();
            return false;
        }
    }
    if (terminal_update_end() != MG_OK) return false;
    current_frame = previous;
    previous_frame = current;
    return true;
}

int main(int argc, char **argv)
{
    mg_terminal_size_t size;
    mg_result_t result;
    bool entered = false;
    bool raw = false;
    u32 key;
    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 1) {
        command_usage_error(argv[0], "tmon", argc > 1 ? argv[1] : NULL);
        return 1;
    }

    result = terminal_get_size(&size);
    if (result != MG_OK || !size.rows || !size.columns) {
        printf("tmon: terminal size unavailable.\n");
        return 1;
    }
    result = terminal_alternate_enter();
    if (result != MG_OK) {
        printf("tmon: alternate screen unavailable.\n");
        return 1;
    }
    entered = true;
    result = terminal_input_raw(true);
    if (result != MG_OK) {
        (void)terminal_alternate_leave();
        printf("tmon: raw input unavailable.\n");
        return 1;
    }
    raw = true;
    (void)terminal_cursor_set_visible(false);

    for (;;) {
        result = terminal_get_size(&size);
        if (result != MG_OK || !size.rows || !size.columns) break;
        if (!build_frame(current_frame, size.rows, size.columns)) {
            result = MG_ERR_IO;
            break;
        }
        if (!render_frame()) {
            result = MG_ERR_IO;
            break;
        }
        result = terminal_read_key(1000U, &key);
        if (result == MG_ERR_TIMEOUT || result == MG_ERR_WOULD_BLOCK) continue;
        if (result != MG_OK) break;
        if (key == (u32)'q' || key == 0x1BU) break;
    }

    if (raw) (void)terminal_input_raw(false);
    if (entered && terminal_alternate_leave() != MG_OK) return 1;
    return result == MG_OK || result == MG_ERR_TIMEOUT ||
           result == MG_ERR_WOULD_BLOCK ? 0 : 1;
}
