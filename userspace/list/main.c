/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <mg/utf8.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/help.h"
#include "../common/path.h"

#define LIST_COLUMN_GAP 2U
#define LIST_INITIAL_CAPACITY MG_DIRECTORY_BATCH_MAX
#define LIST_TREE_PREFIX_CAPACITY 128U
#define LIST_MAX_TREE_DEPTH 16U
#define LIST_MAX_TREE_ENTRIES 4096U
#define LIST_OWNER_CAPACITY (MG_IDENTITY_USERNAME_CAPACITY + 12U)
#define LIST_SIZE_CAPACITY 32U
#define LIST_DISPLAY_BYTES_PER_INPUT 10U

typedef struct {
    mg_directory_entry_ex_t entry;
    mg_path_info_t info;
    char owner[LIST_OWNER_CAPACITY];
    char permissions[6];
    char size[LIST_SIZE_CAPACITY];
    char tree_prefix[LIST_TREE_PREFIX_CAPACITY];
    usize display_width;
    bool has_info;
} list_entry_t;

typedef struct {
    bool long_listing;
    bool tree;
    usize tree_depth;
    const char *path;
    const char **paths;
    usize path_count;
} list_options_t;

static bool needs_directory_suffix(const list_entry_t *entry,
                                   bool styled_output)
{
    return entry && !styled_output &&
           entry->entry.type == MG_PATH_TYPE_DIRECTORY &&
           strcmp(entry->entry.name, "/") != 0;
}

static bool unicode_whitespace(u32 scalar)
{
    return scalar == 0x0009U || (scalar >= 0x000aU && scalar <= 0x000dU) ||
           scalar == 0x0020U || scalar == 0x0085U || scalar == 0x00a0U ||
           scalar == 0x1680U || (scalar >= 0x2000U && scalar <= 0x200aU) ||
           scalar == 0x2028U || scalar == 0x2029U || scalar == 0x202fU ||
           scalar == 0x205fU || scalar == 0x3000U;
}

static bool unicode_invisible(u32 scalar)
{
    return scalar <= 0x001fU || (scalar >= 0x007fU && scalar <= 0x009fU) ||
           (scalar >= 0xfdd0U && scalar <= 0xfdefU) ||
           ((scalar & 0xfffeU) == 0xfffeU && scalar <= 0x10ffffU) ||
           scalar == 0x00adU || scalar == 0x034fU ||
           (scalar >= 0x0600U && scalar <= 0x0605U) || scalar == 0x061cU ||
           scalar == 0x06ddU || scalar == 0x070fU || scalar == 0x08e2U ||
           (scalar >= 0x0890U && scalar <= 0x0891U) || scalar == 0x115fU ||
           scalar == 0x1160U || (scalar >= 0x17b4U && scalar <= 0x17b5U) ||
           (scalar >= 0x180bU && scalar <= 0x180fU) ||
           (scalar >= 0x200bU && scalar <= 0x200fU) ||
           (scalar >= 0x202aU && scalar <= 0x202eU) ||
           (scalar >= 0x2060U && scalar <= 0x206fU) ||
           scalar == 0x3164U ||
           (scalar >= 0xfe00U && scalar <= 0xfe0fU) || scalar == 0xfeffU ||
           scalar == 0xffa0U ||
           (scalar >= 0xfff0U && scalar <= 0xfff8U) ||
           (scalar >= 0xfff9U && scalar <= 0xfffbU) ||
           scalar == 0x110bdU || scalar == 0x110cdU ||
           (scalar >= 0x13430U && scalar <= 0x1343fU) ||
           (scalar >= 0x1bca0U && scalar <= 0x1bca3U) ||
           (scalar >= 0x1d173U && scalar <= 0x1d17aU) ||
           scalar == 0xe0001U ||
           (scalar >= 0xe0020U && scalar <= 0xe007fU) ||
           (scalar >= 0xe0100U && scalar <= 0xe01efU);
}

static bool name_needs_quotes(const char *name)
{
    usize length;

    if (!name) return false;
    length = strlen(name);
    for (usize offset = 0; offset < length;) {
        u32 scalar;
        usize consumed = 0;
        mg_utf8_result_t decoded = mg_utf8_decode_one(
            (const u8 *)name + offset, length - offset, &scalar, &consumed);

        if (decoded != MG_UTF8_OK || !consumed) return true;
        if (scalar == '\'' || scalar == '"' || scalar == '\\' ||
            unicode_whitespace(scalar) || unicode_invisible(scalar))
            return true;
        offset += consumed;
    }
    return false;
}

