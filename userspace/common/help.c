#include "help.h"

#include <mangrove.h>
#include <mg/filesystem.h>
#include <mg/object.h>
#include <stdio.h>
#include <string.h>

#define HELP_FILE_CAPACITY 4096U
#define HELP_INDEX_PATH "/share/help/.index"
#define HELP_INDEX_CAPACITY 16384U
#define HELP_INDEX_MAX_RECORDS 64U

typedef struct {
    mg_help_record_t records[HELP_INDEX_MAX_RECORDS];
    usize count;
    u64 directory_size;
    bool ready;
    bool loading;
} help_index_cache_t;

static help_index_cache_t help_index_cache;
static char help_index_data[HELP_INDEX_CAPACITY];

static bool help_name_valid(const char *name)
{
    usize length;

    if (!name || !name[0]) return false;
    length = strlen(name);
    if (length >= MG_HELP_NAME_CAPACITY) return false;
    for (usize index = 0; index < length; index++) {
        char value = name[index];
        if (!((value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '_' ||
              value == '-')) return false;
    }
    return true;
}

static bool copy_field(char *destination, usize capacity, const char *value)
{
    usize length;

    if (!destination || !capacity || !value) return false;
    length = strlen(value);
    if (!length || length >= capacity) return false;
    memcpy(destination, value, length + 1U);
    return true;
}

static char *trim(char *text)
{
    char *end;

    while (*text == ' ' || *text == '\t' || *text == '\r') text++;
    end = text + strlen(text);
    while (end != text && (end[-1] == ' ' || end[-1] == '\t' ||
                           end[-1] == '\r')) *--end = '\0';
    return text;
}

static bool parse_record(char *text, const char *requested,
                         mg_help_record_t *record)
{
    char *line = text;
    bool have_name = false;
    bool have_category = false;
    bool have_description = false;

    memset(record, 0, sizeof(*record));
    while (line && *line) {
        char *next = strchr(line, '\n');
        char *separator;
        char *key;
        char *value;

        if (next) *next++ = '\0';
        line = trim(line);
        if (*line && strncmp(line, "//", 2) != 0) {
            separator = strchr(line, '=');
            if (!separator) return false;
            *separator = '\0';
            key = trim(line);
            value = trim(separator + 1);
            if (!*key || !*value) return false;
            if (!strcmp(key, "name")) {
                if (have_name || !copy_field(record->name,
                                             sizeof(record->name), value))
                    return false;
                have_name = true;
            } else if (!strcmp(key, "category")) {
                if (have_category || !copy_field(record->category,
                                                 sizeof(record->category), value))
                    return false;
                have_category = true;
            } else if (!strcmp(key, "description")) {
                if (have_description || !copy_field(record->description,
                                                    sizeof(record->description),
                                                    value)) return false;
                have_description = true;
            } else if (!strcmp(key, "usage")) {
                if (record->usage_count == MG_HELP_MAX_USAGES ||
                    !copy_field(record->usages[record->usage_count],
                                sizeof(record->usages[record->usage_count]),
                                value)) return false;
                record->usage_count++;
            } else if (!strcmp(key, "option")) {
                if (record->option_count == MG_HELP_MAX_OPTIONS ||
                    !copy_field(record->options[record->option_count],
                                sizeof(record->options[record->option_count]),
                                value)) return false;
                record->option_count++;
            } else {
                return false;
            }
        }
        line = next;
    }
    return have_name && have_category && have_description &&
           record->usage_count != 0 && help_name_valid(record->name) &&
           !strcmp(record->name, requested) && help_name_valid(record->category);
}

static bool parse_index_record(char *text, mg_help_record_t *record)
{
    char *line = text;
    bool have_name = false;
    bool have_category = false;
    bool have_description = false;

    memset(record, 0, sizeof(*record));
    while (line && *line) {
        char *next = strchr(line, '\n');
        char *separator;
        char *key;
        char *value;

        if (next) *next++ = '\0';
        line = trim(line);
        if (*line && strncmp(line, "//", 2) != 0) {
            separator = strchr(line, '=');
            if (!separator) return false;
            *separator = '\0';
            key = trim(line);
            value = trim(separator + 1);
            if (!*key || !*value) return false;
            if (!strcmp(key, "name")) {
                if (have_name || !copy_field(record->name,
                                             sizeof(record->name), value))
                    return false;
                have_name = true;
            } else if (!strcmp(key, "category")) {
                if (have_category || !copy_field(record->category,
                                                 sizeof(record->category), value))
                    return false;
                have_category = true;
            } else if (!strcmp(key, "description")) {
                if (have_description || !copy_field(record->description,
                                                    sizeof(record->description),
                                                    value)) return false;
                have_description = true;
            } else {
                return false;
            }
        }
        line = next;
    }
    return have_name && have_category && have_description &&
           help_name_valid(record->name) && help_name_valid(record->category);
}

static mg_result_t read_file_text(const char *path, char *data, usize capacity,
                                  usize *out_used)
{
    mg_handle_t handle;
    usize used = 0;
    mg_result_t result;

    if (!path || !data || capacity < 2U || !out_used) return MG_ERR_BAD_ARGUMENT;
    result = file_open(path, MG_OPEN_READ);
    if (result < 0) return result;
    handle = (mg_handle_t)result;
    for (;;) {
        usize room = capacity - 1U - used;
        if (!room) {
            (void)handle_close(handle);
            return MG_ERR_BAD_ARGUMENT;
        }
        result = object_read(handle, data + used, room);
        if (result == MG_ERR_END_OF_FILE || result == 0) break;
        if (result < 0 || (usize)result > room) {
            (void)handle_close(handle);
            return result < 0 ? result : MG_ERR_IO;
        }
        used += (usize)result;
    }
    (void)handle_close(handle);
    data[used] = '\0';
    *out_used = used;
    return MG_OK;
}

mg_result_t help_load_record(const char *name, mg_help_record_t *record)
{
    char path[128];
    char data[HELP_FILE_CAPACITY];
    usize used = 0;
    mg_result_t result;

    if (!help_name_valid(name) || !record) return MG_ERR_BAD_ARGUMENT;
    if (strlen(name) + 12U >= sizeof(path)) return MG_ERR_BAD_ARGUMENT;
    strcpy(path, "/share/help/");
    strcpy(path + 12, name);
    result = read_file_text(path, data, sizeof(data), &used);
    if (result != MG_OK) return result;
    return parse_record(data, name, record) ? MG_OK : MG_ERR_BAD_ARGUMENT;
}

void help_print_record(const mg_help_record_t *record)
{
    if (!record) return;
    printf("%s - %s\n\n", record->name, record->description);
    printf("Usage:\n");
    for (usize index = 0; index < record->usage_count; index++)
        printf("  %s\n", record->usages[index]);
    if (record->option_count) {
        printf("\nOptions:\n");
        for (usize index = 0; index < record->option_count; index++)
            printf("  %s\n", record->options[index]);
    }
}

static bool category_insert(mg_help_category_t *categories, usize *count,
                            usize capacity, const char *name)
{
    for (usize index = 0; index < *count; index++)
        if (!strcmp(categories[index].name, name)) {
            categories[index].command_count++;
            return true;
        }
    if (*count == capacity || !copy_field(categories[*count].name,
                                           sizeof(categories[*count].name), name))
        return false;
    categories[*count].command_count = 1;
    (*count)++;
    return true;
}

static void sort_records(mg_help_record_t *records, usize count)
{
    for (usize index = 1; index < count; index++) {
        mg_help_record_t value = records[index];
        usize position = index;
        while (position && strcmp(records[position - 1].name, value.name) > 0) {
            records[position] = records[position - 1];
            position--;
        }
        records[position] = value;
    }
}

static bool parse_decimal(const char *value, u64 *out)
{
    u64 result = 0;

    if (!value || !*value || !out) return false;
    for (const char *cursor = value; *cursor; cursor++) {
        if (*cursor < '0' || *cursor > '9' || result >
            (~(u64)0 - (u64)(*cursor - '0')) / 10U) return false;
        result = result * 10U + (u64)(*cursor - '0');
    }
    *out = result;
    return true;
}

static bool index_matches_directory(void)
{
    mg_path_info_t info;

    return path_info("/share/help", &info) == MG_OK &&
           info.type == MG_PATH_TYPE_DIRECTORY &&
           info.size == help_index_cache.directory_size;
}

static char *find_blank_line(char *text)
{
    if (!text) return NULL;
    for (char *cursor = text; cursor[0]; cursor++)
        if (cursor[0] == '\n' && cursor[1] == '\n') return cursor;
    return NULL;
}

static bool parse_index(char *data, usize length)
{
    char *cursor = data;
    char *end = data + length;
    usize count = 0;
    bool have_version = false;
    bool have_directory_size = false;

    while (cursor < end) {
        char *record_start;
        char *record_end;
        char saved = '\0';
        bool terminated;

        while (cursor < end && (*cursor == '\n' || *cursor == '\r' ||
                                *cursor == ' ' || *cursor == '\t')) cursor++;
        if (cursor >= end) break;
        if (strncmp(cursor, "version=1", 9) == 0 &&
            (cursor + 9 == end || cursor[9] == '\n' || cursor[9] == '\r')) {
            have_version = true;
            cursor += 9;
            continue;
        }
        if (strncmp(cursor, "directory_size=", 15) == 0) {
            char *value = cursor + 15;
            char *line_end = strchr(value, '\n');
            char saved = '\0';
            u64 size;

            if (!line_end) line_end = end;
            if (line_end < end) {
                saved = *line_end;
                *line_end = '\0';
            }
            if (!parse_decimal(value, &size)) {
                if (line_end < end) *line_end = saved;
                return false;
            }
            help_index_cache.directory_size = size;
            have_directory_size = true;
            if (line_end < end) *line_end = saved;
            cursor = line_end < end ? line_end + 1 : end;
            continue;
        }
        record_start = cursor;
        record_end = find_blank_line(cursor);
        if (!record_end) record_end = end;
        if (record_end > end || count == HELP_INDEX_MAX_RECORDS) return false;
        terminated = record_end < end;
        if (terminated) {
            saved = *record_end;
            *record_end = '\0';
        }
        if (!parse_index_record(record_start,
                                &help_index_cache.records[count])) {
            if (terminated) *record_end = saved;
            return false;
        }
        if (terminated) *record_end = saved;
        count++;
        cursor = terminated ? record_end + 2 : end;
        while (cursor < end && (*cursor == '\n' || *cursor == '\r')) cursor++;
    }
    if (!have_version || !have_directory_size || !count) return false;
    help_index_cache.count = count;
    return index_matches_directory();
}

static bool load_index_cache(void)
{
    usize used = 0;
    mg_result_t result;

    if (help_index_cache.ready) return true;
    if (help_index_cache.loading) return false;
    help_index_cache.loading = true;
    result = read_file_text(HELP_INDEX_PATH, help_index_data,
                            sizeof(help_index_data), &used);
    if (result == MG_OK && parse_index(help_index_data, used)) {
        help_index_cache.ready = true;
        help_index_cache.loading = false;
        return true;
    }
    help_index_cache.count = 0;
    help_index_cache.loading = false;
    return false;
}

static mg_result_t load_all_records(void)
{
    mg_handle_t directory;
    mg_directory_entry_t entries[MG_DIRECTORY_BATCH_MAX];
    usize entry_count;
    mg_result_t result;

    directory = (mg_handle_t)directory_open("/share/help");
    if ((mg_result_t)directory < 0) return (mg_result_t)directory;
    help_index_cache.count = 0;
    for (;;) {
        mg_help_record_t record;

        result = directory_read_batch(directory, entries,
                                      MG_DIRECTORY_BATCH_MAX, &entry_count);
        if (result == MG_ERR_END_OF_FILE) break;
        if (result < 0) {
            (void)handle_close(directory);
            return result;
        }
        for (usize i = 0; i < entry_count; i++) {
            mg_directory_entry_t *entry = &entries[i];
            if (entry->type != MG_PATH_TYPE_FILE ||
                !help_name_valid(entry->name)) continue;
            if (help_index_cache.count == HELP_INDEX_MAX_RECORDS) {
                (void)handle_close(directory);
                return MG_ERR_BUFFER_TOO_SMALL;
            }
            result = help_load_record(entry->name, &record);
            if (result != MG_OK) continue;
            help_index_cache.records[help_index_cache.count++] = record;
        }
    }
    (void)handle_close(directory);
    sort_records(help_index_cache.records, help_index_cache.count);
    help_index_cache.ready = true;
    return MG_OK;
}

static mg_result_t ensure_help_cache(void)
{
    if (load_index_cache()) return MG_OK;
    if (help_index_cache.ready) return MG_OK;
    return load_all_records();
}

mg_result_t help_list_category(const char *category,
                               mg_help_record_t *records,
                               usize capacity, usize *out_count)
{
    usize count = 0;
    mg_result_t result;

    if (!help_name_valid(category) || !out_count ||
        (capacity && !records)) return MG_ERR_BAD_ARGUMENT;
    result = ensure_help_cache();
    if (result != MG_OK) return result;
    for (usize index = 0; index < help_index_cache.count; index++) {
        mg_help_record_t record = help_index_cache.records[index];

        if (strcmp(record.category, category) != 0) continue;
        if (count == capacity) {
            return MG_ERR_BUFFER_TOO_SMALL;
        }
        records[count++] = record;
    }
    sort_records(records, count);
    *out_count = count;
    return MG_OK;
}

mg_result_t help_list_categories(mg_help_category_t *categories,
                                 usize capacity, usize *out_count)
{
    usize count = 0;
    mg_result_t result;

    if (!out_count || (capacity && !categories)) return MG_ERR_BAD_ARGUMENT;
    result = ensure_help_cache();
    if (result != MG_OK) return result;
    for (usize index = 0; index < help_index_cache.count; index++)
        if (!category_insert(categories, &count, capacity,
                             help_index_cache.records[index].category))
            return MG_ERR_BUFFER_TOO_SMALL;
    for (usize index = 1; index < count; index++) {
        mg_help_category_t value = categories[index];
        usize position = index;
        while (position && strcmp(categories[position - 1].name, value.name) > 0) {
            categories[position] = categories[position - 1];
            position--;
        }
        categories[position] = value;
    }
    *out_count = count;
    return MG_OK;
}

static const char *command_name(const char *argv0)
{
    const char *name;
    const char *cursor;

    if (!argv0) return "command";
    name = argv0;
    for (cursor = argv0; *cursor; cursor++)
        if (*cursor == '/') name = cursor + 1;
    return name;
}

bool command_help_requested(int argc, char **argv)
{
    return argc == 2 && argv && argv[1] &&
           (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"));
}

int command_print_help(const char *argv0)
{
    mg_help_record_t record;
    const char *name = command_name(argv0);
    mg_result_t result = help_load_record(name, &record);

    if (result != MG_OK) {
        printf("Help unavailable for %s.\n", name);
        return 1;
    }
    help_print_record(&record);
    return 0;
}

void command_usage_error(const char *argv0, const char *usage,
                         const char *option)
{
    const char *name = command_name(argv0);

    if (option) printf("Unknown option: %s\n", option);
    else printf("Invalid arguments.\n");
    printf("Usage: %s\nTry '%s --help' for more information.\n",
           usage, name);
}
