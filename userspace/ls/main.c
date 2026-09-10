/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/help.h"
#include "../common/path.h"

#define LS_COLUMN_GAP 2U
#define LS_INITIAL_CAPACITY MG_DIRECTORY_BATCH_MAX

typedef struct {
    mg_directory_entry_t entry;
    usize display_width;
} ls_entry_t;

static usize entry_display_width(const mg_directory_entry_t *entry,
                                 bool styled_output)
{
    if (!entry) return 0;
    return strlen(entry->name) +
           (!styled_output && entry->type == MG_PATH_TYPE_DIRECTORY ? 1U : 0U);
}

static bool append_entries(ls_entry_t **entries, usize *count,
                           usize *capacity,
                           const mg_directory_entry_t *batch,
                           usize batch_count, bool styled_output)
{
    usize required;

    if (!entries || !count || !capacity || (!batch && batch_count))
        return false;
    if (batch_count > (usize)-1 - *count) return false;
    required = *count + batch_count;
    if (required > *capacity) {
        usize new_capacity = *capacity ? *capacity : LS_INITIAL_CAPACITY;

        while (new_capacity < required) {
            if (new_capacity > (usize)-1 / 2U) {
                new_capacity = required;
                break;
            }
            new_capacity *= 2U;
        }
        if (new_capacity > (usize)-1 / sizeof(**entries)) return false;
        ls_entry_t *replacement = (ls_entry_t *)realloc(
            *entries, new_capacity * sizeof(**entries));
        if (!replacement) return false;
        *entries = replacement;
        *capacity = new_capacity;
    }
    for (usize index = 0; index < batch_count; index++) {
        (*entries)[*count + index].entry = batch[index];
        (*entries)[*count + index].display_width =
            entry_display_width(&batch[index], styled_output);
    }
    *count = required;
    return true;
}

static usize layout_rows(usize count, usize columns)
{
    return count / columns + (count % columns != 0U);
}

static usize layout_used_columns(usize count, usize columns)
{
    usize rows = layout_rows(count, columns);

    return count / rows + (count % rows != 0U);
}

static usize layout_column_width(const ls_entry_t *entries, usize count,
                                 usize columns, usize column)
{
    usize rows = layout_rows(count, columns);
    usize width = 0;

    for (usize row = 0; row < rows; row++) {
        usize index = column * rows + row;
        if (index >= count) continue;
        if (entries[index].display_width > width)
            width = entries[index].display_width;
    }
    return width;
}

static bool layout_fits(const ls_entry_t *entries, usize count,
                        usize columns, usize terminal_width)
{
    usize total = 0;
    usize used_columns = layout_used_columns(count, columns);

    for (usize column = 0; column < used_columns; column++) {
        usize width = layout_column_width(entries, count, columns, column);

        if (column && total > (usize)-1 - LS_COLUMN_GAP) return false;
        if (column) total += LS_COLUMN_GAP;
        if (total > (usize)-1 - width) return false;
        total += width;
        if (total > terminal_width) return false;
    }
    return true;
}

static usize choose_columns(const ls_entry_t *entries, usize count,
                            usize terminal_width)
{
    usize maximum;
    usize columns;

    if (!entries || !count || !terminal_width) return 1U;
    maximum = count;
    if (terminal_width <= (usize)-1 - LS_COLUMN_GAP) {
        usize width_limited =
            (terminal_width + LS_COLUMN_GAP) / (1U + LS_COLUMN_GAP);
        if (width_limited < maximum) maximum = width_limited;
    }
    if (!maximum) maximum = 1U;
    columns = maximum;
    for (;;) {
        if (layout_fits(entries, count, columns, terminal_width))
            return layout_used_columns(count, columns);
        if (columns == 1U) return 1U;
        columns--;
    }
}

static bool write_spaces(usize count)
{
    static const char spaces[] =
        "                                ";

    while (count) {
        usize chunk = count < sizeof(spaces) - 1U ? count :
                      sizeof(spaces) - 1U;
        if (console_write(spaces, chunk) != (mg_result_t)chunk) return false;
        count -= chunk;
    }
    return true;
}

static bool write_entry(const ls_entry_t *entry, bool styled_output)
{
    char display[sizeof(entry->entry.name) + 2U];
    usize length;
    mg_result_t result;

    if (!entry) return false;
    length = strlen(entry->entry.name);
    memcpy(display, entry->entry.name, length);
    if (!styled_output && entry->entry.type == MG_PATH_TYPE_DIRECTORY)
        display[length++] = '/';
    result = styled_output
        ? terminal_write_semantic(
              display, length,
              entry->entry.type == MG_PATH_TYPE_DIRECTORY
                  ? MG_TERMINAL_STYLE_DIRECTORY : MG_TERMINAL_STYLE_DEFAULT)
        : console_write(display, length);
    return result == (mg_result_t)length;
}

