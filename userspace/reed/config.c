/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"

#include <mangrove.h>
#include <mg/filesystem.h>
#include <mg/identity.h>
#include <mg/object.h>
#include <stdio.h>
#include <string.h>

static void set_diagnostic(char *output, usize capacity, const char *message)
{
    if (!output || !capacity) return;
    (void)snprintf(output, capacity, "%s", message ? message : "");
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

void reed_config_defaults(reed_config_t *config)
{
    if (!config) return;
    config->wrap = true;
    config->tab_width = 4U;
}

static bool parse_tab_width(const char *text, u32 *out_width)
{
    u32 value = 0;

    if (!text || !*text || !out_width) return false;
    for (usize index = 0; text[index]; index++) {
        u32 digit;

        if (text[index] < '0' || text[index] > '9') return false;
        digit = (u32)(text[index] - '0');
        if (value > (REED_CONFIG_TAB_WIDTH_MAX - digit) / 10U)
            return false;
        value = value * 10U + digit;
    }
    if (value < 1U || value > REED_CONFIG_TAB_WIDTH_MAX) return false;
    *out_width = value;
    return true;
}

static bool config_error(char *diagnostic, usize capacity, usize line,
                         const char *reason)
{
    if (diagnostic && capacity)
        (void)snprintf(diagnostic, capacity,
                       "Invalid Reed config at line %llu: %s",
                       (unsigned long long)line, reason);
    return false;
}

bool reed_config_parse(const char *text, usize length,
                       reed_config_t *config, char *diagnostic,
                       usize diagnostic_capacity)
{
    reed_config_t candidate;
    bool saw_wrap = false;
    bool saw_tab_width = false;
    usize offset = 0;
    usize line_number = 0;

    if (!text || !config) {
        set_diagnostic(diagnostic, diagnostic_capacity,
                       "Invalid Reed config input.");
        return false;
    }
    reed_config_defaults(&candidate);
    if (diagnostic && diagnostic_capacity) diagnostic[0] = '\0';
    while (offset < length) {
        char line[128];
        usize count = 0;
        char *content;
        char *equals;

        line_number++;
        while (offset < length && text[offset] != '\n') {
            if (text[offset] == '\0')
                return config_error(diagnostic, diagnostic_capacity,
                                    line_number, "line contains a NUL byte.");
            if (count + 1U >= sizeof(line))
                return config_error(diagnostic, diagnostic_capacity,
                                    line_number, "line is too long.");
            line[count++] = text[offset++];
        }
        if (offset < length && text[offset] == '\n') offset++;
        line[count] = '\0';
        for (usize index = 0; index < count; index++) {
            if (line[index] == '#' || line[index] == ';' ||
                (line[index] == '/' && line[index + 1U] == '/')) {
                line[index] = '\0';
                break;
            }
        }
        content = trim(line);
        if (!*content) continue;
        equals = strchr(content, '=');
        if (!equals)
            return config_error(diagnostic, diagnostic_capacity,
                                line_number, "expected key = value.");
        *equals++ = '\0';
        content = trim(content);
        equals = trim(equals);
        if (!strcmp(content, "wrap")) {
            if (saw_wrap)
                return config_error(diagnostic, diagnostic_capacity,
                                    line_number, "duplicate wrap setting.");
            if (!strcmp(equals, "true")) candidate.wrap = true;
            else if (!strcmp(equals, "false")) candidate.wrap = false;
            else
                return config_error(diagnostic, diagnostic_capacity,
                                    line_number,
                                    "wrap must be true or false.");
            saw_wrap = true;
        } else if (!strcmp(content, "tab_width")) {
            if (saw_tab_width)
                return config_error(diagnostic, diagnostic_capacity,
                                    line_number,
                                    "duplicate tab_width setting.");
            if (!parse_tab_width(equals, &candidate.tab_width))
                return config_error(diagnostic, diagnostic_capacity,
                                    line_number,
                                    "tab_width must be from 1 through 16.");
            saw_tab_width = true;
        } else {
            return config_error(diagnostic, diagnostic_capacity,
                                line_number, "unknown setting.");
        }
    }
    *config = candidate;
    return true;
}

bool reed_config_path_for_home(const char *home, char *path,
                               usize path_capacity)
{
    static const char suffix[] = "/.config/reed/config";
    const char *tail = suffix;
    usize home_length;
    usize suffix_length;

    if (!home || home[0] != '/' || !path || !path_capacity) return false;
    home_length = strlen(home);
    if (home_length > 1U && home[home_length - 1U] == '/') home_length--;
    if (home_length == 1U && home[0] == '/') tail++;
    suffix_length = strlen(tail);
    if (home_length > path_capacity ||
        suffix_length >= path_capacity - home_length ||
        home_length > MG_PATH_MAX_BYTES ||
        suffix_length > MG_PATH_MAX_BYTES - home_length) return false;
    memcpy(path, home, home_length);
    memcpy(path + home_length, tail, suffix_length + 1U);
    return true;
}

static mg_result_t read_config(const char *path, char *data,
                               usize capacity, usize *out_length)
{
    mg_path_info_t info;
    mg_handle_t handle;
    usize used = 0;
    mg_result_t result;

    if (!path || !data || !capacity || !out_length)
        return MG_ERR_BAD_ARGUMENT;
    result = path_info(path, &info);
    if (result != MG_OK) return result;
    if (info.type != MG_PATH_TYPE_FILE ||
        info.size >= REED_CONFIG_FILE_CAPACITY)
        return MG_ERR_BAD_ARGUMENT;
    result = file_open(path, MG_OPEN_READ);
    if (result_is_error(result)) return result;
    handle = (mg_handle_t)result;
    while (used < capacity) {
        result = object_read(handle, data + used, capacity - used);
        if (result == MG_ERR_END_OF_FILE || result == 0) break;
        if (result_is_error(result) || (usize)result > capacity - used) {
            (void)handle_close(handle);
            return result_is_error(result) ? result : MG_ERR_IO;
        }
        used += (usize)result;
    }
    result = handle_close(handle);
    if (result != MG_OK) return result;
    *out_length = used;
    return MG_OK;
}

mg_result_t reed_config_load(reed_config_t *config, char *diagnostic,
                             usize diagnostic_capacity)
{
    mg_identity_t identity;
    reed_config_t candidate;
    char path[REED_CONFIG_PATH_CAPACITY];
    char data[REED_CONFIG_FILE_CAPACITY + 1U];
    usize length;
    mg_result_t result;

    if (!config) return MG_ERR_BAD_ARGUMENT;
    reed_config_defaults(config);
    set_diagnostic(diagnostic, diagnostic_capacity, "");
    result = process_get_identity(&identity);
    if (result_is_error(result) || identity.uid == MG_UID_SYSTEM ||
        !reed_config_path_for_home(identity.home, path, sizeof(path))) {
        set_diagnostic(diagnostic, diagnostic_capacity,
                       "Could not resolve Reed config path; using defaults.");
        return result_is_error(result) ? result : MG_ERR_BAD_ARGUMENT;
    }
    result = read_config(path, data, sizeof(data), &length);
    if (result == MG_ERR_NOT_FOUND) return MG_OK;
    if (result != MG_OK) {
        set_diagnostic(diagnostic, diagnostic_capacity,
                       "Could not read Reed config; using defaults.");
        return result;
    }
    if (length >= REED_CONFIG_FILE_CAPACITY) {
        set_diagnostic(diagnostic, diagnostic_capacity,
                       "Reed config is too large; using defaults.");
        return MG_ERR_BAD_ARGUMENT;
    }
    if (!reed_config_parse(data, length, &candidate, diagnostic,
                           diagnostic_capacity))
        return MG_ERR_BAD_ARGUMENT;
    *config = candidate;
    return MG_OK;
}
