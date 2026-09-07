/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <mg/line_editor.h>
#include <mg/storage.h>
#include <stdio.h>
#include <string.h>
#include "../common/help.h"
#include "../common/storage_snapshot.h"
#include "../common/table.h"

#define DISKUTIL_LINE_CAPACITY 256U
#define DISKUTIL_HISTORY_CAPACITY 8U
#define DISKUTIL_MAX_ARGUMENTS 3U
#define DISKUTIL_CONFIRM_CAPACITY 32U

typedef enum {
    DISKUTIL_READ_ONLY,
    DISKUTIL_MUTATING,
} diskutil_command_access_t;

typedef struct {
    const char *name;
    diskutil_command_access_t access;
} diskutil_command_info_t;

typedef struct {
    char *name;
    char *arguments[DISKUTIL_MAX_ARGUMENTS];
    u32 argument_count;
} diskutil_command_t;

typedef struct {
    u64 selected_id;
} diskutil_state_t;

static mg_device_info_t disk_snapshot[MG_STORAGE_SNAPSHOT_MAX];
static mg_table_row_t table_rows[MG_TABLE_MAX_ROWS];

static const mg_table_column_t NAME_COLUMN = { MG_TABLE_ALIGN_LEFT };
static const mg_table_column_t SIZE_COLUMN = { MG_TABLE_ALIGN_RIGHT };
static const mg_table_column_t TEXT_COLUMN = { MG_TABLE_ALIGN_LEFT };

static mg_result_t refresh_snapshot(diskutil_state_t *state, u32 *out_count);
static const mg_device_info_t *selected_disk(const diskutil_state_t *state,
                                             u32 count);

/* Keep read-only inspection separate from mutations in the bounded REPL. All
 * mutations also pass through the authenticated diskutil session and a
 * one-shot kernel operation gate after their confirmation. */
static const diskutil_command_info_t command_info[] = {
    { "list", DISKUTIL_READ_ONLY },
    { "select", DISKUTIL_READ_ONLY },
    { "info", DISKUTIL_READ_ONLY },
    { "format", DISKUTIL_MUTATING },
    { "label", DISKUTIL_MUTATING },
    { "initialize", DISKUTIL_MUTATING },
    { "create", DISKUTIL_MUTATING },
    { "delete", DISKUTIL_MUTATING },
    { "back", DISKUTIL_READ_ONLY },
    { "help", DISKUTIL_READ_ONLY },
    { "exit", DISKUTIL_READ_ONLY },
};

static const diskutil_command_info_t *find_command(const char *name)
{
    for (usize index = 0;
         index < sizeof(command_info) / sizeof(command_info[0]); index++)
        if (!strcmp(command_info[index].name, name)) return &command_info[index];
    return NULL;
}

static void format_size(u64 bytes, char *out, usize capacity)
{
    static const u64 units[] = {
        1024ULL * 1024ULL * 1024ULL * 1024ULL,
        1024ULL * 1024ULL * 1024ULL,
        1024ULL * 1024ULL,
        1024ULL,
    };
    static const char *suffixes[] = { "TiB", "GiB", "MiB", "KiB" };

    if (!out || !capacity) return;
    out[0] = '\0';
    for (usize index = 0; index < sizeof(units) / sizeof(units[0]); index++) {
        if (bytes < units[index]) continue;
        u64 whole = bytes / units[index];
        u64 tenth = ((bytes % units[index]) * 10ULL) / units[index];
        if (tenth)
            (void)snprintf(out, capacity, "%llu.%llu %s", whole, tenth,
                           suffixes[index]);
        else
            (void)snprintf(out, capacity, "%llu %s", whole,
                           suffixes[index]);
        return;
    }
    (void)snprintf(out, capacity, "%llu B", bytes);
}

static const char *filesystem_text(const mg_device_info_t *device)
{
    if (!device || !device->filesystem[0]) return "-";
    if (!strcmp(device->filesystem, "mgfs") ||
        !strcmp(device->filesystem, "MGFS")) return "MGFS";
    if (!strcmp(device->filesystem, "fat32") ||
        !strcmp(device->filesystem, "FAT32")) return "FAT32";
    if (!strcmp(device->filesystem, "exfat") ||
        !strcmp(device->filesystem, "exFAT")) return "exFAT";
    return "-";
}

static const char *mount_text(const mg_device_info_t *device)
{
    if (!device || !(device->flags & MG_DEVICE_FLAG_MOUNTED) ||
        !device->mount_point[0]) return "-";
    return device->mount_point;
}

static void disk_name(const mg_device_info_t *device, char *out,
                      usize capacity)
{
    if (!out || !capacity || !device) return;
    (void)snprintf(out, capacity, "disk%u", (u32)device->disk_number);
}

static void partition_name(const mg_device_info_t *device, const char *prefix,
                           char *out, usize capacity)
{
    if (!out || !capacity || !device) return;
    (void)snprintf(out, capacity, "%spart%u", prefix ? prefix : "",
                   (u32)device->partition_number);
}

static int find_device_by_id(const mg_device_info_t *devices, u32 count, u64 id)
{
    for (u32 index = 0; index < count; index++)
        if (devices[index].id == id) return (int)index;
    return -1;
}

static u32 child_count(const mg_device_info_t *devices, u32 count,
                       u64 parent_id)
{
    u32 result = 0;

    for (u32 index = 0; index < count; index++)
        if (devices[index].category == MG_DEVICE_CATEGORY_PARTITION &&
            devices[index].parent_id == parent_id) result++;
    return result;
}

static bool append_top_header(mg_table_t *table)
{
    mg_table_row_t *row = table_row_begin(table);

    if (!row) return false;
    table_row_column(row, &NAME_COLUMN, "DISK");
    table_row_column(row, &SIZE_COLUMN, "SIZE");
    table_row_column(row, &TEXT_COLUMN, "CONNECTION");
    table_row_column(row, &TEXT_COLUMN, "MODEL");
    return true;
}

