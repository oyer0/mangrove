/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <string.h>

#include <mg/object.h>
#include <mg/types.h>

#include "../../userspace/common/table.h"

static mg_table_row_t rows[1];
static u8 literal_output[1024];
static usize literal_length;
static usize ordinary_write_count;

mg_result_t console_begin_transaction(void)
{
    return MG_OK;
}

mg_result_t console_end_transaction(void)
{
    return MG_OK;
}

mg_result_t console_write(const void *buffer, usize length)
{
    (void)buffer;
    (void)length;
    ordinary_write_count++;
    return (mg_result_t)length;
}

mg_result_t console_write_literal(const void *buffer, usize length)
{
    if (length > sizeof(literal_output) - literal_length)
        return MG_ERR_BUFFER_TOO_SMALL;
    memcpy(literal_output + literal_length, buffer, length);
    literal_length += length;
    return (mg_result_t)length;
}

static bool contains_bytes(const u8 *buffer, usize length,
                           const u8 *needle, usize needle_length)
{
    if (!buffer || !needle || needle_length > length) return false;
    for (usize offset = 0; offset <= length - needle_length; offset++)
        if (memcmp(buffer + offset, needle, needle_length) == 0) return true;
    return false;
}

int main(void)
{
    static const mg_table_column_t column = {MG_TABLE_ALIGN_LEFT};
    static const u8 cls[] = "\x1b{cls}";
    static const u8 position[] = "\x1b{at:1,1}";
    mg_table_t table;

    table_init(&table, rows, 1U);
    mg_table_row_t *row = table_row_begin(&table);
    assert(row != NULL);
    table_row_column(row, &column, (const char *)cls);
    table_row_column(row, &column, (const char *)position);

    assert(table_render(&table));
    assert(ordinary_write_count == 0U);
    assert(contains_bytes(literal_output, literal_length, cls,
                          sizeof(cls) - 1U));
    assert(contains_bytes(literal_output, literal_length, position,
                          sizeof(position) - 1U));
    assert(literal_output[literal_length - 1U] == '\n');
    return 0;
}