static bool append_display(char *output, usize capacity, usize *length,
                           usize *width, const char *text, usize count,
                           usize cells)
{
    if (!output || !length || !width || !text || *length > capacity ||
        count > capacity - *length) return false;
    memcpy(output + *length, text, count);
    *length += count;
    if (*width > (usize)-1 - cells) return false;
    *width += cells;
    return true;
}

static bool append_codepoint_escape(char *output, usize capacity,
                                    usize *length, usize *width, u32 scalar)
{
    static const char digits[] = "0123456789ABCDEF";
    char escape[10];
    usize digit_count = 4U;
    u32 value = scalar;

    while (value > 0xffffU) {
        digit_count++;
        value >>= 4U;
    }
    escape[0] = '\\';
    escape[1] = 'u';
    escape[2] = '{';
    for (usize index = 0; index < digit_count; index++) {
        usize shift = 4U * (digit_count - index - 1U);
        escape[3U + index] = digits[(scalar >> shift) & 0x0fU];
    }
    escape[3U + digit_count] = '}';
    return append_display(output, capacity, length, width, escape,
                          digit_count + 4U, digit_count + 4U);
}

static bool append_byte_escape(char *output, usize capacity,
                               usize *length, usize *width, u8 byte)
{
    static const char digits[] = "0123456789ABCDEF";
    char escape[4] = { '\\', 'x', digits[byte >> 4U],
                       digits[byte & 0x0fU] };

    return append_display(output, capacity, length, width, escape,
                          sizeof(escape), sizeof(escape));
}

static bool format_entry_name(const list_entry_t *entry, bool styled_output,
                              char *output, usize capacity, usize *out_length,
                              usize *out_width)
{
    const char *name;
    usize name_length;
    usize length = 0;
    usize width = 0;
    bool quoted;

    if (!entry || !output || !out_length || !out_width || !capacity)
        return false;
    name = entry->entry.name;
    name_length = strlen(name);
    quoted = name_needs_quotes(name);
    if (quoted && !append_display(output, capacity, &length, &width,
                                   "'", 1U, 1U)) return false;
    for (usize offset = 0; offset < name_length;) {
        u32 scalar = 0;
        usize consumed = 0;
        mg_utf8_result_t decoded = mg_utf8_decode_one(
            (const u8 *)name + offset, name_length - offset, &scalar,
            &consumed);

        if (decoded != MG_UTF8_OK || !consumed) {
            if (!append_byte_escape(output, capacity, &length, &width,
                                    (u8)name[offset])) return false;
            offset++;
            continue;
        }
        if (scalar == '\\' || scalar == '\'' || scalar == '"') {
            char escaped[2] = { '\\', (char)scalar };

            if (!append_display(output, capacity, &length, &width, escaped,
                                sizeof(escaped), sizeof(escaped))) return false;
        } else if (scalar == '\t' || scalar == '\n' || scalar == '\r' ||
                   scalar == '\b' || scalar == '\f' || scalar == '\v') {
            char escaped[2] = { '\\', scalar == '\t' ? 't' :
                scalar == '\n' ? 'n' : scalar == '\r' ? 'r' :
                scalar == '\b' ? 'b' : scalar == '\f' ? 'f' : 'v' };

            if (!append_display(output, capacity, &length, &width, escaped,
                                sizeof(escaped), sizeof(escaped))) return false;
        } else if (scalar == ' ') {
            if (!append_display(output, capacity, &length, &width,
                                name + offset, consumed, 1U)) return false;
        } else if (unicode_invisible(scalar) || unicode_whitespace(scalar)) {
            if (!append_codepoint_escape(output, capacity, &length, &width,
                                         scalar)) return false;
        } else {
            if (!append_display(output, capacity, &length, &width,
                                name + offset, consumed, 1U)) return false;
        }
        offset += consumed;
    }
    if (quoted && !append_display(output, capacity, &length, &width,
                                   "'", 1U, 1U)) return false;
    if (needs_directory_suffix(entry, styled_output) &&
        !append_display(output, capacity, &length, &width, "/", 1U, 1U))
        return false;
    if (length >= capacity) return false;
    output[length] = '\0';
    *out_length = length;
    *out_width = width;
    return true;
}