static bool append_top_row(mg_table_t *table, const mg_device_info_t *device,
                           const char *prefix, bool partition)
{
    mg_table_row_t *row = table_row_begin(table);
    char name[64];
    char size[32];

    if (!row || !device) return false;
    (void)prefix;
    if (partition) partition_name(device, NULL, name, sizeof(name));
    else disk_name(device, name, sizeof(name));
    format_size(device->size_bytes, size, sizeof(size));
    table_row_column(row, &NAME_COLUMN, name);
    table_row_column(row, &SIZE_COLUMN, size);
    table_row_column(row, &TEXT_COLUMN,
                     device->connection[0] ? device->connection : "-");
    table_row_column(row, &TEXT_COLUMN,
                     device->model[0] ? device->model : "-");
    return true;
}

static bool append_partition_header(mg_table_t *table)
{
    mg_table_row_t *row = table_row_begin(table);

    if (!row) return false;
    table_row_column(row, &NAME_COLUMN, "PART");
    table_row_column(row, &SIZE_COLUMN, "SIZE");
    table_row_column(row, &TEXT_COLUMN, "FS");
    table_row_column(row, &TEXT_COLUMN, "LABEL");
    table_row_column(row, &TEXT_COLUMN, "MOUNT");
    return true;
}

static bool append_partition_row(mg_table_t *table,
                                 const mg_device_info_t *device)
{
    mg_table_row_t *row = table_row_begin(table);
    char name[32];
    char size[32];

    if (!row || !device) return false;
    partition_name(device, NULL, name, sizeof(name));
    format_size(device->size_bytes, size, sizeof(size));
    table_row_column(row, &NAME_COLUMN, name);
    table_row_column(row, &SIZE_COLUMN, size);
    table_row_column(row, &TEXT_COLUMN, filesystem_text(device));
    table_row_column(row, &TEXT_COLUMN,
                     device->label[0] ? device->label : "-");
    table_row_column(row, &TEXT_COLUMN, mount_text(device));
    return true;
}

static void print_snapshot_error(mg_result_t result)
{
    printf("diskutil: storage snapshot unavailable: %s\n",
           error_string(result));
}

static bool render_top_list(const mg_device_info_t *devices, u32 count)
{
    mg_table_t table;

    table_init(&table, table_rows, MG_TABLE_MAX_ROWS);
    if (!append_top_header(&table)) return false;
    for (u32 index = 0; index < count; index++) {
        const mg_device_info_t *disk = &devices[index];
        if (disk->category != MG_DEVICE_CATEGORY_BLOCK) continue;
        if (!append_top_row(&table, disk, NULL, false)) return false;
    }
    return table_render(&table);
}

static bool render_selected_list(const mg_device_info_t *devices, u32 count,
                                 const mg_device_info_t *disk)
{
    u32 children = child_count(devices, count, disk->id);
    mg_table_t table;

    if (!children) {
        printf("No partitions.\n");
        if (disk->filesystem[0]) {
            printf("Whole-disk filesystem: %s\n", filesystem_text(disk));
            printf("Label: %s\n", disk->label[0] ? disk->label : "-");
            printf("Mount: %s\n", mount_text(disk));
        }
        return true;
    }
    table_init(&table, table_rows, MG_TABLE_MAX_ROWS);
    if (!append_partition_header(&table)) return false;
    for (u32 index = 0; index < count; index++)
        if (devices[index].category == MG_DEVICE_CATEGORY_PARTITION &&
            devices[index].parent_id == disk->id &&
            !append_partition_row(&table, &devices[index])) return false;
    return table_render(&table);
}

static void print_info(const mg_device_info_t *devices, u32 count,
                       const mg_device_info_t *disk)
{
    char name[32];
    char size[32];
    u32 children = child_count(devices, count, disk->id);
    mg_storage_gpt_info_t gpt = {0};
    mg_result_t gpt_result = storage_gpt_info(disk->id, &gpt);
    const char *table_name = "-";

    if (gpt_result == MG_OK) {
        if (gpt.table_type == MG_STORAGE_GPT_TABLE_GPT) table_name = "GPT";
        else if (gpt.table_type == MG_STORAGE_GPT_TABLE_MBR) table_name = "MBR";
        else if (gpt.table_type == MG_STORAGE_GPT_TABLE_CORRUPT)
            table_name = "corrupt";
        else if (gpt.table_type == MG_STORAGE_GPT_TABLE_NONE)
            table_name = "none";
    }

    disk_name(disk, name, sizeof(name));
    format_size(disk->size_bytes, size, sizeof(size));
    printf("Disk:            %s\n", name);
    printf("Size:            %s\n", size);
    printf("Connection:      %s\n",
           disk->connection[0] ? disk->connection : "-");
    printf("Model:           %s\n", disk->model[0] ? disk->model : "-");
    printf("Partition table: %s\n", table_name);
    printf("Partitions:      %u\n", gpt_result == MG_OK &&
           gpt.table_type == MG_STORAGE_GPT_TABLE_GPT
               ? gpt.partition_count : children);
    printf("Read-only:       %s\n",
           (disk->flags & MG_DEVICE_FLAG_READ_ONLY) ? "yes" : "no");
    printf("System-managed:  %s\n",
           (disk->flags & MG_DEVICE_FLAG_SYSTEM_MANAGED) ? "yes" : "no");
    printf("Removable:       %s\n",
           (disk->flags & MG_DEVICE_FLAG_REMOVABLE) ? "yes" : "no");
}

