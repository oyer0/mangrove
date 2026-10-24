/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"

#include <mangrove.h>
#include <mg/filesystem.h>
#include <mg/object.h>
#include <stdio.h>
#include <string.h>
#include "../common/app_directory.h"
#include "../common/path.h"

#define SHOOT_CONFIG_DIRECTORY_CAPACITY 320U
#define SHOOT_CONFIG_FILE_CAPACITY      2048U

static char *trim(char *text)
{
    char *end;

    while (*text == ' ' || *text == '\t' || *text == '\r') text++;
    end = text + strlen(text);
    while (end != text && (end[-1] == ' ' || end[-1] == '\t' ||
                           end[-1] == '\r')) *--end = '\0';
    return text;
}

static bool config_color(const char *name, mg_terminal_color_t *out_color)
{
    static const char *const names[MG_TERMINAL_COLOR_COUNT] = {
        "black", "red", "green", "yellow", "blue", "magenta", "cyan",
        "light_gray", "dark_gray", "bright_red", "bright_green",
        "bright_yellow", "bright_blue", "bright_magenta", "bright_cyan",
        "white",
    };

    if (!name || !out_color) return false;
    for (usize index = 0; index < MG_TERMINAL_COLOR_COUNT; index++) {
        if (!strcmp(name, names[index])) {
            *out_color = (mg_terminal_color_t)index;
            return true;
        }
    }
    return false;
}

static bool config_bool(const char *value, bool *out_value)
{
    if (!value || !out_value) return false;
    if (!strcmp(value, "true")) {
        *out_value = true;
        return true;
    }
    if (!strcmp(value, "false")) {
        *out_value = false;
        return true;
    }
    return false;
}

static bool config_history_limit(const char *value, u32 *out_value)
{
    u32 parsed = 0;
    const char *cursor;

    if (!value || !*value || !out_value) return false;
    for (cursor = value; *cursor; cursor++) {
        u32 digit;
        if (*cursor < '0' || *cursor > '9') return false;
        digit = (u32)(*cursor - '0');
        if (parsed > (4096U - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
    }
    if (parsed < 1U || parsed > 4096U) return false;
    *out_value = parsed;
    return true;
}

static bool parse_config(char *data, shoot_config_t *config)
{
    char *line = data;

    if (!data || !config) return false;
    while (*line) {
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
            if (!*key) return false;
            if (!strcmp(key, "prompt_color")) {
                if (!config_color(value, &config->prompt_color)) return false;
            } else if (!strcmp(key, "completion_color")) {
                if (!config_color(value, &config->completion_color)) return false;
            } else if (!strcmp(key, "completion_case_sensitive")) {
                if (!config_bool(value, &config->completion_case_sensitive))
                    return false;
            } else if (!strcmp(key, "history_enabled")) {
                if (!config_bool(value, &config->history_enabled)) return false;
            } else if (!strcmp(key, "history_limit")) {
                if (!config_history_limit(value, &config->history_limit))
                    return false;
            } else if (!strcmp(key, "history_persist")) {
                if (!config_bool(value, &config->history_persist)) return false;
            } else if (!strcmp(key, "history_ignore_duplicates")) {
                if (!config_bool(value, &config->history_ignore_duplicates))
                    return false;
            } else if (!strcmp(key, "history_ignore_space")) {
                if (!config_bool(value, &config->history_ignore_space))
                    return false;
            }
        }
        if (!next) break;
        line = next;
    }
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

static mg_result_t read_config_file(const char *path, char *data,
                                    usize capacity)
{
    mg_path_info_t info;
    mg_handle_t handle;
    usize used = 0;
    mg_result_t result;

    result = path_info(path, &info);
    if (result != MG_OK) return result;
    if (info.type != MG_PATH_TYPE_FILE || info.size >= capacity)
        return MG_ERR_BAD_ARGUMENT;
    result = file_open(path, MG_OPEN_READ);
    if (result < 0) return result;
    handle = (mg_handle_t)result;
    while (used < capacity - 1U) {
        result = object_read(handle, data + used, capacity - 1U - used);
        if (result == MG_ERR_END_OF_FILE || result == 0) break;
        if (result < 0 || (usize)result > capacity - 1U - used) {
            (void)handle_close(handle);
            return result < 0 ? result : MG_ERR_IO;
        }
        used += (usize)result;
    }
    result = handle_close(handle);
    if (result < 0) return result;
    if ((u64)used != info.size || memchr(data, '\0', used) != NULL)
        return MG_ERR_IO;
    data[used] = '\0';
    return MG_OK;
}

static mg_result_t legacy_config_path(char *path, usize capacity)
{
    return command_expand_home_path("~/.shoot/config", true, path,
                                    capacity)
        ? MG_OK : MG_ERR_BAD_ARGUMENT;
}

static bool apply_config_text(shoot_config_t *current, const char *data)
{
    char parsed[SHOOT_CONFIG_FILE_CAPACITY];
    shoot_config_t candidate = *current;
    usize length = strlen(data);

    if (length >= sizeof(parsed)) return false;
    memcpy(parsed, data, length + 1U);
    if (!parse_config(parsed, &candidate)) return false;
    *current = candidate;
    return true;
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
    if (result == MG_ERR_ALREADY_EXISTS) return result;
    printf("Shoot: could not copy legacy configuration to %s: %s. "
           "The original remains at ~/.shoot/config.\n",
           file, error_string(result));
    return result;
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

bool shoot_config_reload(shoot_config_t *config)
{
    char directory[SHOOT_CONFIG_DIRECTORY_CAPACITY];
    char file[SHOOT_CONFIG_DIRECTORY_CAPACITY];
    char legacy[SHOOT_CONFIG_DIRECTORY_CAPACITY];
    char data[SHOOT_CONFIG_FILE_CAPACITY];
    char legacy_data[SHOOT_CONFIG_FILE_CAPACITY];
    shoot_config_t candidate;
    mg_result_t result;

    if (!config) return false;
    result = new_config_path(directory, sizeof(directory), file, sizeof(file));
    if (result < 0) return false;
    result = read_config_file(file, data, sizeof(data));
    if (result == MG_ERR_NOT_FOUND) {
        result = legacy_config_path(legacy, sizeof(legacy));
        if (result < 0) return false;
        result = read_config_file(legacy, legacy_data, sizeof(legacy_data));
        if (result == MG_ERR_NOT_FOUND) {
            shoot_config_defaults(&candidate);
            *config = candidate;
            return true;
        }
        if (result != MG_OK) return false;
        shoot_config_defaults(&candidate);
        if (!apply_config_text(&candidate, legacy_data)) return false;
        result = migrate_legacy_config(directory, file, legacy_data,
                                       strlen(legacy_data));
        if (result == MG_ERR_ALREADY_EXISTS) {
            result = read_config_file(file, data, sizeof(data));
            if (result != MG_OK) return false;
            shoot_config_defaults(&candidate);
            if (!apply_config_text(&candidate, data)) return false;
        }
        *config = candidate;
        return true;
    }
    if (result != MG_OK) return false;
    shoot_config_defaults(&candidate);
    if (!apply_config_text(&candidate, data)) return false;
    *config = candidate;
    return true;
}
