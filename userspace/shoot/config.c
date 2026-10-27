/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"

#include <mangrove.h>
#include <mg/filesystem.h>
#include <mg/object.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/app_directory.h"
#include "../common/path.h"

#define SHOOT_CONFIG_DIRECTORY_CAPACITY 320U

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

static bool config_color(const char *text, config_span_t name,
                         mg_terminal_color_t *out_color)
{
    static const char *const names[MG_TERMINAL_COLOR_COUNT] = {
        "black", "red", "green", "yellow", "blue", "magenta", "cyan",
        "light_gray", "dark_gray", "bright_red", "bright_green",
        "bright_yellow", "bright_blue", "bright_magenta", "bright_cyan",
        "white",
    };

    if (!text || !out_color) return false;
    for (usize index = 0; index < MG_TERMINAL_COLOR_COUNT; index++) {
        if (span_equals(text, name, names[index])) {
            *out_color = (mg_terminal_color_t)index;
            return true;
        }
    }
    return false;
}

static bool config_bool(const char *text, config_span_t value,
                        bool *out_value)
{
    if (!text || !out_value) return false;
    if (span_equals(text, value, "true")) {
        *out_value = true;
        return true;
    }
    if (span_equals(text, value, "false")) {
        *out_value = false;
        return true;
    }
    return false;
}

