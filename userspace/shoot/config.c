/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"

#include <mangrove.h>
#include <mg/filesystem.h>
#include <mg/object.h>
#include <string.h>
#include "../common/path.h"

#define SHOOT_CONFIG_DIRECTORY_CAPACITY 320U
#define SHOOT_CONFIG_FILE_CAPACITY      2048U

static const char default_config[] =
    "// Shoot configuration\n"
    "\n"
    "prompt_color=green\n"
    "completion_color=dark_gray\n"
    "completion_case_sensitive=true\n";

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
            }
        }
        if (!next) break;
        line = next;
    }
    return true;
}

static bool config_paths(char *directory, char *file)
{
    mg_identity_t identity;
    usize home_length;

    if (!directory || !file ||
        result_is_error(process_get_identity(&identity)) ||
        identity.uid == MG_UID_SYSTEM || identity.home[0] != '/') return false;
    home_length = strlen(identity.home);
    if (!home_length || home_length + 14U >= SHOOT_CONFIG_DIRECTORY_CAPACITY)
        return false;
    memcpy(directory, identity.home, home_length);
    if (directory[home_length - 1U] != '/') directory[home_length++] = '/';
    memcpy(directory + home_length, ".shoot", 7U);
    directory[home_length + 6U] = '\0';
    memcpy(file, directory, home_length + 6U);
    memcpy(file + home_length + 6U, "/config", 8U);
    return true;
}

static void create_default_config(const char *directory, const char *file)
{
    mg_path_info_t info;
    mg_result_t result;
    mg_handle_t handle;

    result = path_info(directory, &info);
    if (result == MG_ERR_NOT_FOUND) {
        result = directory_create(directory);
        if (result != MG_OK && result != MG_ERR_ALREADY_EXISTS) return;
    } else if (result_is_error(result) ||
               info.type != MG_PATH_TYPE_DIRECTORY) {
        return;
    }

    result = path_info(file, &info);
    if (result == MG_ERR_NOT_FOUND) {
        result = file_create(file);
        if (result != MG_OK && result != MG_ERR_ALREADY_EXISTS) return;
        if (result == MG_OK) {
            result = file_open(file, MG_OPEN_WRITE);
            if (result < 0) return;
            handle = (mg_handle_t)result;
            (void)object_write_all(handle, default_config,
                                   sizeof(default_config) - 1U);
            (void)handle_close(handle);
        }
    }
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
    (void)handle_close(handle);
    data[used] = '\0';
    return MG_OK;
}

void shoot_config_defaults(shoot_config_t *config)
{
    if (!config) return;
    config->prompt_color = MG_TERMINAL_COLOR_GREEN;
    config->completion_color = MG_TERMINAL_COLOR_DARK_GRAY;
    config->completion_case_sensitive = true;
}

bool shoot_config_reload(shoot_config_t *config)
{
    char directory[SHOOT_CONFIG_DIRECTORY_CAPACITY];
    char file[SHOOT_CONFIG_DIRECTORY_CAPACITY];
    char data[SHOOT_CONFIG_FILE_CAPACITY];
    shoot_config_t candidate;
    mg_result_t result;

    if (!config || !config_paths(directory, file)) return false;
    result = read_config_file(file, data, sizeof(data));
    if (result == MG_ERR_NOT_FOUND) {
        shoot_config_defaults(&candidate);
        create_default_config(directory, file);
        *config = candidate;
        return true;
    }
    if (result != MG_OK) return false;
    candidate = *config;
    if (!parse_config(data, &candidate)) return false;
    *config = candidate;
    return true;
}
