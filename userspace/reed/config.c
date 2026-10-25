/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"

#include <mangrove.h>
#include <mg/filesystem.h>
#include <mg/identity.h>
#include <mg/object.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/app_directory.h"

static void set_diagnostic(char *output, usize capacity, const char *message)
{
    if (!output || !capacity) return;
    (void)snprintf(output, capacity, "%s", message ? message : "");
}

typedef struct config_span {
    usize begin;
    usize end;
} config_span_t;

static config_span_t trim_span(const char *text, config_span_t span)
{
    while (span.begin < span.end &&
           (text[span.begin] == ' ' || text[span.begin] == '\t' ||
            text[span.begin] == '\r')) span.begin++;
    while (span.end > span.begin &&
           (text[span.end - 1U] == ' ' || text[span.end - 1U] == '\t' ||
            text[span.end - 1U] == '\r')) span.end--;
    return span;
}

static bool span_equals(const char *text, config_span_t span,
                        const char *expected)
{
    usize length = strlen(expected);
    return span.end - span.begin == length &&
        memcmp(text + span.begin, expected, length) == 0;
}

void reed_config_defaults(reed_config_t *config)
{
    if (!config) return;
    config->wrap = true;
    config->tab_width = 4U;
}

static bool parse_tab_width(const char *text, config_span_t span,
                            u32 *out_width)
{
    u32 value = 0;

    if (!text || span.begin == span.end || !out_width) return false;
    for (usize index = span.begin; index < span.end; index++) {
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
        usize line_end = offset;
        usize content_end;
        usize equals = (usize)-1;
        config_span_t content;
        config_span_t key;
        config_span_t value;

        line_number++;
        while (line_end < length && text[line_end] != '\n') {
            if (text[line_end] == '\0')
                return config_error(diagnostic, diagnostic_capacity,
                                    line_number, "line contains a NUL byte.");
            line_end++;
        }
        content_end = line_end;
        for (usize index = offset; index < line_end; index++) {
            if (text[index] == '#' || text[index] == ';' ||
                (text[index] == '/' && index + 1U < line_end &&
                 text[index + 1U] == '/')) {
                content_end = index;
                break;
            }
        }
        content = trim_span(text, (config_span_t){offset, content_end});
        if (content.begin != content.end) {
            for (usize index = content.begin; index < content.end; index++)
                if (text[index] == '=') {
                    equals = index;
                    break;
                }
        }
        if (content.begin != content.end && equals == (usize)-1)
            return config_error(diagnostic, diagnostic_capacity,
                                line_number, "expected key = value.");
        if (equals != (usize)-1) {
            key = trim_span(text, (config_span_t){content.begin, equals});
            value = trim_span(text, (config_span_t){equals + 1U, content.end});
        } else {
            key = value = (config_span_t){0, 0};
        }
        if (equals != (usize)-1 && key.begin == key.end)
            return config_error(diagnostic, diagnostic_capacity,
                                line_number, "unknown setting.");
        if (span_equals(text, key, "wrap")) {
            if (saw_wrap)
                return config_error(diagnostic, diagnostic_capacity,
                                    line_number, "duplicate wrap setting.");
            if (span_equals(text, value, "true")) candidate.wrap = true;
            else if (span_equals(text, value, "false")) candidate.wrap = false;
            else
                return config_error(diagnostic, diagnostic_capacity,
                                    line_number,
                                    "wrap must be true or false.");
            saw_wrap = true;
        } else if (span_equals(text, key, "tab_width")) {
            if (saw_tab_width)
                return config_error(diagnostic, diagnostic_capacity,
                                    line_number,
                                    "duplicate tab_width setting.");
            if (!parse_tab_width(text, value, &candidate.tab_width))
                return config_error(diagnostic, diagnostic_capacity,
                                    line_number,
                                    "tab_width must be from 1 through 16.");
            saw_tab_width = true;
        } else if (key.begin != key.end) {
            return config_error(diagnostic, diagnostic_capacity,
                                line_number, "unknown setting.");
        }
        offset = line_end < length ? line_end + 1U : line_end;
    }
    *config = candidate;
    return true;
}