static void print_help(bool selected)
{
    printf("Commands:\n");
    printf("  list                 show %s\n",
           selected ? "partitions for the selected disk" :
                      "whole disks available for selection");
    if (!selected) printf("  select <disk>        select a whole disk\n");
    printf("  info                 show %s disk properties\n",
           selected ? "selected" : "general");
    if (selected) {
        printf("  format <target> <fat32|mgfs|exfat>  format a target safely\n");
        printf("  label <target> <label>        change a filesystem label\n");
        printf("  initialize gpt                create a fresh GPT\n");
        printf("  create <size|rest>             create a GPT partition\n");
        printf("  delete <partN>                 delete a GPT partition\n");
    }
    if (selected) printf("  back                 return to the diskutil prompt\n");
    printf("  help                 show this help\n");
    printf("  exit                 leave diskutil\n");
}

static bool parse_command(char *line, diskutil_command_t *command)
{
    char *read;
    char *write;
    u32 token_count = 0;

    if (!line || !command) return false;
    memset(command, 0, sizeof(*command));
    read = line;
    write = line;
    while (*read) {
        char *token;
        bool in_quotes = false;

        while (*read == ' ' || *read == '\t') read++;
        if (!*read) break;
        if (token_count >= DISKUTIL_MAX_ARGUMENTS + 1U)
            return false;
        token = write;
        while (*read) {
            if (*read == '"') {
                in_quotes = !in_quotes;
                read++;
                continue;
            }
            if (!in_quotes && (*read == ' ' || *read == '\t')) break;
            if (in_quotes && *read == '\\' && read[1]) read++;
            *write++ = *read++;
        }
        if (in_quotes) return false;
        if (*read) read++;
        *write++ = '\0';
        if (!token_count) command->name = token;
        else command->arguments[token_count - 1U] = token;
        token_count++;
    }
    command->argument_count = token_count ? token_count - 1U : 0U;
    return true;
}

