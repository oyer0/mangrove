#include <mangrove.h>
#include <stdio.h>
#include "../common/help.h"
#include "../common/table.h"

static const mg_table_column_t MEMORY_LABEL_COLUMN = {
    MG_TABLE_ALIGN_LEFT
};
static const mg_table_column_t MEMORY_VALUE_COLUMN = {
    MG_TABLE_ALIGN_RIGHT
};

static mg_table_row_t memory_rows[MG_TABLE_MAX_ROWS];

static bool append_bytes(mg_table_t *table, const char *label, u64 bytes)
{
    char value[32];
    mg_table_row_t *row;

    if (bytes >= 1024ULL * 1024ULL * 1024ULL)
        (void)snprintf(value, sizeof(value), "%llu GiB",
                       bytes / (1024ULL * 1024ULL * 1024ULL));
    else if (bytes >= 1024ULL * 1024ULL)
        (void)snprintf(value, sizeof(value), "%llu MiB",
                       bytes / (1024ULL * 1024ULL));
    else if (bytes >= 1024ULL)
        (void)snprintf(value, sizeof(value), "%llu KiB", bytes / 1024ULL);
    else
        (void)snprintf(value, sizeof(value), "%llu B", bytes);
    row = table_row_begin(table);
    if (!row) return false;
    table_row_column(row, &MEMORY_LABEL_COLUMN, label);
    table_row_column(row, &MEMORY_VALUE_COLUMN, value);
    return true;
}

int main(int argc, char **argv)
{
    mg_system_memory_info_t info;
    mg_result_t result;
    mg_table_t table;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 1) {
        command_usage_error(argv[0], "mem", argc > 1 ? argv[1] : NULL);
        return 1;
    }
    result = memory_info(&info);
    if (result != MG_OK) {
        printf("mem: %s\n", error_string(result));
        return 1;
    }
    table_init(&table, memory_rows, MG_TABLE_MAX_ROWS);
    if (!append_bytes(&table, "Total:", info.physical_total_bytes) ||
        !append_bytes(&table, "Used:", info.physical_used_bytes) ||
        !append_bytes(&table, "Free:", info.physical_free_bytes) ||
        !append_bytes(&table, "Kernel heap:", info.kernel_heap_used_bytes) ||
        !table_render(&table)) {
        printf("mem: invalid table data.\n");
        return 1;
    }
    return 0;
}