static bool render_one_per_line(const ls_entry_t *entries, usize count,
                                bool styled_output)
{
    if (!count) return true;
    if (console_begin_transaction() != MG_OK) return false;
    for (usize index = 0; index < count; index++) {
        if (!write_entry(&entries[index], styled_output) ||
            console_write("\n", 1U) != 1) {
            (void)console_end_transaction();
            return false;
        }
    }
    return console_end_transaction() == MG_OK;
}

static bool render_columns(const ls_entry_t *entries, usize count,
                           usize terminal_width, bool styled_output)
{
    usize columns;
    usize rows;
    usize *widths;
    bool success = true;

    if (!count) return true;
    columns = choose_columns(entries, count, terminal_width);
    rows = layout_rows(count, columns);
    if (columns > (usize)-1 / sizeof(*widths)) return false;
    widths = (usize *)calloc(columns, sizeof(*widths));
    if (!widths) return false;
    for (usize column = 0; column < columns; column++)
        widths[column] = layout_column_width(entries, count, columns, column);

    if (console_begin_transaction() != MG_OK) {
        free(widths);
        return false;
    }
    for (usize row = 0; row < rows && success; row++) {
        usize last_column = 0;
        bool have_entry = false;

        for (usize column = 0; column < columns; column++) {
            if (column * rows + row < count) {
                last_column = column;
                have_entry = true;
            }
        }
        if (!have_entry) continue;
        for (usize column = 0; column <= last_column; column++) {
            usize index = column * rows + row;
            if (index >= count) continue;
            if (!write_entry(&entries[index], styled_output)) {
                success = false;
                break;
            }
            if (column != last_column &&
                !write_spaces(widths[column] - entries[index].display_width +
                              LS_COLUMN_GAP)) {
                success = false;
                break;
            }
        }
        if (success && console_write("\n", 1U) != 1) success = false;
    }
    if (console_end_transaction() != MG_OK) success = false;
    free(widths);
    return success;
}

int main(int argc, char **argv)
{
    char path[256];
    const char *requested_path = ".";
    mg_handle_t directory;
    mg_directory_entry_t batch[MG_DIRECTORY_BATCH_MAX];
    ls_entry_t *entries = NULL;
    usize entry_count = 0;
    usize entry_capacity = 0;
    usize terminal_width = 1U;
    bool one_per_line = false;
    mg_terminal_size_t terminal_size = {0};
    mg_terminal_capabilities_t terminal_capabilities = {0};
    bool styled_output = false;
    mg_result_t result;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc > 3 ||
        (argc == 2 && argv[1][0] == '-' && strcmp(argv[1], "-1") != 0) ||
        (argc == 3 && strcmp(argv[1], "-1") != 0)) {
        command_usage_error(argv[0], "ls [-1] [path]",
                            argc > 1 ? argv[1] : NULL);
        return 1;
    }
    if (argc >= 2 && !strcmp(argv[1], "-1")) {
        one_per_line = true;
        if (argc == 3) requested_path = argv[2];
    } else if (argc == 2) {
        requested_path = argv[1];
    }
    styled_output = terminal_get_capabilities(&terminal_capabilities) == MG_OK &&
        (terminal_capabilities.capabilities & MG_TERMINAL_CAP_STYLED_OUTPUT) != 0;
    if (!command_resolve_path(requested_path, path, sizeof(path))) {
        printf("Could not list \"%s\": invalid path.\n", requested_path);
        return 1;
    }
    result = directory_open(path);
    if (result_is_error(result)) {
        printf("Could not list \"%s\": %s.\n", requested_path,
               error_string(result));
        return 1;
    }
    directory = (mg_handle_t)result;
    for (;;) {
        usize batch_count = 0;

        result = directory_read_batch(directory, batch,
                                      MG_DIRECTORY_BATCH_MAX, &batch_count);
        if (result == MG_ERR_END_OF_FILE) break;
        if (result_is_error(result)) {
            printf("Could not read directory: %s.\n", error_string(result));
            (void)handle_close(directory);
            free(entries);
            return 1;
        }
        if (!append_entries(&entries, &entry_count, &entry_capacity,
                            batch, batch_count, styled_output)) {
            printf("Could not list \"%s\": not enough memory.\n",
                   requested_path);
            (void)handle_close(directory);
            free(entries);
            return 1;
        }
    }
    (void)handle_close(directory);

    if (!one_per_line && terminal_get_size(&terminal_size) == MG_OK &&
        terminal_size.columns) terminal_width = terminal_size.columns;
    result = one_per_line
        ? (render_one_per_line(entries, entry_count, styled_output)
           ? MG_OK : MG_ERR_IO)
        : (render_columns(entries, entry_count, terminal_width, styled_output)
           ? MG_OK : MG_ERR_IO);
    free(entries);
    if (result != MG_OK) {
        printf("Could not write directory listing.\n");
        return 1;
    }
    return 0;
}
