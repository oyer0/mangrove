#include <mangrove.h>
#include <mg/device_service.h>
#include <stdio.h>
#include <string.h>
#include "../common/help.h"
#include "../common/storage_snapshot.h"
#include "../common/table.h"

#define LSDISK_LABEL_COLUMN 4U
#define LSDISK_MOUNT_COLUMN 5U
#define LSDISK_DETAIL_MODEL_COLUMN 2U

static const mg_table_column_t DISK_NAME_COLUMN = { MG_TABLE_ALIGN_LEFT };
static const mg_table_column_t DISK_SIZE_COLUMN = { MG_TABLE_ALIGN_RIGHT };
static const mg_table_column_t DISK_TYPE_COLUMN = { MG_TABLE_ALIGN_LEFT };
static const mg_table_column_t DISK_FILESYSTEM_COLUMN = { MG_TABLE_ALIGN_LEFT };
static const mg_table_column_t DISK_LABEL_COLUMN = { MG_TABLE_ALIGN_LEFT };
static const mg_table_column_t DISK_MOUNT_COLUMN = { MG_TABLE_ALIGN_LEFT };
static const mg_table_column_t DISK_CONNECTION_COLUMN = { MG_TABLE_ALIGN_LEFT };
static const mg_table_column_t DISK_MODEL_COLUMN = { MG_TABLE_ALIGN_LEFT };

/* A full storage snapshot is intentionally kept out of the small fixed
 * userspace stack.  IPC itself is paged in two-entry replies. */
static mg_device_info_t disk_snapshot[MG_STORAGE_SNAPSHOT_MAX];
static mg_table_row_t disk_rows[MG_TABLE_MAX_ROWS];

static void format_size(u64 bytes, char *out, usize capacity)
{
    static const u64 units[] = {
        1024ULL * 1024ULL * 1024ULL * 1024ULL,
        1024ULL * 1024ULL * 1024ULL,
        1024ULL * 1024ULL,
        1024ULL,
    };
    static const char *suffixes[] = { "TiB", "GiB", "MiB", "KiB" };

    if (!out || capacity == 0U) return;
    out[0] = '\0';
    for (usize index = 0; index < sizeof(units) / sizeof(units[0]); index++) {
        if (bytes < units[index]) continue;
        u64 whole = bytes / units[index];
        u64 tenth = ((bytes % units[index]) * 10ULL) / units[index];
        if (tenth)
            (void)snprintf(out, capacity, "%llu.%llu %s", whole, tenth,
                           suffixes[index]);
        else
            (void)snprintf(out, capacity, "%llu %s", whole, suffixes[index]);
        return;
    }
    (void)snprintf(out, capacity, "%llu B", bytes);
}

static const char *filesystem_text(const mg_device_info_t *device)
{
    const char *filesystem;

    if (!device || !device->filesystem[0]) return "-";
    filesystem = device->filesystem;
    if (!strcmp(filesystem, "mgfs") || !strcmp(filesystem, "MGFS"))
        return "MGFS";
    if (!strcmp(filesystem, "fat32") || !strcmp(filesystem, "FAT32"))
        return "FAT32";
    if (!strcmp(filesystem, "exfat") || !strcmp(filesystem, "exFAT"))
        return "exFAT";
    return "-";
}

static const char *connection_text(const mg_device_info_t *device)
{
    if (!device || !device->connection[0]) return "-";
    return device->connection;
}

static void disk_name(const mg_device_info_t *device, const char *tree_prefix,
                      char *out, usize capacity)
{
    if (!out || capacity == 0U || !device) return;
    if (device->category == MG_DEVICE_CATEGORY_PARTITION) {
        (void)snprintf(out, capacity, "%sdisk%up%u",
                       tree_prefix ? tree_prefix : "",
                       (u32)device->disk_number,
                       (u32)device->partition_number);
    } else {
        (void)snprintf(out, capacity, "disk%u", (u32)device->disk_number);
    }
}

static bool append_disk(mg_table_t *table, const mg_device_info_t *device,
                        const char *tree_prefix)
{
    mg_table_row_t *row = table_row_begin(table);
    char name[64];
    char size[32];
    const char *type;
    const char *label;
    const char *mount;

    if (!row || !device) return false;
    disk_name(device, tree_prefix, name, sizeof(name));
    format_size(device->size_bytes, size, sizeof(size));
    type = device->category == MG_DEVICE_CATEGORY_PARTITION ? "part" : "disk";
    label = device->label[0] ? device->label : "-";
    mount = ((device->flags & MG_DEVICE_FLAG_MOUNTED) &&
             device->mount_point[0]) ? device->mount_point : "-";
    table_row_column(row, &DISK_NAME_COLUMN, name);
    table_row_column(row, &DISK_SIZE_COLUMN, size);
    table_row_column(row, &DISK_TYPE_COLUMN, type);
    table_row_column(row, &DISK_FILESYSTEM_COLUMN, filesystem_text(device));
    table_row_column(row, &DISK_LABEL_COLUMN, label);
    table_row_column(row, &DISK_MOUNT_COLUMN, mount);
    return true;
}