static usize entry_display_width(const list_entry_t *entry, bool styled_output)
{
    usize name_length;
    usize capacity;
    char *display;
    usize length;
    usize width;

    if (!entry) return (usize)-1;
    name_length = strlen(entry->entry.name);
    if (name_length > ((usize)-1 - 4U) / LIST_DISPLAY_BYTES_PER_INPUT)
        return (usize)-1;
    capacity = name_length * LIST_DISPLAY_BYTES_PER_INPUT + 4U;
    display = (char *)malloc(capacity);
    if (!display) return (usize)-1;
    bool formatted = format_entry_name(entry, styled_output, display, capacity,
                                       &length, &width);
    free(display);
    if (!formatted) return (usize)-1;
    return width;
}

static bool copy_string(char *destination, usize capacity,
                        const char *source)
{
    usize length;

    if (!destination || !source || capacity == 0U) return false;
    length = strlen(source);
    if (length >= capacity) return false;
    memcpy(destination, source, length + 1U);
    return true;
}

static bool append_entry(list_entry_t **entries, usize *count,
                         usize *capacity, const list_entry_t *entry)
{
    usize required;

    if (!entries || !count || !capacity || !entry) return false;
    if (*count == LIST_MAX_TREE_ENTRIES || *count == (usize)-1) return false;
    required = *count + 1U;
    if (required > *capacity) {
        usize new_capacity = *capacity ? *capacity : LIST_INITIAL_CAPACITY;
        list_entry_t *replacement;

        while (new_capacity < required) {
            if (new_capacity > (usize)-1 / 2U) {
                new_capacity = required;
                break;
            }
            new_capacity *= 2U;
        }
        if (new_capacity > (usize)-1 / sizeof(**entries)) return false;
        replacement = (list_entry_t *)realloc(
            *entries, new_capacity * sizeof(**entries));
        if (!replacement) return false;
        *entries = replacement;
        *capacity = new_capacity;
    }
    (*entries)[*count] = *entry;
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

static usize layout_column_width(const list_entry_t *entries, usize count,
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

static bool layout_fits(const list_entry_t *entries, usize count,
                        usize columns, usize terminal_width)
{
    usize total = 0;
    usize used_columns = layout_used_columns(count, columns);

    for (usize column = 0; column < used_columns; column++) {
        usize width = layout_column_width(entries, count, columns, column);

        if (column && total > (usize)-1 - LIST_COLUMN_GAP) return false;
        if (column) total += LIST_COLUMN_GAP;
        if (total > (usize)-1 - width) return false;
        total += width;
        if (total > terminal_width) return false;
    }
    return true;
}

static usize choose_columns(const list_entry_t *entries, usize count,
                            usize terminal_width)
{
    usize maximum;
    usize columns;

    if (!entries || !count || !terminal_width) return 1U;
    maximum = count;
    if (terminal_width <= (usize)-1 - LIST_COLUMN_GAP) {
        usize width_limited =
            (terminal_width + LIST_COLUMN_GAP) / (1U + LIST_COLUMN_GAP);

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

static bool write_string(const char *text)
{
    usize length;

    if (!text) return false;
    length = strlen(text);
    return console_write(text, length) == (mg_result_t)length;
}

static bool write_name(const list_entry_t *entry, bool styled_output)
{
    usize name_length, capacity;
    char *display;
    usize length;
    usize width;
    mg_result_t result;

    if (!entry) return false;
    name_length = strlen(entry->entry.name);
    if (name_length > ((usize)-1 - 4U) / LIST_DISPLAY_BYTES_PER_INPUT)
        return false;
    capacity = name_length * LIST_DISPLAY_BYTES_PER_INPUT + 4U;
    display = (char *)malloc(capacity);
    if (!display) return false;
    if (!format_entry_name(entry, styled_output, display, capacity,
                           &length, &width)) {
        free(display);
        return false;
    }
    (void)width;
    result = styled_output
        ? terminal_write_semantic(
              display, length,
              entry->entry.type == MG_PATH_TYPE_DIRECTORY
                  ? MG_TERMINAL_STYLE_DIRECTORY : MG_TERMINAL_STYLE_DEFAULT)
        : console_write(display, length);
    free(display);
    return result == (mg_result_t)length;
}

static bool write_aligned_field(const char *text, usize width,
                                bool right_aligned)
{
    usize length;

    if (!text) return false;
    length = strlen(text);
    if (length > width) return false;
    if (right_aligned && !write_spaces(width - length)) return false;
    if (!write_string(text)) return false;
    if (!right_aligned && !write_spaces(width - length)) return false;
    return true;
}

static bool fill_owner(const mg_path_info_t *info,
                       const mg_account_info_t *accounts, usize account_count,
                       char *output, usize capacity)
{
    if (!info || !output || capacity == 0U) return false;
    if (info->owner_uid == MG_UID_SYSTEM)
        return copy_string(output, capacity, "system");
    for (usize index = 0; index < account_count; index++) {
        if (accounts[index].uid == info->owner_uid)
            return copy_string(output, capacity, accounts[index].username);
    }
    return snprintf(output, capacity, "%u", info->owner_uid) >= 0 &&
           strlen(output) < capacity;
}

static bool fill_metadata(list_entry_t *entry, const char *path,
                          const mg_account_info_t *accounts,
                          usize account_count)
{
    mg_result_t result;
    u32 permissions;

    if (!entry || !path) return false;
    result = path_info(path, &entry->info);
    if (result_is_error(result)) return false;
    entry->has_info = true;
    entry->entry.type = entry->info.type;
    permissions = entry->info.permissions & MG_PERMISSION_KNOWN;
    entry->permissions[0] = (permissions & MG_PERMISSION_OWNER_READ) ? 'r' : '-';
    entry->permissions[1] = (permissions & MG_PERMISSION_OWNER_WRITE) ? 'w' : '-';
    entry->permissions[2] = ':';
    entry->permissions[3] = (permissions & MG_PERMISSION_OTHER_READ) ? 'r' : '-';
    entry->permissions[4] = (permissions & MG_PERMISSION_OTHER_WRITE) ? 'w' : '-';
    entry->permissions[5] = '\0';
    if (!fill_owner(&entry->info, accounts, account_count,
                    entry->owner, sizeof(entry->owner))) return false;
    if (entry->info.type == MG_PATH_TYPE_DIRECTORY) {
        if (!copy_string(entry->size, sizeof(entry->size), "-")) return false;
    } else {
        if (snprintf(entry->size, sizeof(entry->size), "%llu",
                     entry->info.size) < 0 ||
            strlen(entry->size) >= sizeof(entry->size)) return false;
    }
    return true;
}

static bool load_accounts(mg_account_info_t *accounts, usize *count)
{
    mg_result_t result;

    if (!accounts || !count) return false;
    *count = 0;
    result = account_list(accounts, MG_ACCOUNT_MAX_RECORDS, count);
    return !result_is_error(result);
}

static mg_result_t collect_directory(const char *path, bool need_info,
                                     bool styled_output,
                                     const mg_account_info_t *accounts,
                                     usize account_count,
                                     list_entry_t **entries, usize *count,
                                     usize *capacity)
{
    mg_handle_t directory;
    mg_directory_entry_ex_t *batch;
    mg_result_t result;

    if (!path || !entries || !count || !capacity) return MG_ERR_BAD_ARGUMENT;
    result = directory_open(path);
    if (result_is_error(result)) return result;
    directory = (mg_handle_t)result;
    batch = (mg_directory_entry_ex_t *)malloc(
        MG_DIRECTORY_BATCH_MAX * sizeof(*batch));
    if (!batch) {
        (void)handle_close(directory);
        return MG_ERR_NO_MEMORY;
    }
    for (;;) {
        usize batch_count = 0;

        result = directory_read_batch_ex(directory, batch,
                                         MG_DIRECTORY_BATCH_MAX, &batch_count);
        if (result == MG_ERR_END_OF_FILE) break;
        if (result_is_error(result)) {
            (void)handle_close(directory);
            free(batch);
            return result;
        }
        for (usize index = 0; index < batch_count; index++) {
            COMMAND_PATH_AUTO char *child_path = NULL;
            list_entry_t entry = {0};

            entry.entry = batch[index];
            if (!command_path_join_alloc(path, entry.entry.name,
                                         &child_path) ||
                (need_info && !fill_metadata(&entry, child_path, accounts,
                                              account_count))) {
                (void)handle_close(directory);
                free(batch);
                return MG_ERR_IO;
            }
            entry.display_width = entry_display_width(&entry, styled_output);
            if (entry.display_width == (usize)-1) {
                (void)handle_close(directory);
                free(batch);
                return MG_ERR_BUFFER_TOO_SMALL;
            }
            if (!append_entry(entries, count, capacity, &entry)) {
                (void)handle_close(directory);
                free(batch);
                return MG_ERR_NO_MEMORY;
            }
        }
    }
    (void)handle_close(directory);
    free(batch);
    return MG_OK;
}

static bool render_columns(const list_entry_t *entries, usize count,
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
            if (!write_name(&entries[index], styled_output)) {
                success = false;
                break;
            }
            if (column != last_column &&
                !write_spaces(widths[column] - entries[index].display_width +
                              LIST_COLUMN_GAP)) {
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

static bool render_long(const list_entry_t *entries, usize count,
                        bool styled_output)
{
    usize owner_width = 0;
    usize size_width = 0;
    bool success = true;

    for (usize index = 0; index < count; index++) {
        usize owner_length = strlen(entries[index].owner);
        usize size_length = strlen(entries[index].size);

        if (owner_length > owner_width) owner_width = owner_length;
        if (size_length > size_width) size_width = size_length;
    }
    if (console_begin_transaction() != MG_OK) return false;
    for (usize index = 0; index < count && success; index++) {
        const list_entry_t *entry = &entries[index];
        char type = entry->entry.type == MG_PATH_TYPE_DIRECTORY ? 'd' :
                    entry->entry.type == MG_PATH_TYPE_FILE ? 'f' : '?';

        if (console_write(&type, 1U) != 1 || !write_spaces(1U) ||
            !write_string(entry->permissions) || !write_spaces(1U) ||
            !write_aligned_field(entry->owner, owner_width, false) ||
            !write_spaces(1U) ||
            !write_aligned_field(entry->size, size_width, true) ||
            !write_spaces(1U) || !write_string(entry->tree_prefix) ||
            !write_name(entry, styled_output) || console_write("\n", 1U) != 1)
            success = false;
    }
    if (console_end_transaction() != MG_OK) success = false;
    return success;
}

static bool render_tree(const list_entry_t *entries, usize count,
                        bool styled_output)
{
    bool success = true;

    if (console_begin_transaction() != MG_OK) return false;
    for (usize index = 0; index < count && success; index++) {
        if (!write_string(entries[index].tree_prefix) ||
            !write_name(&entries[index], styled_output) ||
            console_write("\n", 1U) != 1) success = false;
    }
    if (console_end_transaction() != MG_OK) success = false;
    return success;
}

static bool append_tree_row(list_entry_t **rows, usize *row_count,
                            usize *row_capacity, const list_entry_t *source,
                            const char *prefix)
{
    list_entry_t row;

    if (!source || !prefix) return false;
    row = *source;
    if (!copy_string(row.tree_prefix, sizeof(row.tree_prefix), prefix))
        return false;
    return append_entry(rows, row_count, row_capacity, &row);
}

static mg_result_t collect_tree(const char *path, usize remaining_depth,
                                const char *indent, bool need_info,
                                bool styled_output,
                                const mg_account_info_t *accounts,
                                usize account_count, list_entry_t **rows,
                                usize *row_count, usize *row_capacity)
{
    list_entry_t *children = NULL;
    usize child_count = 0;
    usize child_capacity = 0;
    mg_result_t result;

    if (remaining_depth == 0U) return MG_OK;
    result = collect_directory(path, need_info, styled_output, accounts,
                               account_count, &children, &child_count,
                               &child_capacity);
    if (result_is_error(result)) {
        free(children);
        return result;
    }
    for (usize index = 0; index < child_count; index++) {
        char prefix[LIST_TREE_PREFIX_CAPACITY];
        char child_indent[LIST_TREE_PREFIX_CAPACITY];
        COMMAND_PATH_AUTO char *child_path = NULL;
        usize indent_length = strlen(indent);
        usize branch_length;
        usize continuation_length;
        bool last = index + 1U == child_count;
        const char *branch = last ? "└── " : "├── ";
        const char *continuation = last ? "    " : "│   ";

        branch_length = strlen(branch);
        continuation_length = strlen(continuation);
        if (indent_length + branch_length >= sizeof(prefix) ||
            indent_length + continuation_length >= sizeof(child_indent)) {
            free(children);
            return MG_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(prefix, indent, indent_length);
        memcpy(prefix + indent_length, branch, branch_length + 1U);
        memcpy(child_indent, indent, indent_length);
        memcpy(child_indent + indent_length, continuation,
               continuation_length + 1U);
        if (!append_tree_row(rows, row_count, row_capacity,
                             &children[index], prefix)) {
            free(children);
            return MG_ERR_NO_MEMORY;
        }
        if (remaining_depth > 1U &&
            children[index].entry.type == MG_PATH_TYPE_DIRECTORY) {
            if (!command_path_join_alloc(path, children[index].entry.name,
                                         &child_path)) {
                free(children);
                return MG_ERR_BUFFER_TOO_SMALL;
            }
            result = collect_tree(child_path, remaining_depth - 1U,
                                  child_indent, need_info, styled_output,
                                  accounts, account_count, rows, row_count,
                                  row_capacity);
            if (result == MG_ERR_ACCESS_DENIED) {
                /* The child remains visible, but its contents are not. */
                continue;
            }
            if (result_is_error(result)) {
                free(children);
                return result;
            }
        }
    }
    free(children);
    return MG_OK;
}

static bool parse_depth(const char *text, usize *depth)
{
    usize value = 0;

    if (!text || !*text || !depth) return false;
    for (const char *cursor = text; *cursor; cursor++) {
        usize digit;

        if (*cursor < '0' || *cursor > '9') return false;
        digit = (usize)(*cursor - '0');
        if (value > ((usize)-1 - digit) / 10U) return false;
        value = value * 10U + digit;
        if (value > LIST_MAX_TREE_DEPTH) return false;
    }
    if (value == 0U) return false;
    *depth = value;
    return true;
}

static bool parse_options(int argc, char **argv, list_options_t *options,
                          const char **bad_option)
{

    if (!options || !bad_option) return false;
    options->long_listing = false;
    options->tree = false;
    options->tree_depth = 0;
    options->path = ".";
    options->path_count = 0;
    *bad_option = NULL;
    for (int index = 1; index < argc; index++) {
        const char *argument = argv[index];

        if (!argument || argument[0] != '-' || argument[1] == '\0') {
            options->paths[options->path_count++] = argument;
            continue;
        }
        if (!strcmp(argument, "--long")) {
            options->long_listing = true;
            continue;
        }
        if (!strcmp(argument, "--tree")) {
            if (index + 1 >= argc ||
                !parse_depth(argv[++index], &options->tree_depth)) {
                *bad_option = index < argc ? argv[index] : argument;
                return false;
            }
            options->tree = true;
            continue;
        }
        if (argument[1] == '-') {
            *bad_option = argument;
            return false;
        }
        for (usize option_index = 1; argument[option_index]; option_index++) {
            char option = argument[option_index];

            if (option == 'l') {
                options->long_listing = true;
                continue;
            }
            if (option == 't') {
                if (argument[option_index + 1] != '\0' || index + 1 >= argc ||
                    !parse_depth(argv[++index], &options->tree_depth)) {
                    *bad_option = index < argc ? argv[index] : argument;
                    return false;
                }
                options->tree = true;
                break;
            }
            *bad_option = argument;
            return false;
        }
    }
    return true;
}

static bool make_target_entry(const char *path, const mg_path_info_t *info,
                              const mg_account_info_t *accounts,
                              usize account_count, list_entry_t *entry)
{
    const char *name = path;

    if (!path || !info || !entry) return false;
    memset(entry, 0, sizeof(*entry));
    for (const char *cursor = path; *cursor; cursor++)
        if (*cursor == '/' && cursor[1]) name = cursor + 1;
    if (name == path && path[0] == '/') name = "/";
    if (!copy_string(entry->entry.name, sizeof(entry->entry.name), name))
        return false;
    entry->entry.type = info->type;
    entry->entry.identifier = info->identifier;
    return fill_metadata(entry, path, accounts, account_count);
}

static int list_target(list_options_t options)
{
    const char *path = options.path;
    mg_path_info_t target_info;
    mg_account_info_t *accounts = NULL;
    usize account_count = 0;
    list_entry_t *entries = NULL;
    usize entry_count = 0;
    usize entry_capacity = 0;
    usize terminal_width = 1U;
    mg_terminal_size_t terminal_size = {0};
    mg_terminal_capabilities_t terminal_capabilities = {0};
    bool styled_output;
    mg_result_t result;

    styled_output = terminal_get_capabilities(&terminal_capabilities) == MG_OK &&
        (terminal_capabilities.capabilities & MG_TERMINAL_CAP_STYLED_OUTPUT) != 0;
    result = path_info(path, &target_info);
    if (result_is_error(result)) {
        printf("Could not list \"%s\": %s.\n", options.path,
               error_string(result));
        return 1;
    }
    if (options.long_listing) {
        accounts = (mg_account_info_t *)malloc(
            MG_ACCOUNT_MAX_RECORDS * sizeof(*accounts));
        if (accounts && !load_accounts(accounts, &account_count))
            account_count = 0;
    }
    if (target_info.type == MG_PATH_TYPE_FILE) {
        list_entry_t target;

        if (!make_target_entry(path, &target_info, accounts, account_count,
                               &target)) {
            printf("Could not list \"%s\": metadata unavailable.\n",
                   options.path);
            free(accounts);
            return 1;
        }
        target.display_width = entry_display_width(&target, styled_output);
        if (target.display_width == (usize)-1) {
            printf("Could not list \"%s\": filename is too long to display.\n",
                   options.path);
            free(accounts);
            return 1;
        }
        if (!append_entry(&entries, &entry_count, &entry_capacity, &target)) {
            free(entries);
            free(accounts);
            return 1;
        }
    } else if (target_info.type == MG_PATH_TYPE_DIRECTORY) {
        if (options.tree) {
            list_entry_t root;

            if (!make_target_entry(path, &target_info, accounts, account_count,
                                   &root) ||
                !append_tree_row(&entries, &entry_count, &entry_capacity,
                                 &root, "")) {
                printf("Could not list \"%s\": metadata unavailable.\n",
                       options.path);
                free(entries);
                free(accounts);
                return 1;
            }
            result = collect_tree(path, options.tree_depth, "",
                                  options.long_listing, styled_output,
                                  accounts, account_count, &entries,
                                  &entry_count, &entry_capacity);
            if (result_is_error(result)) {
                printf("Could not list \"%s\": %s.\n", options.path,
                       error_string(result));
                free(entries);
                free(accounts);
                return 1;
            }
        } else {
            result = collect_directory(path, options.long_listing,
                                       styled_output, accounts, account_count,
                                       &entries, &entry_count, &entry_capacity);
            if (result_is_error(result)) {
                printf("Could not list \"%s\": %s.\n", options.path,
                       error_string(result));
                free(entries);
                free(accounts);
                return 1;
            }
        }
    } else {
        printf("Could not list \"%s\": unsupported file type.\n",
               options.path);
        free(accounts);
        return 1;
    }

    if (!options.long_listing && !options.tree &&
        terminal_get_size(&terminal_size) == MG_OK && terminal_size.columns)
        terminal_width = terminal_size.columns;
    if (options.long_listing)
        result = render_long(entries, entry_count, styled_output) ? MG_OK :
                 MG_ERR_IO;
    else if (options.tree)
        result = render_tree(entries, entry_count, styled_output) ? MG_OK :
                 MG_ERR_IO;
    else
        result = render_columns(entries, entry_count, terminal_width,
                                styled_output) ? MG_OK : MG_ERR_IO;
    free(entries);
    free(accounts);
    if (result != MG_OK) {
        printf("Could not write directory listing.\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (command_help_requested(argc, argv)) return command_print_help(argv[0]);
    if (argc < 1) return 1;
    const char **paths = malloc((usize)argc * sizeof(*paths));
    if (!paths) { printf("Could not list: not enough memory.\n"); return 1; }
    list_options_t options = {.paths = paths};
    const char *bad_option;
    if (!parse_options(argc, argv, &options, &bad_option)) {
        command_usage_error(argv[0], "list [-l|--long] [-t|--tree DEPTH] [path...]", bad_option);
        free(paths);
        return 1;
    }
    if (!options.path_count) paths[options.path_count++] = ".";
    int status = 0;
    for (usize index = 0; index < options.path_count; index++) {
        options.path = paths[index];
        if (list_target(options) != 0) status = 1;
    }
    free(paths);
    return status;
}
