/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include <string.h>
#include "../common/help.h"
#include "../common/table.h"

#define TMON_MAX_CPUS           256U
#define TMON_MAX_TASKS          64U
#define TMON_MIN_COLUMNS        30U
#define TMON_MAX_FRAME_ROWS     128U
#define TMON_MAX_FRAME_COLUMNS  256U
#define TMON_FRAME_LINE_CAPACITY (TMON_MAX_FRAME_COLUMNS * 4U + 1U)

static const mg_table_column_t TMON_PID_COLUMN = {
    MG_TABLE_ALIGN_RIGHT
};
static const mg_table_column_t TMON_NAME_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t TMON_CPU_COLUMN = {
    MG_TABLE_ALIGN_RIGHT
};
static const mg_table_column_t TMON_STATE_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};

static mg_process_info_t task_snapshot[TMON_MAX_TASKS];
static mg_process_info_t task_page[MG_PROCESS_SNAPSHOT_PAGE_MAX];
static mg_table_row_t task_rows[TMON_MAX_TASKS + 1U];

typedef struct {
    u64 total_ticks;
    u64 busy_ticks;
    bool valid;
} tmon_cpu_sample_t;

typedef struct {
    bool present;
    bool online;
    u32 percent;
} tmon_cpu_usage_t;

static mg_cpu_info_t cpu_page[MG_CPU_SNAPSHOT_PAGE_MAX];
static tmon_cpu_sample_t cpu_previous[TMON_MAX_CPUS];
static tmon_cpu_usage_t cpu_usage[TMON_MAX_CPUS];
static bool cpu_sample_valid;