static bool append_disk_header(mg_table_t *table)
{
    mg_table_row_t *row = table_row_begin(table);

    if (!row) return false;
    table_row_column(row, &DISK_NAME_COLUMN, "NAME");
    table_row_column(row, &DISK_SIZE_COLUMN, "SIZE");
    table_row_column(row, &DISK_TYPE_COLUMN, "TYPE");
    table_row_column(row, &DISK_FILESYSTEM_COLUMN, "FS");
    table_row_column(row, &DISK_LABEL_COLUMN, "LABEL");
    table_row_column(row, &DISK_MOUNT_COLUMN, "MOUNT");
    return true;
}

static bool append_detail_header(mg_table_t *table)
{
    mg_table_row_t *row = table_row_begin(table);

    if (!row) return false;
    table_row_column(row, &DISK_NAME_COLUMN, "NAME");
    table_row_column(row, &DISK_CONNECTION_COLUMN, "CONNECTION");
    table_row_column(row, &DISK_MODEL_COLUMN, "MODEL");
    return true;
}

static bool append_disk_detail(mg_table_t *table,
                               const mg_device_info_t *device)
{
    mg_table_row_t *row = table_row_begin(table);
    char name[32];

    if (!row || !device) return false;
    disk_name(device, NULL, name, sizeof(name));
    table_row_column(row, &DISK_NAME_COLUMN, name);
    table_row_column(row, &DISK_CONNECTION_COLUMN, connection_text(device));
    table_row_column(row, &DISK_MODEL_COLUMN,
                     device->model[0] ? device->model : "-");
    return true;
}

static int find_device(const mg_device_info_t *devices, u32 count, u64 id)
{
    for (u32 index = 0; index < count; index++)
        if (devices[index].id == id) return (int)index;
    return -1;
}

static usize utf8_sequence_length(const char *text, usize length,
                                  usize offset)
{
    u8 first;
    usize sequence = 1U;
    u32 codepoint;

    if (!text || offset >= length) return 0;
    first = (u8)text[offset];
    codepoint = first;
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
        return 1U;
    }
    if (offset + sequence > length) return 1U;
    for (usize index = 1U; index < sequence; index++) {
        u8 byte = (u8)text[offset + index];
        if ((byte & 0xc0U) != 0x80U) return 1U;
        codepoint = (codepoint << 6) | (byte & 0x3fU);
    }
    if ((sequence == 2U && codepoint < 0x80U) ||
        (sequence == 3U && codepoint < 0x800U) ||
        (sequence == 4U && codepoint < 0x10000U) ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
        codepoint > 0x10ffffU) return 1U;
    return sequence;
}

static usize text_prefix_bytes(const char *text, usize max_width)
{
    usize source_length = 0;
    usize offset = 0;
    usize width = 0;

    if (!text) return 0;
    while (source_length + 1U < MG_TABLE_CELL_CAPACITY &&
           text[source_length]) source_length++;
    while (offset < source_length && width < max_width) {
        usize sequence = utf8_sequence_length(text, source_length, offset);
        if (!sequence) break;
        offset += sequence;
        width++;
    }
    return offset;
}

static void truncate_table_cell(mg_table_row_t *row, usize column,
                                usize width)
{
    char truncated[MG_TABLE_CELL_CAPACITY];
    usize prefix_width;
    usize prefix_bytes;
    usize output_length;

    if (!row || column >= row->column_count || width == 0U ||
        row->display_widths[column] <= width) return;
    prefix_width = width >= 3U ? width - 3U : width;
    prefix_bytes = text_prefix_bytes(row->cells[column], prefix_width);
    memcpy(truncated, row->cells[column], prefix_bytes);
    output_length = prefix_bytes;
    if (width >= 3U) {
        memcpy(truncated + output_length, "...", 3U);
        output_length += 3U;
    }
    truncated[output_length] = '\0';
    memcpy(row->cells[column], truncated, output_length + 1U);
    row->lengths[column] = output_length;
    table_text_metrics(row->cells[column], &row->lengths[column],
                       &row->display_widths[column]);
}

static bool fit_default_table_to_terminal(mg_table_t *table)
{
    mg_terminal_size_t size = {0};
    usize widths[MG_TABLE_MAX_COLUMNS] = {0};
    usize column_count = 0;

    if (!table || !table->row_count) return true;
    if (terminal_get_size(&size) != MG_OK || !size.columns) return true;
    if (!table_calculate_widths(table, widths, &column_count)) return false;
    for (usize pass = 0; pass < 2U; pass++) {
        usize total = table_display_width_from_widths(widths, column_count);
        usize column;
        usize excess;
        usize target;

        if (total <= size.columns) return true;
        column = pass == 0U ? LSDISK_LABEL_COLUMN : LSDISK_MOUNT_COLUMN;
        if (column >= column_count || widths[column] <= 1U) continue;
        excess = total - size.columns;
        target = widths[column] > excess ? widths[column] - excess : 1U;
        for (usize row = 0; row < table->row_count; row++)
            truncate_table_cell(&table->rows[row], column, target);
        if (!table_calculate_widths(table, widths, &column_count)) return false;
    }
    return table_display_width_from_widths(widths, column_count) <= size.columns;
}