static bool config_buffer_reserve(char **data, usize *capacity,
                                  usize required)
{
    usize next;
    char *replacement;

    if (!data || !capacity) return false;
    if (*capacity >= required) return true;
    if (required == (usize)-1) return false;
    next = *capacity ? *capacity : 256U;
    while (next < required) {
        if (next > ((usize)-1) / 2U) {
            next = required;
            break;
        }
        next *= 2U;
    }
    if (next >= (usize)-1) return false;
    replacement = (char *)realloc(*data, next + 1U);
    if (!replacement) return false;
    *data = replacement;
    *capacity = next;
    return true;
}

static mg_result_t read_config(const char *path, char **out_data,
                               usize *out_length)
{
    mg_path_info_t info;
    mg_handle_t handle;
    char *data = NULL;
    usize capacity = 0;
    usize used = 0;
    mg_result_t result;

    if (!path || !out_data || !out_length)
        return MG_ERR_BAD_ARGUMENT;
    *out_data = NULL;
    *out_length = 0;
    result = path_info(path, &info);
    if (result != MG_OK) return result;
    if (info.type != MG_PATH_TYPE_FILE || info.size > (u64)((usize)-2))
        return MG_ERR_BAD_ARGUMENT;
    if (info.size && !config_buffer_reserve(&data, &capacity,
                                            (usize)info.size))
        return MG_ERR_NO_MEMORY;
    result = file_open(path, MG_OPEN_READ);
    if (result_is_error(result)) {
        free(data);
        return result;
    }
    handle = (mg_handle_t)result;
    for (;;) {
        if (used == capacity &&
            !config_buffer_reserve(&data, &capacity,
                                   used == (usize)-2 ? (usize)-1 : used + 1U)) {
            (void)handle_close(handle);
            free(data);
            return MG_ERR_NO_MEMORY;
        }
        result = object_read(handle, data + used, capacity - used);
        if (result == MG_ERR_END_OF_FILE || result == 0) break;
        if (result_is_error(result) || (usize)result > capacity - used) {
            (void)handle_close(handle);
            free(data);
            return result_is_error(result) ? result : MG_ERR_IO;
        }
        used += (usize)result;
    }
    data[used] = '\0';
    result = handle_close(handle);
    if (result != MG_OK) {
        free(data);
        return result;
    }
    *out_data = data;
    *out_length = used;
    return MG_OK;
}

mg_result_t reed_config_load(reed_config_t *config, char *diagnostic,
                             usize diagnostic_capacity)
{
    reed_config_t candidate;
    char *path = NULL;
    char *data = NULL;
    usize length = 0;
    usize base_length;
    const usize suffix_length = sizeof("/config") - 1U;
    mg_result_t result;

    if (!config) return MG_ERR_BAD_ARGUMENT;
    reed_config_defaults(config);
    set_diagnostic(diagnostic, diagnostic_capacity, "");
    path = (char *)malloc(MG_PATH_MAX_BYTES + 1U);
    if (!path) {
        set_diagnostic(diagnostic, diagnostic_capacity,
                       "Could not allocate Reed config path; using defaults.");
        return MG_ERR_NO_MEMORY;
    }
    result = app_directory_path(MG_APP_DIRECTORY_CONFIG, "reed", path,
                                MG_PATH_MAX_BYTES + 1U);
    if (result_is_error(result)) {
        set_diagnostic(diagnostic, diagnostic_capacity,
                       "Could not resolve Reed config path; using defaults.");
        free(path);
        return result;
    }
    base_length = strlen(path);
    if (base_length > MG_PATH_MAX_BYTES - suffix_length) {
        free(path);
        set_diagnostic(diagnostic, diagnostic_capacity,
                       "Reed config path is too long; using defaults.");
        return MG_ERR_NAME_TOO_LONG;
    }
    memcpy(path + base_length, "/config", suffix_length + 1U);
    result = read_config(path, &data, &length);
    free(path);
    if (result == MG_ERR_NOT_FOUND) return MG_OK;
    if (result != MG_OK) {
        set_diagnostic(diagnostic, diagnostic_capacity,
                       "Could not read Reed config; using defaults.");
        return result;
    }
    if (!reed_config_parse(data, length, &candidate, diagnostic,
                           diagnostic_capacity)) {
        free(data);
        return MG_ERR_BAD_ARGUMENT;
    }
    *config = candidate;
    free(data);
    return MG_OK;
}
