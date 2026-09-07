#pragma once

#include <mg/object.h>
#include <mg/types.h>
#include <stdio.h>
#include <string.h>

/* Tables are collected before output so every row uses one set of widths. */
#define MG_TABLE_MAX_COLUMNS       8U
#define MG_TABLE_MAX_ROWS          256U
#define MG_TABLE_CELL_CAPACITY     256U
#define MG_TABLE_ROW_CAPACITY      \
    (MG_TABLE_MAX_COLUMNS * MG_TABLE_CELL_CAPACITY + \
     (MG_TABLE_MAX_COLUMNS - 1U) * 2U + 1U)

typedef enum {
    MG_TABLE_ALIGN_LEFT,
    MG_TABLE_ALIGN_RIGHT,
} mg_table_alignment_t;

typedef struct {
    mg_table_alignment_t alignment;
} mg_table_column_t;

typedef struct {
    char cells[MG_TABLE_MAX_COLUMNS][MG_TABLE_CELL_CAPACITY];
    usize lengths[MG_TABLE_MAX_COLUMNS];
    usize display_widths[MG_TABLE_MAX_COLUMNS];
    mg_table_alignment_t alignments[MG_TABLE_MAX_COLUMNS];
    usize column_count;
} mg_table_row_t;

typedef struct {
    mg_table_row_t *rows;
    usize row_count;
    usize row_capacity;
} mg_table_t;

static void table_row_init(mg_table_row_t *row)
{
    if (!row) return;
    memset(row, 0, sizeof(*row));
}

static void table_init(mg_table_t *table, mg_table_row_t *rows,
                       usize row_capacity)
{
    if (!table) return;
    table->rows = rows;
    table->row_count = 0;
    table->row_capacity = row_capacity > MG_TABLE_MAX_ROWS
        ? MG_TABLE_MAX_ROWS : row_capacity;
}

static mg_table_row_t *table_row_begin(mg_table_t *table)
{
    mg_table_row_t *row;

    if (!table || !table->rows || table->row_count >= table->row_capacity)
        return NULL;
    row = &table->rows[table->row_count++];
    table_row_init(row);
    return row;
}

/* Table cells are UTF-8 strings.  Mangrove currently treats every decoded
 * scalar as one terminal cell; malformed bytes are retained as one-cell
 * fallback bytes so a bad value cannot desynchronize layout. */