static bool text_equal_fold(const char *left, const char *right)
{
    if (!left || !right) return false;
    while (*left && *right) {
        char a = *left++;
        char b = *right++;
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return *left == '\0' && *right == '\0';
}

static bool parse_size_bytes(const char *text, u64 *out_bytes)
{
    u64 value = 0;
    u64 multiplier = 1;
    usize length;
    char suffix;

    if (!text || !out_bytes || !(length = strlen(text)) || length > 24U)
        return false;
    suffix = text[length - 1U];
    if (suffix == 'K' || suffix == 'k') multiplier = 1024ULL;
    else if (suffix == 'M' || suffix == 'm') multiplier = 1024ULL * 1024ULL;
    else if (suffix == 'G' || suffix == 'g') multiplier = 1024ULL * 1024ULL * 1024ULL;
    else if (suffix == 'T' || suffix == 't') multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    else return false;
    if (length == 1U) return false;
    for (usize index = 0; index + 1U < length; index++) {
        if (text[index] < '0' || text[index] > '9' ||
            value > ( ~(u64)0 - (u64)(text[index] - '0')) / 10ULL)
            return false;
        value = value * 10ULL + (u64)(text[index] - '0');
    }
    if (!value || value > ~(u64)0 / multiplier) return false;
    *out_bytes = value * multiplier;
    return true;
}

static bool parse_partition_target(const char *text, u32 *out_number)
{
    u32 value = 0;
    usize length;

    if (!text || strncmp(text, "part", 4) != 0 || !(length = strlen(text)) ||
        length > 13U || length == 4U) return false;
    for (usize index = 4; index < length; index++) {
        if (text[index] < '0' || text[index] > '9') return false;
        if (value > 0xffffffffU / 10U) return false;
        value = value * 10U + (u32)(text[index] - '0');
    }
    if (!value) return false;
    if (out_number) *out_number = value;
    return true;
}

static const mg_device_info_t *resolve_target(
    const mg_device_info_t *disk, const mg_device_info_t *devices, u32 count,
    const char *target, bool *out_whole)
{
    u32 partition_number;

    if (!disk || !devices || !target || !out_whole) return NULL;
    if (!strcmp(target, "disk")) {
        *out_whole = true;
        return disk;
    }
    if (!parse_partition_target(target, &partition_number)) return NULL;
    *out_whole = false;
    for (u32 index = 0; index < count; index++) {
        const mg_device_info_t *device = &devices[index];
        if (device->category == MG_DEVICE_CATEGORY_PARTITION &&
            device->parent_id == disk->id &&
            device->partition_number == partition_number) return device;
    }
    return NULL;
}

static void print_storage_error(const char *operation, const char *target,
                                mg_result_t result)
{
    if (result == MG_ERR_PRIVILEGE_REQUIRED)
        printf("diskutil: %s: administrator privileges required\n", operation);
    else if (result == MG_ERR_ACCESS_DENIED)
        printf("diskutil: %s: %s: access denied\n", operation, target);
    else if (result == MG_ERR_BUSY)
        printf("diskutil: %s: %s: target is busy\n", operation, target);
    else if (result == MG_ERR_DEVICE_GONE)
        printf("diskutil: %s: %s: target is no longer present\n",
               operation, target);
    else if (result == MG_ERR_IO || result == MG_ERR_UNSUPPORTED)
        printf("diskutil: %s: %s: unsupported or invalid filesystem\n",
               operation, target);
    else if (result == MG_ERR_BAD_ARGUMENT && !strcmp(operation, "label"))
        printf("diskutil: %s: label cannot be represented by the filesystem\n",
               target);
    else
        printf("diskutil: %s: %s: %s\n", operation, target,
               error_string(result));
}

static bool read_format_confirmation(const char *disk_name_text,
                                     const char *target, const char *filesystem,
                                     const mg_device_info_t *disk,
                                     const mg_device_info_t *device)
{
    char line[DISKUTIL_CONFIRM_CAPACITY];
    char prompt[64];
    mg_line_editor_t editor;
    int written;

    written = snprintf(prompt, sizeof(prompt),
                       "Type FORMAT to continue: ");
    if (written < 0 || (usize)written >= sizeof(prompt)) return false;
    printf("Format %s on %s as %s?\n", target, disk_name_text, filesystem);
    printf("Disk:       %s\n", disk_name_text);
    printf("Target:     %s\n", target);
    printf("Size:       ");
    {
        char size[32];
        format_size(device->size_bytes, size, sizeof(size));
        printf("%s\n", size);
    }
    printf("Connection: %s\n", disk->connection[0] ? disk->connection : "-");
    printf("Model:      %s\n", disk->model[0] ? disk->model : "-");
    printf("Current FS: %s\n", filesystem_text(device));
    printf("Label:      %s\n", device->label[0] ? device->label : "-");
    printf("\nAll data on this target will be lost.\n");
    line_editor_init(&editor, line, sizeof(line), prompt);
    if (line_editor_read_line(&editor) < 0) return false;
    if (strcmp(line, "FORMAT") != 0) {
        printf("Cancelled.\n");
        return false;
    }
    return true;
}

static bool target_preflight(const mg_device_info_t *disk,
                             const mg_device_info_t *target,
                             const char *target_name,
                             bool formatting)
{
    if (!disk || !target || !target_name) return false;
    if ((disk->flags & MG_DEVICE_FLAG_SYSTEM_MANAGED) ||
        (target->flags & MG_DEVICE_FLAG_SYSTEM_MANAGED)) {
        printf("diskutil: %s is a system-managed volume\n", target_name);
        return false;
    }
    if (target->flags & MG_DEVICE_FLAG_MOUNTED) {
        printf("diskutil: %s is mounted at %s\n",
               target_name, mount_text(target));
        printf("unmount it before %s\n", formatting ? "formatting" :
               "changing the label");
        return false;
    }
    if (target->flags & MG_DEVICE_FLAG_READ_ONLY) {
        printf("diskutil: %s is read-only\n", target_name);
        return false;
    }
    return true;
}

static bool disk_gpt_preflight(const mg_device_info_t *disk,
                               const mg_device_info_t *devices, u32 count,
                               const char *operation)
{
    if (!disk || !devices || !operation) return false;
    if (disk->flags & MG_DEVICE_FLAG_SYSTEM_MANAGED) {
        printf("diskutil: %s is system-managed\n", operation);
        return false;
    }
    if (disk->flags & MG_DEVICE_FLAG_READ_ONLY) {
        printf("diskutil: %s is read-only\n", disk->name[0] ? disk->name :
               "selected disk");
        return false;
    }
    if (disk->flags & MG_DEVICE_FLAG_MOUNTED) {
        printf("diskutil: selected disk is mounted\n");
        return false;
    }
    for (u32 index = 0; index < count; index++) {
        const mg_device_info_t *child = &devices[index];
        if (child->category != MG_DEVICE_CATEGORY_PARTITION ||
            child->parent_id != disk->id) continue;
        if (child->flags & MG_DEVICE_FLAG_SYSTEM_MANAGED) {
            printf("diskutil: disk contains a system-managed partition\n");
            return false;
        }
        if (child->flags & MG_DEVICE_FLAG_MOUNTED) {
            char name[32];
            partition_name(child, NULL, name, sizeof(name));
            printf("diskutil: %s is mounted at %s\n", name,
                   mount_text(child));
            printf("unmount it before %s\n", operation);
            return false;
        }
    }
    return true;
}

static bool read_exact_confirmation(const char *prompt, const char *token)
{
    static char line[DISKUTIL_CONFIRM_CAPACITY];
    mg_line_editor_t editor;

    if (!prompt || !token) return false;
    line_editor_init(&editor, line, sizeof(line), prompt);
    if (line_editor_read_line(&editor) < 0) return false;
    if (strcmp(line, token) != 0) {
        printf("Cancelled.\n");
        return false;
    }
    return true;
}

static bool disk_has_children(const mg_device_info_t *devices, u32 count,
                              u64 disk_id)
{
    return child_count(devices, count, disk_id) != 0U;
}

static bool revalidate_operation_target(diskutil_state_t *state,
                                        u64 disk_id, u64 target_id,
                                        bool whole, u32 *out_count,
                                        const mg_device_info_t **out_disk,
                                        const mg_device_info_t **out_target)
{
    int target_index;
    const mg_device_info_t *disk;

    if (!state || !out_count || !out_disk || !out_target) return false;
    if (refresh_snapshot(state, out_count) != MG_OK) return false;
    disk = selected_disk(state, *out_count);
    target_index = find_device_by_id(disk_snapshot, *out_count, target_id);
    if (!disk || disk->id != disk_id || target_index < 0) return false;
    if (whole) {
        if (target_index < 0 ||
            disk_snapshot[target_index].category != MG_DEVICE_CATEGORY_BLOCK)
            return false;
    } else if (disk_snapshot[target_index].category !=
                   MG_DEVICE_CATEGORY_PARTITION ||
               disk_snapshot[target_index].parent_id != disk->id) return false;
    *out_disk = disk;
    *out_target = &disk_snapshot[target_index];
    return true;
}

static void run_format_command(const diskutil_command_t *command,
                               diskutil_state_t *state, u32 count)
{
    const mg_device_info_t *disk;
    const mg_device_info_t *target;
    const mg_device_info_t *current_disk;
    const mg_device_info_t *current_target;
    mg_storage_format_request_t request = {0};
    char disk_text[32];
    char filesystem[MG_STORAGE_FILESYSTEM_MAX];
    bool whole;
    u64 disk_id;
    u64 target_id;
    u32 refreshed_count;
    mg_result_t result;

    if (command->argument_count != 2U) {
        printf("Usage: format <partN|disk> <fat32|mgfs|exfat>\n");
        return;
    }
    disk = selected_disk(state, count);
    if (!disk) {
        printf("diskutil: select a disk before formatting\n");
        return;
    }
    if (text_equal_fold(command->arguments[1], "fat32"))
        strcpy(filesystem, "fat32");
    else if (text_equal_fold(command->arguments[1], "mgfs"))
        strcpy(filesystem, "mgfs");
    else if (text_equal_fold(command->arguments[1], "exfat"))
        strcpy(filesystem, "exfat");
    else {
        printf("diskutil: unsupported filesystem '%s'\n",
               command->arguments[1]);
        return;
    }
    target = resolve_target(disk, disk_snapshot, count,
                            command->arguments[0], &whole);
    if (!target) {
        printf("diskutil: %s: no such target on selected disk\n",
               command->arguments[0]);
        return;
    }
    if (!target_preflight(disk, target, command->arguments[0], true))
        return;
    if (whole && disk_has_children(disk_snapshot, count, disk->id)) {
        printf("diskutil: %s contains partitions\n", command->arguments[0]);
        return;
    }
    disk_name(disk, disk_text, sizeof(disk_text));
    disk_id = disk->id;
    target_id = target->id;
    result = storage_authorize(MG_STORAGE_OP_FORMAT);
    if (result != MG_OK) {
        print_storage_error("format", command->arguments[0], result);
        return;
    }
    if (!read_format_confirmation(disk_text, command->arguments[0], filesystem,
                                  disk, target)) {
        (void)storage_authorize_cancel();
        return;
    }
    if (!revalidate_operation_target(state, disk_id, target_id, whole,
                                     &refreshed_count, &current_disk,
                                     &current_target) ||
        !target_preflight(current_disk, current_target, command->arguments[0],
                          true) ||
        (whole && disk_has_children(disk_snapshot, refreshed_count, disk_id))) {
        (void)storage_authorize_cancel();
        printf("diskutil: target is no longer present or changed\n");
        return;
    }
    request.instance_id = current_target->id;
    request.parent_instance_id = whole ? 0ULL : current_disk->id;
    request.filesystem = text_equal_fold(filesystem, "fat32")
        ? MG_STORAGE_FILESYSTEM_FAT32
        : text_equal_fold(filesystem, "mgfs")
        ? MG_STORAGE_FILESYSTEM_MGFS : MG_STORAGE_FILESYSTEM_EXFAT;
    result = storage_format(&request);
    if (result != MG_OK) {
        print_storage_error("format", command->arguments[0], result);
        return;
    }
    printf("Formatted %s as %s.\n", command->arguments[0], filesystem);
}

static void run_label_command(const diskutil_command_t *command,
                              diskutil_state_t *state, u32 count)
{
    const mg_device_info_t *disk;
    const mg_device_info_t *target;
    const mg_device_info_t *current_disk;
    const mg_device_info_t *current_target;
    mg_storage_label_request_t request = {0};
    char filesystem[MG_STORAGE_FILESYSTEM_MAX];
    bool whole;
    u64 disk_id;
    u64 target_id;
    u32 refreshed_count;
    mg_result_t result;

    if (command->argument_count != 2U) {
        printf("Usage: label <partN|disk> <label>\n");
        return;
    }
    disk = selected_disk(state, count);
    if (!disk) {
        printf("diskutil: select a disk before changing labels\n");
        return;
    }
    target = resolve_target(disk, disk_snapshot, count,
                            command->arguments[0], &whole);
    if (!target) {
        printf("diskutil: %s: no such target on selected disk\n",
               command->arguments[0]);
        return;
    }
    if (!target_preflight(disk, target, command->arguments[0], false))
        return;
    if (!text_equal_fold(target->filesystem, "fat32") &&
        !text_equal_fold(target->filesystem, "mgfs") &&
        !text_equal_fold(target->filesystem, "exfat")) {
        printf("diskutil: %s: unsupported or invalid filesystem\n",
               command->arguments[0]);
        return;
    }
    if (strlen(command->arguments[1]) >= sizeof(request.label)) {
        printf("diskutil: label is too long\n");
        return;
    }
    strcpy(filesystem, text_equal_fold(target->filesystem, "fat32")
                       ? "fat32" : text_equal_fold(target->filesystem, "mgfs")
                       ? "mgfs" : "exfat");
    disk_id = disk->id;
    target_id = target->id;
    result = storage_authorize(MG_STORAGE_OP_LABEL);
    if (result != MG_OK) {
        print_storage_error("label", command->arguments[0], result);
        return;
    }
    if (!revalidate_operation_target(state, disk_id, target_id, whole,
                                     &refreshed_count, &current_disk,
                                     &current_target) ||
        !target_preflight(current_disk, current_target, command->arguments[0],
                          false) ||
        !text_equal_fold(current_target->filesystem, filesystem)) {
        (void)storage_authorize_cancel();
        printf("diskutil: target is no longer present or changed\n");
        return;
    }
    request.instance_id = current_target->id;
    request.parent_instance_id = whole ? 0ULL : current_disk->id;
    strcpy(request.filesystem, filesystem);
    strcpy(request.label, command->arguments[1]);
    result = storage_set_label(&request);
    if (result != MG_OK) {
        print_storage_error("label", command->arguments[0], result);
        return;
    }
    if (request.label[0]) printf("Label set on %s.\n", command->arguments[0]);
    else printf("Label cleared on %s.\n", command->arguments[0]);
}

static void print_gpt_error(const char *operation, const char *target,
                            mg_result_t result)
{
    if (result == MG_ERR_PRIVILEGE_REQUIRED)
        printf("diskutil: %s: administrator privileges required\n", operation);
    else if (result == MG_ERR_ACCESS_DENIED)
        printf("diskutil: %s: %s: access denied\n", operation, target);
    else if (result == MG_ERR_BUSY)
        printf("diskutil: %s: %s: target is busy\n", operation, target);
    else if (result == MG_ERR_DEVICE_GONE)
        printf("diskutil: %s: %s: target is no longer present\n",
               operation, target);
    else if (result == MG_ERR_UNSUPPORTED)
        printf("diskutil: %s: %s: unsupported partition table or geometry\n",
               operation, target);
    else if (result == MG_ERR_IO)
        printf("diskutil: %s: %s: operation failed or no suitable space\n",
               operation, target);
    else
        printf("diskutil: %s: %s: %s\n", operation, target,
               error_string(result));
}

static bool revalidate_selected_disk(diskutil_state_t *state, u64 disk_id,
                                     u32 *out_count,
                                     const mg_device_info_t **out_disk)
{
    const mg_device_info_t *disk;

    if (!state || !out_count || !out_disk ||
        refresh_snapshot(state, out_count) != MG_OK) return false;
    disk = selected_disk(state, *out_count);
    if (!disk || disk->id != disk_id) return false;
    *out_disk = disk;
    return true;
}

static void run_initialize_command(const diskutil_command_t *command,
                                   diskutil_state_t *state, u32 count)
{
    const mg_device_info_t *disk = selected_disk(state, count);
    const mg_device_info_t *current_disk;
    char name[32];
    char size[32];
    u64 disk_id;
    u32 refreshed_count;
    mg_result_t result;

    if (command->argument_count != 1U ||
        !text_equal_fold(command->arguments[0], "gpt")) {
        printf("Usage: initialize gpt\n");
        return;
    }
    if (!disk) {
        printf("diskutil: select a disk before initializing GPT\n");
        return;
    }
    if (!disk_gpt_preflight(disk, disk_snapshot, count, "initialization"))
        return;
    disk_name(disk, name, sizeof(name));
    format_size(disk->size_bytes, size, sizeof(size));
    result = storage_authorize(MG_STORAGE_OP_GPT_INITIALIZE);
    if (result != MG_OK) {
        print_gpt_error("initialize", name, result);
        return;
    }
    printf("Initialize %s with a new GPT partition table?\n", name);
    printf("Disk:        %s\n", name);
    printf("Size:        %s\n", size);
    printf("Connection:  %s\n", disk->connection[0] ? disk->connection : "-");
    printf("Model:       %s\n", disk->model[0] ? disk->model : "-");
    printf("\nAll existing partition-table data on this disk will be lost.\n");
    if (!read_exact_confirmation("Type INITIALIZE to continue: ",
                                "INITIALIZE")) {
        (void)storage_authorize_cancel();
        return;
    }
    disk_id = disk->id;
    if (!revalidate_selected_disk(state, disk_id, &refreshed_count,
                                  &current_disk) ||
        !disk_gpt_preflight(current_disk, disk_snapshot, refreshed_count,
                            "initialization")) {
        (void)storage_authorize_cancel();
        printf("diskutil: target is no longer present or changed\n");
        return;
    }
    result = storage_gpt_initialize(current_disk->id);
    if (result != MG_OK) {
        print_gpt_error("initialize", name, result);
        return;
    }
    printf("Initialized %s with GPT.\n", name);
}

static void run_create_command(const diskutil_command_t *command,
                               diskutil_state_t *state, u32 count)
{
    const mg_device_info_t *disk = selected_disk(state, count);
    const mg_device_info_t *current_disk;
    mg_storage_gpt_info_t info = {0};
    mg_storage_gpt_plan_request_t plan_request = {0};
    mg_storage_gpt_plan_t plan = {0};
    mg_storage_gpt_plan_t current_plan = {0};
    mg_storage_gpt_create_request_t create_request = {0};
    char disk_text[32];
    char size[32];
    char target[32];
    bool use_rest;
    u64 requested_bytes = 0;
    u64 disk_id;
    u32 refreshed_count;
    mg_result_t result;

    if (command->argument_count != 1U) {
        printf("Usage: create <size|rest>\n");
        return;
    }
    if (!disk) {
        printf("diskutil: select a disk before creating a partition\n");
        return;
    }
    use_rest = text_equal_fold(command->arguments[0], "rest");
    if (!use_rest && !parse_size_bytes(command->arguments[0], &requested_bytes)) {
        printf("diskutil: invalid partition size '%s'\n", command->arguments[0]);
        return;
    }
    if (!disk_gpt_preflight(disk, disk_snapshot, count, "partition creation"))
        return;
    result = storage_gpt_info(disk->id, &info);
    if (result != MG_OK) {
        print_gpt_error("create", command->arguments[0], result);
        return;
    }
    if (info.table_type != MG_STORAGE_GPT_TABLE_GPT) {
        if (info.table_type == MG_STORAGE_GPT_TABLE_NONE)
            printf("diskutil: selected disk has no GPT partition table\n"
                   "initialize it first\n");
        else if (info.table_type == MG_STORAGE_GPT_TABLE_MBR)
            printf("diskutil: selected disk has an unsupported MBR partition table\n");
        else
            printf("diskutil: selected disk has an inconsistent GPT\n");
        return;
    }
    plan_request.instance_id = disk->id;
    plan_request.requested_bytes = requested_bytes;
    plan_request.flags = use_rest ? MG_STORAGE_GPT_PLAN_FLAG_REST : 0U;
    result = storage_gpt_plan(&plan_request, &plan);
    if (result != MG_OK) {
        print_gpt_error("create", command->arguments[0], result);
        return;
    }
    disk_name(disk, disk_text, sizeof(disk_text));
    (void)snprintf(target, sizeof(target), "part%u", plan.partition_number);
    format_size(plan.size_bytes, size, sizeof(size));
    result = storage_authorize(MG_STORAGE_OP_GPT_CREATE);
    if (result != MG_OK) {
        print_gpt_error("create", target, result);
        return;
    }
    printf("Create a %s partition on %s?\n", size, disk_text);
    printf("Disk:       %s\n", disk_text);
    printf("Partition:  %s\n", target);
    printf("Size:       %s\n", size);
    printf("Start LBA:  %llu\n", plan.first_lba);
    printf("End LBA:    %llu\n", plan.last_lba);
    printf("\nType CREATE to continue.\n");
    if (!read_exact_confirmation("Confirmation: ", "CREATE")) {
        (void)storage_authorize_cancel();
        return;
    }
    disk_id = disk->id;
    if (!revalidate_selected_disk(state, disk_id, &refreshed_count,
                                  &current_disk) ||
        !disk_gpt_preflight(current_disk, disk_snapshot, refreshed_count,
                            "partition creation")) {
        (void)storage_authorize_cancel();
        printf("diskutil: target is no longer present or changed\n");
        return;
    }
    memset(&plan_request, 0, sizeof(plan_request));
    plan_request.instance_id = current_disk->id;
    plan_request.requested_bytes = requested_bytes;
    plan_request.flags = use_rest ? MG_STORAGE_GPT_PLAN_FLAG_REST : 0U;
    result = storage_gpt_plan(&plan_request, &current_plan);
    if (result != MG_OK || current_plan.partition_number != plan.partition_number ||
        current_plan.first_lba != plan.first_lba ||
        current_plan.last_lba != plan.last_lba) {
        (void)storage_authorize_cancel();
        printf("diskutil: target layout changed; creation cancelled\n");
        return;
    }
    create_request.instance_id = current_disk->id;
    create_request.first_lba = current_plan.first_lba;
    create_request.last_lba = current_plan.last_lba;
    create_request.partition_number = current_plan.partition_number;
    result = storage_gpt_create(&create_request);
    if (result != MG_OK) {
        print_gpt_error("create", target, result);
        return;
    }
    printf("Created %s (%s).\n", target, size);
}

static void run_delete_command(const diskutil_command_t *command,
                               diskutil_state_t *state, u32 count)
{
    const mg_device_info_t *disk = selected_disk(state, count);
    const mg_device_info_t *target;
    const mg_device_info_t *current_disk;
    const mg_device_info_t *current_target;
    mg_storage_gpt_delete_request_t request = {0};
    char disk_text[32];
    char target_text[32];
    char size[32];
    bool whole;
    u64 disk_id;
    u64 target_id;
    u32 partition_number;
    u32 refreshed_count;
    mg_result_t result;

    if (command->argument_count != 1U ||
        !parse_partition_target(command->arguments[0], &partition_number)) {
        printf("Usage: delete <partN>\n");
        return;
    }
    if (!disk) {
        printf("diskutil: select a disk before deleting a partition\n");
        return;
    }
    target = resolve_target(disk, disk_snapshot, count,
                            command->arguments[0], &whole);
    if (!target || whole) {
        printf("diskutil: %s: no such partition on selected disk\n",
               command->arguments[0]);
        return;
    }
    if (!disk_gpt_preflight(disk, disk_snapshot, count, "partition deletion"))
        return;
    disk_name(disk, disk_text, sizeof(disk_text));
    partition_name(target, NULL, target_text, sizeof(target_text));
    format_size(target->size_bytes, size, sizeof(size));
    result = storage_authorize(MG_STORAGE_OP_GPT_DELETE);
    if (result != MG_OK) {
        print_gpt_error("delete", target_text, result);
        return;
    }
    printf("Delete %s from %s?\n", target_text, disk_text);
    printf("Partition:  %s\n", target_text);
    printf("Size:       %s\n", size);
    printf("Filesystem: %s\n", filesystem_text(target));
    printf("Label:      %s\n", target->label[0] ? target->label : "-");
    printf("Mount:      %s\n", mount_text(target));
    printf("\nDeleting the partition will make its filesystem inaccessible.\n");
    if (!read_exact_confirmation("Type DELETE to continue: ", "DELETE")) {
        (void)storage_authorize_cancel();
        return;
    }
    disk_id = disk->id;
    target_id = target->id;
    if (!revalidate_operation_target(state, disk_id, target_id, false,
                                     &refreshed_count, &current_disk,
                                     &current_target) ||
        !disk_gpt_preflight(current_disk, disk_snapshot, refreshed_count,
                            "partition deletion") ||
        current_target->partition_number != partition_number) {
        (void)storage_authorize_cancel();
        printf("diskutil: target is no longer present or changed\n");
        return;
    }
    request.instance_id = current_disk->id;
    request.partition_instance_id = current_target->id;
    request.partition_number = partition_number;
    result = storage_gpt_delete(&request);
    if (result != MG_OK) {
        print_gpt_error("delete", target_text, result);
        return;
    }
    printf("Deleted %s.\n", target_text);
}

static mg_result_t refresh_snapshot(diskutil_state_t *state, u32 *out_count)
{
    mg_result_t result;
    int selected;

    result = storage_snapshot_read(disk_snapshot,
                                   MG_STORAGE_SNAPSHOT_MAX, out_count);
    if (result != MG_OK) return result;
    if (!state || !state->selected_id) return MG_OK;
    selected = find_device_by_id(disk_snapshot, *out_count,
                                 state->selected_id);
    if (selected >= 0 &&
        disk_snapshot[selected].category == MG_DEVICE_CATEGORY_BLOCK)
        return MG_OK;
    printf("diskutil: selected disk is no longer present\n");
    state->selected_id = 0;
    return MG_OK;
}

static const mg_device_info_t *selected_disk(const diskutil_state_t *state,
                                             u32 count)
{
    int index;

    if (!state || !state->selected_id) return NULL;
    index = find_device_by_id(disk_snapshot, count, state->selected_id);
    if (index < 0 || disk_snapshot[index].category != MG_DEVICE_CATEGORY_BLOCK)
        return NULL;
    return &disk_snapshot[index];
}

static bool looks_like_partition(const char *target)
{
    return target && (!strncmp(target, "part", 4) || strchr(target, 'p'));
}

static void make_prompt(const diskutil_state_t *state, u32 count,
                        char *prompt, usize capacity)
{
    const mg_device_info_t *disk = selected_disk(state, count);

    if (!prompt || !capacity) return;
    if (disk)
        (void)snprintf(prompt, capacity, "disk%u> ",
                       (u32)disk->disk_number);
    else
        (void)snprintf(prompt, capacity, "diskutil> ");
}

static bool dispatch_command(const diskutil_command_t *command,
                             diskutil_state_t *state, u32 count)
{
    const diskutil_command_info_t *metadata;
    const mg_device_info_t *disk;

    if (!command || !command->name || !state) return true;
    metadata = find_command(command->name);
    if (!metadata) {
        printf("diskutil: unknown command '%s'\n", command->name);
        return true;
    }
    /* Mutating commands are deliberately kept in the same bounded REPL
     * dispatcher as inspection commands; the kernel performs the actual
     * authenticated operation after diskutil's confirmation. */
    if (metadata->access == DISKUTIL_MUTATING && !state->selected_id) {
        printf("diskutil: select a disk first\n");
        return true;
    }
    if (!strcmp(command->name, "exit")) {
        if (command->argument_count)
            printf("diskutil: exit takes no arguments\n");
        else
            return false;
        return true;
    }
    if (!strcmp(command->name, "help")) {
        if (command->argument_count)
            printf("diskutil: help takes no arguments\n");
        else
            print_help(state->selected_id != 0);
        return true;
    }
    if (!strcmp(command->name, "back")) {
        if (command->argument_count)
            printf("diskutil: back takes no arguments\n");
        else
            state->selected_id = 0;
        return true;
    }
    if (!strcmp(command->name, "select")) {
        if (command->argument_count != 1U) {
            printf("Usage: select <disk>\n");
            return true;
        }
        if (looks_like_partition(command->arguments[0])) {
            printf("diskutil: select requires a whole disk\n");
            return true;
        }
        for (u32 index = 0; index < count; index++) {
            char name[32];

            if (disk_snapshot[index].category != MG_DEVICE_CATEGORY_BLOCK)
                continue;
            disk_name(&disk_snapshot[index], name, sizeof(name));
            if (!strcmp(name, command->arguments[0])) {
                state->selected_id = disk_snapshot[index].id;
                printf("Selected %s.\n", name);
                return true;
            }
        }
        printf("diskutil: %s: no such disk\n", command->arguments[0]);
        return true;
    }
    if (!strcmp(command->name, "list")) {
        if (command->argument_count) {
            printf("diskutil: list takes no arguments\n");
            return true;
        }
        disk = selected_disk(state, count);
        if (disk) {
            if (!render_selected_list(disk_snapshot, count, disk))
                printf("diskutil: unable to render partition list\n");
        } else if (!render_top_list(disk_snapshot, count)) {
            printf("diskutil: unable to render disk list\n");
        }
        return true;
    }
    if (!strcmp(command->name, "info")) {
        if (command->argument_count) {
            printf("diskutil: info takes no arguments\n");
            return true;
        }
        disk = selected_disk(state, count);
        if (!disk) printf("No disk selected.\n");
        else print_info(disk_snapshot, count, disk);
        return true;
    }
    if (!strcmp(command->name, "format")) {
        run_format_command(command, state, count);
        return true;
    }
    if (!strcmp(command->name, "label")) {
        run_label_command(command, state, count);
        return true;
    }
    if (!strcmp(command->name, "initialize")) {
        run_initialize_command(command, state, count);
        return true;
    }
    if (!strcmp(command->name, "create")) {
        run_create_command(command, state, count);
        return true;
    }
    if (!strcmp(command->name, "delete")) {
        run_delete_command(command, state, count);
        return true;
    }
    return true;
}

static int run_repl(void)
{
    static char line[DISKUTIL_LINE_CAPACITY];
    static char history_storage[DISKUTIL_HISTORY_CAPACITY]
                              [DISKUTIL_LINE_CAPACITY];
    char prompt[32];
    mg_line_editor_t editor;
    mg_line_history_t history;
    diskutil_state_t state = {0};

    line_editor_init(&editor, line, sizeof(line), prompt);
    line_editor_history_init(&history, &history_storage[0][0],
                             sizeof(history_storage[0]),
                             DISKUTIL_HISTORY_CAPACITY);
    line_editor_set_history(&editor, &history);
    printf("Mangrove Disk Utility\n");
    printf("Type 'help' for commands.\n");
    for (;;) {
        diskutil_command_t command;
        u32 count = 0;
        mg_result_t result;

        result = refresh_snapshot(&state, &count);
        if (result != MG_OK) {
            print_snapshot_error(result);
            state.selected_id = 0;
        }
        make_prompt(&state, count, prompt, sizeof(prompt));
        line_editor_set_prompt(&editor, prompt);
        result = line_editor_read_line(&editor);
        if (result < 0) return 1;
        if (!parse_command(line, &command)) {
            printf("diskutil: too many arguments (maximum %u)\n",
                   DISKUTIL_MAX_ARGUMENTS);
            continue;
        }
        if (!command.name) continue;
        result = refresh_snapshot(&state, &count);
        if (result != MG_OK) {
            print_snapshot_error(result);
            state.selected_id = 0;
            count = 0;
        }
        if (!dispatch_command(&command, &state, count)) return 0;
    }
}

int main(int argc, char **argv)
{
    mg_result_t result;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 1) {
        command_usage_error(argv[0], "diskutil", argc > 1 ? argv[1] : NULL);
        return 1;
    }
    result = storage_session_authorize();
    if (result != MG_OK) {
        printf("diskutil: storage administration authorization failed: %s\n",
               error_string(result));
        return 1;
    }
    return run_repl();
}
