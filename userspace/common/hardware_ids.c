/* SPDX-License-Identifier: GPL-3.0-only */
#include "hardware_ids.h"

#include <mangrove.h>
#include <mg/object.h>
#include <string.h>

#define HARDWARE_LINE_MAX 256U
#define HARDWARE_READ_MAX 4096U

static bool hex_digit(char value, u8 *out)
{
    if (value >= '0' && value <= '9') *out = (u8)(value - '0');
    else if (value >= 'a' && value <= 'f') *out = (u8)(value - 'a' + 10);
    else if (value >= 'A' && value <= 'F') *out = (u8)(value - 'A' + 10);
    else return false;
    return true;
}

static bool parse_hex4(const char *text, u16 *out)
{
    u16 value = 0;
    u8 digit;

    if (!text || !out) return false;
    for (usize index = 0; index < 4; index++) {
        if (!hex_digit(text[index], &digit)) return false;
        value = (u16)((value << 4) | digit);
    }
    *out = value;
    return true;
}

static const char *skip_space(const char *text)
{
    while (text && (*text == ' ' || *text == '\t' || *text == '\r')) text++;
    return text;
}

static bool copy_name(char *out, const char *start)
{
    const char *end;
    usize length;

    if (!out || !start) return false;
    start = skip_space(start);
    end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' ||
                           end[-1] == '\r')) end--;
    length = (usize)(end - start);
    if (!length || length >= MG_HARDWARE_ID_NAME_MAX) return false;
    memcpy(out, start, length);
    out[length] = '\0';
    return true;
}

static mg_hardware_id_match_t *find_entry(mg_hardware_ids_t *ids,
                                          u16 vendor_id, u16 device_id)
{
    if (!ids) return NULL;
    for (usize index = 0; index < ids->count; index++)
        if (ids->entries[index].vendor_id == vendor_id &&
            ids->entries[index].device_id == device_id)
            return &ids->entries[index];
    return NULL;
}

void hardware_ids_init(mg_hardware_ids_t *ids)
{
    if (ids) memset(ids, 0, sizeof(*ids));
}

bool hardware_ids_add(mg_hardware_ids_t *ids, u16 vendor_id, u16 device_id)
{
    if (!ids) return false;
    if (find_entry(ids, vendor_id, device_id)) return true;
    if (ids->count >= MG_HARDWARE_ID_MAX_MATCHES) return false;
    ids->entries[ids->count].vendor_id = vendor_id;
    ids->entries[ids->count].device_id = device_id;
    ids->count++;
    return true;
}

static void process_line(mg_hardware_ids_t *ids, char *line,
                         u16 *current_vendor, bool *have_vendor)
{
    usize tabs = 0;
    u16 value;
    const char *name;

    if (!ids || !line || !current_vendor || !have_vendor) return;
    if (line[0] == '#') return;
    while (line[tabs] == '\t') tabs++;
    if (tabs == 0) {
        if (strlen(line) >= 5U && parse_hex4(line, &value) &&
            (line[4] == ' ' || line[4] == '\t') &&
            (name = skip_space(line + 4), *name)) {
            *current_vendor = value;
            *have_vendor = true;
            for (usize index = 0; index < ids->count; index++) {
                mg_hardware_id_match_t *entry = &ids->entries[index];
                if (entry->vendor_id == value) {
                    (void)copy_name(entry->vendor, name);
                    entry->vendor_found = true;
                }
            }
        }
        return;
    }
    if (tabs != 1 || !*have_vendor || strlen(line) < 6U ||
        !parse_hex4(line + 1, &value) ||
        (line[5] != ' ' && line[5] != '\t')) return;
    name = skip_space(line + 5);
    if (!*name) return;
    {
        mg_hardware_id_match_t *entry = find_entry(ids, *current_vendor, value);
        if (entry && copy_name(entry->device, name)) entry->device_found = true;
    }
}

mg_result_t hardware_ids_load(const char *path, mg_hardware_ids_t *ids)
{
    mg_handle_t file;
    u8 input[HARDWARE_READ_MAX];
    char line[HARDWARE_LINE_MAX];
    usize line_length = 0;
    bool line_too_long = false;
    u16 current_vendor = 0;
    bool have_vendor = false;
    mg_result_t result;

    if (!path || !ids) return MG_ERR_BAD_ARGUMENT;
    result = file_open(path, MG_OPEN_READ);
    if (result < 0) return result;
    file = (mg_handle_t)result;
    for (;;) {
        result = object_read(file, input, sizeof(input));
        if (result == MG_ERR_END_OF_FILE || result == 0) break;
        if (result < 0 || (usize)result > sizeof(input)) {
            (void)handle_close(file);
            return result < 0 ? result : MG_ERR_IO;
        }
        for (usize index = 0; index < (usize)result; index++) {
            char value = (char)input[index];
            if (value == '\n') {
                if (!line_too_long) {
                    line[line_length] = '\0';
                    process_line(ids, line, &current_vendor, &have_vendor);
                }
                line_length = 0;
                line_too_long = false;
            } else if (!line_too_long && line_length + 1U < sizeof(line)) {
                line[line_length++] = value;
            } else {
                line_too_long = true;
            }
        }
    }
    if (line_length && !line_too_long) {
        line[line_length] = '\0';
        process_line(ids, line, &current_vendor, &have_vendor);
    }
    (void)handle_close(file);
    return MG_OK;
}

const mg_hardware_id_match_t *hardware_ids_find(const mg_hardware_ids_t *ids,
                                                u16 vendor_id, u16 device_id)
{
    if (!ids) return NULL;
    for (usize index = 0; index < ids->count; index++)
        if (ids->entries[index].vendor_id == vendor_id &&
            ids->entries[index].device_id == device_id)
            return &ids->entries[index];
    return NULL;
}