static void table_text_metrics(const char *text, usize *byte_length,
                               usize *display_width)
{
    usize bytes = 0;
    usize width = 0;
    usize source_length = 0;

    if (!text) {
        if (byte_length) *byte_length = 1U;
        if (display_width) *display_width = 1U;
        return;
    }
    while (source_length + 1U < MG_TABLE_CELL_CAPACITY &&
           text[source_length]) source_length++;
    while (bytes < source_length) {
        u8 first = (u8)text[bytes];
        usize sequence = 1U;
        u32 codepoint = first;
        bool valid = true;

        if (first >= 0xc2U && first <= 0xdfU) {
            sequence = 2U;
            codepoint = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            sequence = 3U;
            codepoint = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            sequence = 4U;
            codepoint = first & 0x07U;
        } else if (first >= 0x80U) {
            valid = false;
        }
        if (valid && sequence > 1U && bytes + sequence <= source_length) {
            for (usize part = 1U; part < sequence; part++) {
                u8 byte = (u8)text[bytes + part];
                if (!byte || (byte & 0xc0U) != 0x80U) {
                    valid = false;
                    break;
                }
                codepoint = (codepoint << 6) | (byte & 0x3fU);
            }
            if (valid && ((sequence == 2U && codepoint < 0x80U) ||
                          (sequence == 3U && codepoint < 0x800U) ||
                          (sequence == 4U && codepoint < 0x10000U) ||
                          (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
                          codepoint > 0x10ffffU)) valid = false;
        } else if (sequence > 1U) valid = false;
        bytes += valid ? sequence : 1U;
        width++;
    }
    if (byte_length) *byte_length = bytes;
    if (display_width) *display_width = width;
}

static void table_row_column(mg_table_row_t *row,
                             const mg_table_column_t *column,
                             const char *text)
{
    usize index;
    usize length;
    usize display_width;

    if (!row || !column || row->column_count >= MG_TABLE_MAX_COLUMNS) return;
    if (!text) text = "-";
    index = row->column_count++;
    table_text_metrics(text, &length, &display_width);
    memcpy(row->cells[index], text, length);
    row->cells[index][length] = '\0';
    row->lengths[index] = length;
    row->display_widths[index] = display_width;
    row->alignments[index] = column->alignment;
}

static void table_row_u64_column(mg_table_row_t *row,
                                 const mg_table_column_t *column, u64 value)
{
    char text[32];

    (void)snprintf(text, sizeof(text), "%llu", value);
    table_row_column(row, column, text);
}

static void table_row_u32_column(mg_table_row_t *row,
                                 const mg_table_column_t *column, u32 value)
{
    char text[16];

    (void)snprintf(text, sizeof(text), "%u", value);
    table_row_column(row, column, text);
}

/* Kept as a source-compatible marker for callers.  Separators are emitted by
 * table_render(), after the complete table has established its widths. */
static void table_row_gap(mg_table_row_t *row)
{
    (void)row;
}

static bool table_output_append(char *output, usize *output_length,
                                usize output_capacity, const char *text,
                                usize text_length)
{
    usize available;

    if (!output || !output_length || !text || output_capacity == 0U ||
        *output_length >= output_capacity) return false;
    available = output_capacity - 1U - *output_length;
    if (text_length > available) return false;
    memcpy(output + *output_length, text, text_length);
    *output_length += text_length;
    output[*output_length] = '\0';
    return true;
}

static bool table_output_spaces(char *output, usize *output_length,
                                usize output_capacity, usize count)
{
    static const char spaces[] = "                                ";

    while (count) {
        usize chunk = count < sizeof(spaces) - 1U ? count :
                      sizeof(spaces) - 1U;
        if (!chunk || !table_output_append(output, output_length,
                                           output_capacity, spaces, chunk))
            return false;
        count -= chunk;
    }
    return true;
}

static bool table_calculate_widths(const mg_table_t *table,
                                   usize widths[MG_TABLE_MAX_COLUMNS],
                                   usize *out_column_count)
{
    usize column_count;

    if (!table || !table->rows || !table->row_count || !widths)
        return false;
    column_count = table->rows[0].column_count;
    if (!column_count || column_count > MG_TABLE_MAX_COLUMNS) return false;
    for (usize row_index = 0; row_index < table->row_count; row_index++) {
        const mg_table_row_t *row = &table->rows[row_index];

        if (row->column_count != column_count) return false;
        for (usize column = 0; column < column_count; column++) {
            if (row->display_widths[column] > widths[column])
                widths[column] = row->display_widths[column];
            if (row_index &&
                row->alignments[column] !=
                    table->rows[0].alignments[column]) return false;
        }
    }
    if (out_column_count) *out_column_count = column_count;
    return true;
}

static usize table_display_width_from_widths(
    const usize widths[MG_TABLE_MAX_COLUMNS], usize column_count)
{
    usize total = 0;

    if (!widths || !column_count || column_count > MG_TABLE_MAX_COLUMNS)
        return 0;
    for (usize column = 0; column < column_count; column++)
        total += widths[column];
    if (column_count > 1U) total += (column_count - 1U) * 2U;
    return total;
}

/* Format one complete row using widths calculated over the whole table.  The
 * result does not include a newline so callers such as full-screen programs
 * can compare and rewrite rows without changing terminal history. */
static bool table_format_row(const mg_table_t *table, usize row_index,
                             const usize widths[MG_TABLE_MAX_COLUMNS],
                             usize column_count, char *output,
                             usize output_capacity, usize *out_length)
{
    const mg_table_row_t *row;
    usize length = 0;

    if (!table || !table->rows || row_index >= table->row_count ||
        !widths || !column_count || column_count > MG_TABLE_MAX_COLUMNS ||
        !output || !output_capacity || !out_length) return false;
    row = &table->rows[row_index];
    if (row->column_count != column_count) return false;
    output[0] = '\0';
    for (usize column = 0; column < column_count; column++) {
        usize cell_length = row->lengths[column];
        usize padding = widths[column] - row->display_widths[column];

        if (column && !table_output_append(output, &length,
                                           output_capacity, "  ", 2U))
            return false;
        if (row->alignments[column] == MG_TABLE_ALIGN_RIGHT &&
            !table_output_spaces(output, &length, output_capacity, padding))
            return false;
        if (!table_output_append(output, &length, output_capacity,
                                 row->cells[column], cell_length))
            return false;
        if (row->alignments[column] == MG_TABLE_ALIGN_LEFT &&
            !table_output_spaces(output, &length, output_capacity, padding))
            return false;
    }
    *out_length = length;
    return true;
}

static bool table_render(const mg_table_t *table)
{
    usize widths[MG_TABLE_MAX_COLUMNS] = {0};
    usize column_count;

    if (!table || !table->rows || !table->row_count) return true;
    if (!table_calculate_widths(table, widths, &column_count)) return false;
    if (console_begin_transaction() != MG_OK) return false;
    for (usize row_index = 0; row_index < table->row_count; row_index++) {
        const mg_table_row_t *row = &table->rows[row_index];
        char output[MG_TABLE_ROW_CAPACITY];
        usize length = 0;

        if (!table_format_row(table, row_index, widths, column_count,
                              output, sizeof(output), &length) ||
            !table_output_append(output, &length, sizeof(output), "\n", 1U) ||
            console_write(output, length) != (mg_result_t)length) {
            (void)console_end_transaction();
            return false;
        }
    }
    return console_end_transaction() == MG_OK;
}