typedef struct {
    char lines[TMON_MAX_FRAME_ROWS][TMON_FRAME_LINE_CAPACITY];
    usize lengths[TMON_MAX_FRAME_ROWS];
    usize display_widths[TMON_MAX_FRAME_ROWS];
    mg_terminal_style_role_t styles[TMON_MAX_FRAME_ROWS];
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

static bool append_task(mg_table_t *table, const mg_process_info_t *process)
{
    mg_table_row_t *row;
    char cpu[16];

    if (!table || !process) return false;
    row = table_row_begin(table);
    if (!row) return false;

    table_row_u64_column(row, &TMON_PID_COLUMN, process->pid);
    table_row_column(row, &TMON_NAME_COLUMN, process->name);
    if (process->running_cpu == MG_PROCESS_CPU_NONE)
        strcpy(cpu, "-");
    else
        (void)snprintf(cpu, sizeof(cpu), "%u", process->running_cpu);
    table_row_column(row, &TMON_CPU_COLUMN, cpu);
    table_row_column(row, &TMON_STATE_COLUMN, state_name(process->state));
    return true;
}

static bool append_task_header(mg_table_t *table)
{
    mg_table_row_t *row = table_row_begin(table);

    if (!row) return false;
    table_row_column(row, &TMON_PID_COLUMN, "PID");
    table_row_column(row, &TMON_NAME_COLUMN, "NAME");
    table_row_column(row, &TMON_CPU_COLUMN, "CPU");
    table_row_column(row, &TMON_STATE_COLUMN, "STATE");
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

static bool collect_cpu_usage(u32 *out_count, u32 *out_total_percent)
{
    tmon_cpu_sample_t previous;
    u32 offset = 0;
    u32 total = 0;
    u64 total_delta = 0;
    u64 busy_delta = 0;

    if (!out_count || !out_total_percent) return false;
    memset(cpu_usage, 0, sizeof(cpu_usage));

    for (;;) {
        mg_cpu_snapshot_request_t request = {0};
        u32 count = 0;
        mg_result_t result;

        request.offset = offset;
        request.result_capacity = MG_CPU_SNAPSHOT_PAGE_MAX;
        request.result = cpu_page;
        request.out_count = &count;
        request.out_total = &total;
        result = cpu_snapshot(&request);
        if (result != MG_OK) return false;

        for (u32 page_index = 0; page_index < count; page_index++) {
            const mg_cpu_info_t *info = &cpu_page[page_index];
            tmon_cpu_usage_t *usage;
            u64 current_total;
            u64 current_busy;
            u64 delta_total = 0;
            u64 delta_busy = 0;

            if (info->index >= TMON_MAX_CPUS) continue;
            usage = &cpu_usage[info->index];
            usage->present = true;
            usage->online = (info->flags & MG_CPU_FLAG_ONLINE) != 0;
            current_total = info->total_ticks;
            current_busy = info->busy_ticks;
            previous = cpu_previous[info->index];
            if (cpu_sample_valid && previous.valid) {
                delta_total = current_total - previous.total_ticks;
                delta_busy = current_busy - previous.busy_ticks;
                if (delta_busy > delta_total) delta_busy = delta_total;
            }
            if (delta_total)
                usage->percent = (u32)((delta_busy * 100ULL) / delta_total);
            cpu_previous[info->index].total_ticks = current_total;
            cpu_previous[info->index].busy_ticks = current_busy;
            cpu_previous[info->index].valid = true;
            if (usage->online) {
                total_delta += delta_total;
                busy_delta += delta_busy;
            }
        }

        if (count == 0 || offset >= total || count > total - offset)
            break;
        offset += count;
    }
    cpu_sample_valid = true;
    *out_count = total > TMON_MAX_CPUS ? TMON_MAX_CPUS : total;
    *out_total_percent = total_delta
        ? (u32)((busy_delta * 100ULL) / total_delta) : 0U;
    if (*out_total_percent > 100U) *out_total_percent = 100U;
    return true;
}

static bool line_append_text(char *line, usize capacity, usize *length,
                             const char *text)
{
    usize text_length;

    if (!line || !length || !text || *length >= capacity) return false;
    text_length = strlen(text);
    if (text_length >= capacity - *length) return false;
    memcpy(line + *length, text, text_length);
    *length += text_length;
    line[*length] = '\0';
    return true;
}

static bool line_append_char(char *line, usize capacity, usize *length,
                             char character)
{
    if (!line || !length || *length + 1U >= capacity) return false;
    line[(*length)++] = character;
    line[*length] = '\0';
    return true;
}

static u32 bar_width_for(u32 columns, usize label_width, usize suffix_width)
{
    const u32 maximum = 128U;
    u32 fixed = (u32)label_width + 1U + 2U + (u32)suffix_width;
    u32 width;

    if (columns <= fixed) return 1U;
    width = columns - fixed;
    return width > maximum ? maximum : width;
}

static bool build_bar_line(char *line, usize capacity, const char *label,
                           u32 percent, u32 bar_width, const char *suffix)
{
    static const char filled[] = "\xE2\x96\x88";
    static const char empty[] = "\xE2\x96\x91";
    usize length = 0;
    u32 filled_count;

    if (!line || !label || !suffix || percent > 100U) return false;
    line[0] = '\0';
    if (!line_append_text(line, capacity, &length, label) ||
        !line_append_char(line, capacity, &length, ' ')) return false;
    filled_count = (bar_width * percent) / 100U;
    for (u32 index = 0; index < bar_width; index++) {
        if (!line_append_text(line, capacity, &length,
                              index < filled_count ? filled : empty))
            return false;
    }
    return line_append_text(line, capacity, &length, "  ") &&
           line_append_text(line, capacity, &length, suffix);
}

static void frame_prepare(tmon_frame_t *frame, u32 rows)
{
    if (!frame) return;
    frame->row_count = rows > TMON_MAX_FRAME_ROWS ? TMON_MAX_FRAME_ROWS : rows;
    memset(frame->lengths, 0, sizeof(frame->lengths));
    memset(frame->display_widths, 0, sizeof(frame->display_widths));
    for (u32 row = 0; row < TMON_MAX_FRAME_ROWS; row++) {
        frame->lines[row][0] = '\0';
        frame->styles[row] = MG_TERMINAL_STYLE_DEFAULT;
    }
}

static bool frame_set_text_style(tmon_frame_t *frame, u32 row,
                                 const char *text,
                                 mg_terminal_style_role_t style)
{
    usize length;
    usize display_width;

    if (!frame || row >= frame->row_count || !text ||
        style >= MG_TERMINAL_STYLE_COUNT) return false;
    table_text_metrics_capacity(text, TMON_FRAME_LINE_CAPACITY,
                                &length, &display_width);
    if (length >= TMON_FRAME_LINE_CAPACITY) return false;
    memcpy(frame->lines[row], text, length);
    frame->lines[row][length] = '\0';
    frame->lengths[row] = length;
    frame->display_widths[row] = display_width;
    frame->styles[row] = style;
    return true;
}

static bool frame_set_text(tmon_frame_t *frame, u32 row, const char *text)
{
    return frame_set_text_style(frame, row, text,
                                MG_TERMINAL_STYLE_DEFAULT);
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
    frame->styles[row] = MG_TERMINAL_STYLE_DEFAULT;
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
    u32 cpu_count = 0;
    u32 total_cpu_percent = 0;
    u32 task_count = 0;
    u32 task_total = 0;
    u32 task_capacity = 0;
    u32 row = 0;
    u32 footer;
    bool have_tasks;
    bool have_memory;
    bool have_cpus;

    if (!frame || !rows || rows > TMON_MAX_FRAME_ROWS) return false;
    frame_prepare(frame, rows);

    if (rows < 8U ||
        columns < TMON_MIN_COLUMNS) {
        return frame_set_text(frame, 0, "Terminal too small for tmon") &&
               frame_set_text(frame, rows - 1U, "q quit");
    }
    footer = rows - 1U;

    if (!frame_set_text_style(frame, row++, "Mangrove Task Monitor",
                              MG_TERMINAL_STYLE_HEADING)) return false;
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
        if (!frame_set_text(frame, row++, line)) return false;
    } else if (!frame_set_text(frame, row++, "Uptime: unavailable")) {
        return false;
    }

    if (!frame_set_text_style(frame, row++, "CPU",
                              MG_TERMINAL_STYLE_HEADING)) return false;
    have_cpus = collect_cpu_usage(&cpu_count, &total_cpu_percent);
    if (!have_cpus) {
        if (row < footer && !frame_set_text(frame, row++,
                                            "CPU: unavailable")) return false;
    } else {
        for (u32 index = 0; index < cpu_count && row < footer; index++) {
            const tmon_cpu_usage_t *usage = &cpu_usage[index];
            char label[16];
            char suffix[16];
            char line[TMON_FRAME_LINE_CAPACITY];

            if (!usage->present) continue;
            (void)snprintf(label, sizeof(label), "%u", index);
            if (!usage->online) {
                (void)snprintf(line, sizeof(line), "%s offline", label);
            } else {
                (void)snprintf(suffix, sizeof(suffix), "%u%%",
                               usage->percent);
                if (!build_bar_line(line, sizeof(line), label,
                                    usage->percent,
                                    bar_width_for(columns, strlen(label),
                                                  strlen(suffix)), suffix))
                    return false;
            }
            if (!frame_set_text(frame, row++, line)) return false;
        }
        if (row < footer) {
            char line[TMON_FRAME_LINE_CAPACITY];
            char suffix[16];

            (void)snprintf(suffix, sizeof(suffix), "%u%%",
                           total_cpu_percent);
            if (!build_bar_line(line, sizeof(line), "Total",
                                total_cpu_percent,
                                bar_width_for(columns, 5U, strlen(suffix)),
                                suffix) || !frame_set_text(frame, row++, line))
                return false;
        }
    }

    if (row < footer) row++;
    if (row < footer && !frame_set_text_style(frame, row++, "Memory",
                                              MG_TERMINAL_STYLE_HEADING))
        return false;
    have_memory = memory_info(&memory) == MG_OK;
    if (row < footer) {
        if (have_memory) {
            char line[TMON_FRAME_LINE_CAPACITY];
            char suffix[64];
            u64 total_bytes = memory.physical_total_bytes;
            u64 used_bytes = memory.physical_used_bytes > total_bytes
                ? total_bytes : memory.physical_used_bytes;
            u32 percent = total_bytes
                ? (u32)((used_bytes * 100ULL) / total_bytes) : 0U;

            (void)snprintf(suffix, sizeof(suffix), "%llu MiB / %llu MiB",
                           used_bytes / (1024ULL * 1024ULL),
                           total_bytes / (1024ULL * 1024ULL));
            if (!build_bar_line(line, sizeof(line), "Used", percent,
                                bar_width_for(columns, 4U, strlen(suffix)),
                                suffix) || !frame_set_text(frame, row++, line))
                return false;
        } else if (!frame_set_text(frame, row++, "Memory: unavailable")) {
            return false;
        }
    }

    if (row < footer) row++;
    if (row < footer && !frame_set_text_style(frame, row++, "Processes",
                                              MG_TERMINAL_STYLE_HEADING))
        return false;
    if (footer > row + 1U) task_capacity = footer - row - 1U;
    if (task_capacity > TMON_MAX_TASKS) task_capacity = TMON_MAX_TASKS;
    have_tasks = task_capacity &&
        collect_tasks(task_capacity, &task_count, &task_total);
    if (!row || row >= footer) {
        /* The footer is still useful on very short terminals. */
    } else if (!have_tasks) {
        if (!frame_set_text(frame, row, task_capacity
                            ? "Task snapshot unavailable."
                            : "No room for process table.")) return false;
    } else if (!task_count) {
        if (!frame_set_text(frame, row, "No tasks.")) return false;
    } else {
        table_init(&table, task_rows, task_count + 1U);
        if (!append_task_header(&table)) {
            if (!frame_set_text(frame, row,
                                "Task table unavailable.")) return false;
        } else {
            for (u32 index = 0; index < task_count; index++) {
                if (!append_task(&table, &task_snapshot[index])) {
                    if (!frame_set_text(frame, row,
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
                                                 row + (u32)index,
                                                 &table, index, widths,
                                                 column_count, table_width))
                            return false;
                    }
                    frame->styles[row] = MG_TERMINAL_STYLE_HEADING;
                } else if (!frame_set_text(frame, row,
                                            "Terminal too small for task table.")) {
                    return false;
                }
            } else if (task_count) {
                if (!frame_set_text(frame, row,
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
    if ((row < current->row_count ? current->styles[row]
                                   : MG_TERMINAL_STYLE_DEFAULT) !=
        (row < previous->row_count ? previous->styles[row]
                                   : MG_TERMINAL_STYLE_DEFAULT))
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
             terminal_write_semantic(current->lines[row], current_length,
                                     current->styles[row]) !=
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
        result = terminal_read_key(250U, &key);
        if (result == MG_ERR_TIMEOUT || result == MG_ERR_WOULD_BLOCK) continue;
        if (result != MG_OK) break;
        if (key == (u32)'q' || key == 0x1BU) break;
    }

    if (raw) (void)terminal_input_raw(false);
    if (entered && terminal_alternate_leave() != MG_OK) return 1;
    return result == MG_OK || result == MG_ERR_TIMEOUT ||
           result == MG_ERR_WOULD_BLOCK ? 0 : 1;
}
