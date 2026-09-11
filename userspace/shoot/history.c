/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "history.h"

#include <mg/filesystem.h>
#include <mg/object.h>
#include <stdlib.h>
#include <string.h>

static bool ensure_directory(const char *path)
{
    mg_path_info_t info;
    mg_result_t result;

    if (!path) return false;
    result = path_info(path, &info);
    if (result == MG_ERR_NOT_FOUND) {
        result = directory_create(path);
        return result == MG_OK || result == MG_ERR_ALREADY_EXISTS;
    }
    return result == MG_OK && info.type == MG_PATH_TYPE_DIRECTORY;
}

static bool rewrite_file(shoot_history_t *history)
{
    mg_path_info_t info;
    mg_handle_t handle;
    mg_result_t result;

    if (!history || !history->available || !history->path[0] ||
        !ensure_directory(history->directory)) return false;

    result = path_info(history->path, &info);
    if (result == MG_ERR_NOT_FOUND) {
        result = file_create(history->path);
        if (result != MG_OK && result != MG_ERR_ALREADY_EXISTS) return false;
    } else if (result != MG_OK || info.type != MG_PATH_TYPE_FILE) {
        return false;
    }

    result = file_open(history->path, MG_OPEN_WRITE);
    if (result < 0) return false;
    handle = (mg_handle_t)result;
    result = file_truncate(handle);
    if (result == MG_OK) {
        for (usize index = 0; index < history->editor_history.count; index++) {
            static const char newline = '\n';
            const char *entry = line_editor_history_at(&history->editor_history,
                                                       index);
            usize length;

            if (!entry) {
                result = MG_ERR_IO;
                break;
            }
            length = strlen(entry);
            if (object_write_all(handle, entry, length) != (mg_result_t)length ||
                object_write_all(handle, &newline, 1) != 1) {
                result = MG_ERR_IO;
                break;
            }
        }
    }
    (void)handle_close(handle);
    return result == MG_OK;
}

static void load_line(shoot_history_t *history, char *line, usize length)
{
    if (length && line[length - 1U] == '\r') length--;
    line[length] = '\0';
    if (length) (void)line_editor_history_add_text(&history->editor_history, line);
}

static void load_file(shoot_history_t *history)
{
    mg_result_t result;
    mg_handle_t handle;
    char buffer[128];
    char line[SHOOT_HISTORY_ENTRY_CAPACITY];
    usize line_length = 0;
    bool oversized = false;

    if (!history || !history->available || !history->path[0]) return;
    result = file_open(history->path, MG_OPEN_READ);
    if (result < 0) return;
    handle = (mg_handle_t)result;
    for (;;) {
        result = object_read(handle, buffer, sizeof(buffer));
        if (result == MG_ERR_END_OF_FILE || result == 0) break;
        if (result < 0) break;
        for (usize index = 0; index < (usize)result; index++) {
            char character = buffer[index];

            if (character == '\n') {
                if (!oversized) load_line(history, line, line_length);
                line_length = 0;
                oversized = false;
            } else if (!oversized) {
                if (line_length + 1U >= sizeof(line)) {
                    oversized = true;
                } else {
                    line[line_length++] = character;
                }
            }
        }
    }
    if (line_length && !oversized) load_line(history, line, line_length);
    (void)handle_close(handle);
    history->editor_history.index = -1;
    history->editor_history.draft_valid = false;
}

bool shoot_history_init(shoot_history_t *history, const shoot_config_t *config)
{
    char config_path[SHOOT_HISTORY_PATH_CAPACITY];

    if (!history || !config) return false;
    memset(history, 0, sizeof(*history));
    history->config = *config;
    line_editor_history_init(&history->editor_history, NULL,
                             SHOOT_HISTORY_ENTRY_CAPACITY, 0);
    line_editor_history_set_automatic_recording(&history->editor_history, false);
    line_editor_history_set_enabled(&history->editor_history, false);

    history->storage = (char *)calloc(SHOOT_HISTORY_MAX_ENTRIES,
                                      SHOOT_HISTORY_ENTRY_CAPACITY);
    history->draft = (char *)calloc(1, SHOOT_HISTORY_ENTRY_CAPACITY);
    if (!history->storage || !history->draft) {
        shoot_history_destroy(history);
        return false;
    }

    line_editor_history_init(&history->editor_history, history->storage,
                             SHOOT_HISTORY_ENTRY_CAPACITY,
                             SHOOT_HISTORY_MAX_ENTRIES);
    line_editor_history_set_automatic_recording(&history->editor_history, false);
    line_editor_history_set_capacity(&history->editor_history,
                                     config->history_limit);
    line_editor_history_set_enabled(&history->editor_history,
                                    config->history_enabled);
    line_editor_history_set_draft(&history->editor_history, history->draft,
                                  SHOOT_HISTORY_ENTRY_CAPACITY);
    history->available = true;
    if (shoot_config_paths(history->directory, sizeof(history->directory),
                           config_path, sizeof(config_path), history->path,
                           sizeof(history->path)) &&
        config->history_persist) load_file(history);
    return true;
}

void shoot_history_destroy(shoot_history_t *history)
{
    if (!history) return;
    free(history->storage);
    free(history->draft);
    history->storage = NULL;
    history->draft = NULL;
    history->available = false;
}

void shoot_history_apply_config(shoot_history_t *history,
                                const shoot_config_t *config)
{
    u32 previous_limit;

    if (!history || !config) return;
    previous_limit = history->config.history_limit;
    history->config = *config;
    if (history->storage) {
        (void)line_editor_history_set_capacity(&history->editor_history,
                                               config->history_limit);
        line_editor_history_set_enabled(&history->editor_history,
                                        config->history_enabled);
        if (config->history_persist &&
            previous_limit != config->history_limit)
            (void)rewrite_file(history);
    }
}

void shoot_history_record(shoot_history_t *history, const char *line)
{
    const char *last;
    bool whitespace_only = true;

    if (!history || !history->available || !history->config.history_enabled ||
        !line || !*line) return;
    for (const char *cursor = line; *cursor; cursor++) {
        if (*cursor != ' ' && *cursor != '\t' && *cursor != '\r') {
            whitespace_only = false;
            break;
        }
    }
    if (whitespace_only ||
        (history->config.history_ignore_space && line[0] == ' ')) return;

    last = line_editor_history_last(&history->editor_history);
    if (history->config.history_ignore_duplicates && last &&
        !strcmp(last, line)) return;
    if (!line_editor_history_add_text(&history->editor_history, line)) return;
    if (history->config.history_persist) (void)rewrite_file(history);
}

void shoot_history_clear(shoot_history_t *history)
{
    if (!history) return;
    history->editor_history.count = 0;
    history->editor_history.first = 0;
    history->editor_history.index = -1;
    history->editor_history.draft_cursor = 0;
    history->editor_history.draft_valid = false;
    if (history->editor_history.draft_storage &&
        history->editor_history.draft_capacity > 0)
        history->editor_history.draft_storage[0] = '\0';
    if (history->config.history_persist) (void)rewrite_file(history);
}

mg_line_history_t *shoot_history_editor(shoot_history_t *history)
{
    return history ? &history->editor_history : NULL;
}