static bool config_history_limit(const char *text, config_span_t value,
                                 u32 *out_value)
{
    u32 parsed = 0;

    if (!text || value.begin == value.end || !out_value) return false;
    for (usize index = value.begin; index < value.end; index++) {
        u32 digit;

        if (text[index] < '0' || text[index] > '9') return false;
        digit = (u32)(text[index] - '0');
        if (parsed > (4096U - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
    }
    if (parsed < 1U || parsed > 4096U) return false;
    *out_value = parsed;
    return true;
}

static bool config_error(char *diagnostic, usize capacity, usize line,
                         const char *reason)
{
    if (diagnostic && capacity)
        (void)snprintf(diagnostic, capacity,
                       "Invalid Shoot config at line %llu: %s",
                       (unsigned long long)line, reason);
    return false;
}

void shoot_config_defaults(shoot_config_t *config)
{
    if (!config) return;
    config->prompt_color = MG_TERMINAL_COLOR_GREEN;
    config->completion_color = MG_TERMINAL_COLOR_DARK_GRAY;
    config->completion_case_sensitive = true;
    config->history_enabled = true;
    config->history_limit = 256U;
    config->history_persist = true;
    config->history_ignore_duplicates = true;
    config->history_ignore_space = true;
}

bool shoot_config_parse(const char *text, usize length,
                        shoot_config_t *config, char *diagnostic,
                        usize diagnostic_capacity)
{
    enum {
        CONFIG_PROMPT_COLOR = 1U << 0,
        CONFIG_COMPLETION_COLOR = 1U << 1,
        CONFIG_COMPLETION_CASE = 1U << 2,
        CONFIG_HISTORY_ENABLED = 1U << 3,
        CONFIG_HISTORY_LIMIT = 1U << 4,
        CONFIG_HISTORY_PERSIST = 1U << 5,
        CONFIG_HISTORY_DUPLICATES = 1U << 6,
        CONFIG_HISTORY_SPACE = 1U << 7,
    };
    shoot_config_t candidate;
    u32 seen = 0;
    usize offset = 0;
    usize line_number = 0;

    if (!text || !config) {
        if (diagnostic && diagnostic_capacity)
            (void)snprintf(diagnostic, diagnostic_capacity,
                           "Invalid Shoot config input.");
        return false;
    }
    shoot_config_defaults(&candidate);
    if (diagnostic && diagnostic_capacity) diagnostic[0] = '\0';
    while (offset < length) {
        usize line_end = offset;
        config_span_t line;
        config_span_t key;
        config_span_t value;
        usize equals = (usize)-1;
        u32 bit = 0;
        bool valid = false;

        line_number++;
        while (line_end < length && text[line_end] != '\n') {
            if (text[line_end] == '\0')
                return config_error(diagnostic, diagnostic_capacity,
                                    line_number, "line contains a NUL byte.");
            line_end++;
        }
        line = trim_span(text, (config_span_t){offset, line_end});
        if (line.begin != line.end &&
            !(line.end - line.begin >= 2U && text[line.begin] == '/' &&
              text[line.begin + 1U] == '/')) {
            for (usize index = line.begin; index < line.end; index++) {
                if (text[index] == '=') {
                    equals = index;
                    break;
                }
            }
            if (equals == (usize)-1)
                return config_error(diagnostic, diagnostic_capacity,
                                    line_number, "expected key=value.");
            key = trim_span(text, (config_span_t){line.begin, equals});
            value = trim_span(text, (config_span_t){equals + 1U, line.end});
            if (key.begin == key.end)
                return config_error(diagnostic, diagnostic_capacity,
                                    line_number, "setting name is empty.");

            if (span_equals(text, key, "prompt_color")) {
                bit = CONFIG_PROMPT_COLOR;
                valid = config_color(text, value, &candidate.prompt_color);
            } else if (span_equals(text, key, "completion_color")) {
                bit = CONFIG_COMPLETION_COLOR;
                valid = config_color(text, value,
                                     &candidate.completion_color);
            } else if (span_equals(text, key, "completion_case_sensitive")) {
                bit = CONFIG_COMPLETION_CASE;
                valid = config_bool(text, value,
                                    &candidate.completion_case_sensitive);
            } else if (span_equals(text, key, "history_enabled")) {
                bit = CONFIG_HISTORY_ENABLED;
                valid = config_bool(text, value,
                                    &candidate.history_enabled);
            } else if (span_equals(text, key, "history_limit")) {
                bit = CONFIG_HISTORY_LIMIT;
                valid = config_history_limit(text, value,
                                             &candidate.history_limit);
            } else if (span_equals(text, key, "history_persist")) {
                bit = CONFIG_HISTORY_PERSIST;
                valid = config_bool(text, value, &candidate.history_persist);
            } else if (span_equals(text, key, "history_ignore_duplicates")) {
                bit = CONFIG_HISTORY_DUPLICATES;
                valid = config_bool(text, value,
                                    &candidate.history_ignore_duplicates);
            } else if (span_equals(text, key, "history_ignore_space")) {
                bit = CONFIG_HISTORY_SPACE;
                valid = config_bool(text, value,
                                    &candidate.history_ignore_space);
            } else {
                return config_error(diagnostic, diagnostic_capacity,
                                    line_number, "unknown setting.");
            }
            if (seen & bit)
                return config_error(diagnostic, diagnostic_capacity,
                                    line_number, "duplicate setting.");
            if (!valid) {
                if (bit == CONFIG_PROMPT_COLOR ||
                    bit == CONFIG_COMPLETION_COLOR)
                    return config_error(diagnostic, diagnostic_capacity,
                                        line_number,
                                        "color name is not supported.");
                if (bit == CONFIG_HISTORY_LIMIT)
                    return config_error(diagnostic, diagnostic_capacity,
                                        line_number,
                                        "history_limit must be from 1 through 4096.");
                return config_error(diagnostic, diagnostic_capacity,
                                    line_number,
                                    "setting value must be true or false.");
            }
            seen |= bit;
        }
        offset = line_end < length ? line_end + 1U : line_end;
    }
    *config = candidate;
    return true;
}

static mg_result_t new_config_path(char *directory, usize directory_capacity,
                                   char *file, usize file_capacity)
{
    mg_result_t result;
    usize length;

    if (!directory || !file || !directory_capacity || !file_capacity)
        return MG_ERR_BAD_ARGUMENT;
    result = app_directory_path(MG_APP_DIRECTORY_CONFIG, "shoot", directory,
                                directory_capacity);
    if (result < 0) return result;
    length = strlen(directory);
    if (length + sizeof("/config") > directory_capacity ||
        length + sizeof("/config") > file_capacity)
        return MG_ERR_BAD_ARGUMENT;
    memcpy(file, directory, length);
    memcpy(file + length, "/config", sizeof("/config"));
    return MG_OK;
}

static mg_result_t read_config_file(const char *path, char **out_data,
                                    usize *out_length)
{
    mg_path_info_t info;
    mg_handle_t handle;
    char *data;
    usize expected;
    usize used = 0;
    mg_result_t result;

    if (!path || !out_data || !out_length) return MG_ERR_BAD_ARGUMENT;
    *out_data = NULL;
    *out_length = 0;
    result = path_info(path, &info);
    if (result != MG_OK) return result;
    if (info.type != MG_PATH_TYPE_FILE || info.size > (u64)((usize)-2))
        return MG_ERR_BAD_ARGUMENT;
    expected = (usize)info.size;
    data = (char *)malloc(expected + 1U);
    if (!data) return MG_ERR_NO_MEMORY;
    result = file_open(path, MG_OPEN_READ);
    if (result < 0) {
        free(data);
        return result;
    }
    handle = (mg_handle_t)result;
    while (used < expected) {
        result = object_read(handle, data + used, expected - used);
        if (result == MG_ERR_END_OF_FILE || result == 0) {
            result = MG_ERR_IO;
            break;
        }
        if (result < 0 || (usize)result > expected - used) {
            if (result >= 0) result = MG_ERR_IO;
            break;
        }
        used += (usize)result;
    }
    if (result >= 0 || result == MG_ERR_END_OF_FILE) {
        char extra;
        mg_result_t extra_result = object_read(handle, &extra, 1U);
        if (extra_result > 0) result = MG_ERR_IO;
        else if (extra_result < 0 && extra_result != MG_ERR_END_OF_FILE)
            result = extra_result;
        else result = MG_OK;
    }
    if (result == MG_OK && memchr(data, '\0', expected) != NULL)
        result = MG_ERR_BAD_ARGUMENT;
    {
        mg_result_t close_result = handle_close(handle);
        if (result == MG_OK && close_result < 0) result = close_result;
    }
    if (result != MG_OK) {
        free(data);
        return result;
    }
    data[expected] = '\0';
    *out_data = data;
    *out_length = expected;
    return MG_OK;
}

static mg_result_t legacy_config_path(char *path, usize capacity)
{
    return command_expand_home_path("~/.shoot/config", true, path,
                                    capacity)
        ? MG_OK : MG_ERR_BAD_ARGUMENT;
}

static mg_result_t migrate_legacy_config(char *directory, const char *file,
                                         const char *data, usize length)
{
    mg_handle_t handle = 0;
    mg_result_t result = app_directory_ensure(MG_APP_DIRECTORY_CONFIG,
                                               "shoot", directory,
                                               SHOOT_CONFIG_DIRECTORY_CAPACITY);

    if (result == MG_OK)
        result = file_create_contents(file, data, length, &handle);
    if (result == MG_OK) {
        mg_result_t close_result = handle_close(handle);
        return close_result < 0 ? close_result : MG_OK;
    }
    return result;
}

static void set_load_error(char *diagnostic, usize capacity,
                           const char *path, mg_result_t error)
{
    if (diagnostic && capacity)
        (void)snprintf(diagnostic, capacity,
                       "Shoot config %s: %s; using built-in defaults.",
                       path ? path : "path", error_string(error));
}

static bool parse_file(const char *path, const char *data, usize length,
                       shoot_config_t *candidate, char *diagnostic,
                       usize capacity)
{
    char parse_diagnostic[160];

    if (!shoot_config_parse(data, length, candidate, parse_diagnostic,
                            sizeof(parse_diagnostic))) {
        if (diagnostic && capacity)
            (void)snprintf(diagnostic, capacity,
                           "Shoot config %s: %s; using built-in defaults.",
                           path, parse_diagnostic);
        return false;
    }
    return true;
}

bool shoot_config_reload(shoot_config_t *config, char *diagnostic,
                         usize diagnostic_capacity)
{
    char directory[SHOOT_CONFIG_DIRECTORY_CAPACITY];
    char file[SHOOT_CONFIG_DIRECTORY_CAPACITY];
    char legacy[SHOOT_CONFIG_DIRECTORY_CAPACITY];
    char *data = NULL;
    usize length = 0;
    shoot_config_t candidate;
    mg_result_t result;

    if (!config) return false;
    shoot_config_defaults(config);
    if (diagnostic && diagnostic_capacity) diagnostic[0] = '\0';
    result = new_config_path(directory, sizeof(directory), file, sizeof(file));
    if (result < 0) {
        set_load_error(diagnostic, diagnostic_capacity,
                       "~/.config/shoot/config", result);
        return false;
    }
    result = read_config_file(file, &data, &length);
    if (result == MG_OK) {
        shoot_config_defaults(&candidate);
        if (!parse_file(file, data, length, &candidate, diagnostic,
                        diagnostic_capacity)) {
            free(data);
            return false;
        }
        free(data);
        *config = candidate;
        return true;
    }
    if (result != MG_ERR_NOT_FOUND) {
        set_load_error(diagnostic, diagnostic_capacity, file, result);
        return false;
    }

    result = legacy_config_path(legacy, sizeof(legacy));
    if (result < 0) {
        set_load_error(diagnostic, diagnostic_capacity,
                       "~/.shoot/config", result);
        return false;
    }
    result = read_config_file(legacy, &data, &length);
    if (result == MG_ERR_NOT_FOUND) return true;
    if (result != MG_OK) {
        set_load_error(diagnostic, diagnostic_capacity, legacy, result);
        return false;
    }
    shoot_config_defaults(&candidate);
    if (!parse_file(legacy, data, length, &candidate, diagnostic,
                    diagnostic_capacity)) {
        free(data);
        return false;
    }
    result = migrate_legacy_config(directory, file, data, length);
    free(data);
    if (result == MG_ERR_ALREADY_EXISTS) {
        result = read_config_file(file, &data, &length);
        if (result != MG_OK) {
            set_load_error(diagnostic, diagnostic_capacity, file, result);
            return false;
        }
        shoot_config_defaults(&candidate);
        if (!parse_file(file, data, length, &candidate, diagnostic,
                        diagnostic_capacity)) {
            free(data);
            return false;
        }
        free(data);
    } else if (result != MG_OK) {
        if (diagnostic && diagnostic_capacity)
            (void)snprintf(diagnostic, diagnostic_capacity,
                           "Could not migrate %s to %s: %s; using the valid legacy settings. The original is preserved.",
                           legacy, file, error_string(result));
    }
    *config = candidate;
    return true;
}