static bool fit_detail_table_to_terminal(mg_table_t *table)
{
    mg_terminal_size_t size = {0};
    usize widths[MG_TABLE_MAX_COLUMNS] = {0};
    usize column_count = 0;
    usize total;
    usize excess;
    usize target;

    if (!table || !table->row_count) return true;
    if (terminal_get_size(&size) != MG_OK || !size.columns) return true;
    if (!table_calculate_widths(table, widths, &column_count)) return false;
    total = table_display_width_from_widths(widths, column_count);
    if (total <= size.columns) return true;
    if (LSDISK_DETAIL_MODEL_COLUMN >= column_count ||
        widths[LSDISK_DETAIL_MODEL_COLUMN] <= 1U)
        return false;
    excess = total - size.columns;
    target = widths[LSDISK_DETAIL_MODEL_COLUMN] > excess
                ? widths[LSDISK_DETAIL_MODEL_COLUMN] - excess : 1U;
    for (usize row = 0; row < table->row_count; row++)
        truncate_table_cell(&table->rows[row], LSDISK_DETAIL_MODEL_COLUMN,
                            target);
    if (!table_calculate_widths(table, widths, &column_count)) return false;
    return table_display_width_from_widths(widths, column_count) <= size.columns;
}

static bool build_default_table(u32 count, mg_table_t *table)
{
    if (!table || !append_disk_header(table)) return false;
    for (u32 index = 0; index < count; index++) {
        if (disk_snapshot[index].category != MG_DEVICE_CATEGORY_BLOCK)
            continue;
        if (!append_disk(table, &disk_snapshot[index], NULL)) return false;
        u32 child_count = 0;
        for (u32 child = 0; child < count; child++)
            if (disk_snapshot[child].category == MG_DEVICE_CATEGORY_PARTITION &&
                disk_snapshot[child].parent_id == disk_snapshot[index].id)
                child_count++;
        u32 child_seen = 0;
        for (u32 child = 0; child < count; child++) {
            if (disk_snapshot[child].category != MG_DEVICE_CATEGORY_PARTITION ||
                disk_snapshot[child].parent_id != disk_snapshot[index].id)
                continue;
            child_seen++;
            if (!append_disk(table, &disk_snapshot[child],
                             child_seen == child_count ? "└─" : "├─"))
                return false;
        }
    }
    /* Preserve visibility of malformed/orphaned partition metadata without
     * attaching it to an unrelated disk. */
    for (u32 index = 0; index < count; index++) {
        if (disk_snapshot[index].category != MG_DEVICE_CATEGORY_PARTITION ||
            find_device(disk_snapshot, count, disk_snapshot[index].parent_id) >= 0)
            continue;
        if (!append_disk(table, &disk_snapshot[index], NULL)) return false;
    }
    return true;
}

static bool build_detail_table(u32 count, mg_table_t *table)
{
    if (!table || !append_detail_header(table)) return false;
    for (u32 index = 0; index < count; index++) {
        if (disk_snapshot[index].category != MG_DEVICE_CATEGORY_BLOCK)
            continue;
        if (!append_disk_detail(table, &disk_snapshot[index])) return false;
    }
    return true;
}

static int list_disks(bool verbose)
{
    u32 count = 0;
    mg_result_t result = storage_snapshot_read(disk_snapshot,
                                               MG_STORAGE_SNAPSHOT_MAX,
                                               &count);
    mg_table_t table;

    if (result != MG_OK) {
        printf("lsdsk: %s\n", result == MG_ERR_SERVICE_UNAVAILABLE
               ? "device service unavailable" : error_string(result));
        return 1;
    }
    if (!count) {
        printf("No disks.\n");
        return 0;
    }
    table_init(&table, disk_rows, MG_TABLE_MAX_ROWS);
    if (verbose) {
        if (!build_detail_table(count, &table)) {
            printf("lsdsk: table is too large.\n");
            return 1;
        }
    } else if (!build_default_table(count, &table)) {
        printf("lsdsk: table is too large.\n");
        return 1;
    }
    if (verbose && !fit_detail_table_to_terminal(&table)) {
        printf("Terminal too small for lsdsk\n");
        return 1;
    }
    if (!verbose && !fit_default_table_to_terminal(&table)) {
        printf("Terminal too small for lsdsk\n");
        return 1;
    }
    if (!table_render(&table)) {
        printf("lsdsk: invalid table data.\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    bool verbose = false;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "-v") != 0)) {
        command_usage_error(argv[0], "lsdsk [-v]", argc > 1 ? argv[1] : NULL);
        return 1;
    }
    verbose = argc == 2;
    return list_disks(verbose);
}
