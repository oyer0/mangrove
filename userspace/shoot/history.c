/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "history.h"
#include <mg/object.h>
#include <stdlib.h>
#include <string.h>

#define HISTORY_FILE_CAPACITY (MG_FILE_LINES_MAX * MG_FILE_LINE_CAPACITY)

static usize entry_slot(const shoot_history_t *history, usize index)
{
    return (history->editor_history.first + index) % history->editor_history.storage_capacity;
}

static bool ensure_directory(const char *path)
{
    mg_path_info_t info;
    mg_result_t result = path_info(path, &info);
    if (result == MG_ERR_NOT_FOUND) {
        result = directory_create(path);
        return result == MG_OK || result == MG_ERR_ALREADY_EXISTS;
    }
    return result == MG_OK && info.type == MG_PATH_TYPE_DIRECTORY;
}

static mg_result_t snapshot(shoot_history_t *history, char *data,
                             mg_file_lines_state_t *state)
{
    mg_result_t result = file_open(history->path, MG_OPEN_READ);
    if (result == MG_ERR_NOT_FOUND) {
        *state = (mg_file_lines_state_t){0};
        return MG_OK;
    }
    if (result < 0) return result;
    mg_handle_t handle = (mg_handle_t)result;
    result = file_snapshot(handle, data, HISTORY_FILE_CAPACITY, state);
    (void)handle_close(handle);
    return result;
}

static bool add_entry(shoot_history_t *history, const char *text, bool pending,
                       u64 identifier, usize offset)
{
    usize slot = entry_slot(history, history->editor_history.count);
    bool enabled = history->editor_history.enabled;
    history->editor_history.enabled = true;
    bool added = line_editor_history_add_text(&history->editor_history, text);
    history->editor_history.enabled = enabled;
    if (added) history->entries[slot] = (shoot_history_entry_t){pending, identifier, offset};
    return added;
}

static bool already_saved_here(const shoot_history_t *history, u64 identifier, usize offset)
{
    for (usize i = 0; i < history->editor_history.count; i++) {
        const shoot_history_entry_t *entry = &history->entries[entry_slot(history, i)];
        if (entry->saved_identifier == identifier && entry->saved_offset == offset)
            return true;
    }
    return false;
}

/* Validate before importing anything. A legacy unterminated final record is
 * readable with -r/startup, but is not a reliable boundary for -n. */
static bool valid_records(const char *data, usize length)
{
    usize line = 0;
    for (usize i = 0; i < length; i++) {
        if (!data[i]) return false;
        if (data[i] == '\n') line = 0;
        else if (++line >= SHOOT_HISTORY_ENTRY_CAPACITY) return false;
    }
    return true;
}

mg_result_t shoot_history_read(shoot_history_t *history, bool only_new)
{
    if (!history || !history->available || !history->path[0]) return MG_ERR_BAD_ARGUMENT;
    char *data = malloc(HISTORY_FILE_CAPACITY);
    if (!data) return MG_ERR_NO_MEMORY;
    mg_file_lines_state_t state;
    mg_result_t result = snapshot(history, data, &state);
    if (result != MG_OK) { free(data); return result; }
    usize start = 0;
    bool ambiguous = false;
    if (only_new) {
        const mg_file_lines_state_t *old = &history->baseline_state;
        ambiguous = !history->baseline_valid ||
            (old->identifier && old->identifier != state.identifier) ||
            state.length < old->length ||
            state.revision < old->revision ||
            (state.length != old->length && state.revision == old->revision) ||
            (old->length && memcmp(data, history->baseline, old->length)) ||
            (state.length == old->length && state.revision != old->revision);
        if (!ambiguous) start = old->length;
    }
    bool valid = valid_records(data, state.length);
    bool complete = !state.length || data[state.length - 1U] == '\n';
    if (!valid || (only_new && !complete)) ambiguous = true;
    if (!ambiguous) {
        /* A full reread establishes fresh positions. Old positions may refer
         * to a longer version of this same file and must not hide future
         * appends after an explicit reread of a truncated file. */
        if (!only_new) {
            for (usize i = 0; i < history->editor_history.count; i++)
                history->entries[entry_slot(history, i)].saved_identifier = 0;
        }
        char line[SHOOT_HISTORY_ENTRY_CAPACITY];
        usize first = start;
        for (usize i = start; i <= state.length; i++) {
            if (i < state.length && data[i] != '\n') continue;
            usize length = i - first;
            if (length && (!only_new || !already_saved_here(history, state.identifier, first))) {
                memcpy(line, data + first, length);
                if (length && line[length - 1U] == '\r') length--;
                line[length] = '\0';
                (void)add_entry(history, line, false, state.identifier, first);
            }
            first = i + 1U;
        }
    }
    /* Even an ambiguous snapshot becomes the comparison baseline. A later
     * strict append can be imported without replaying the changed file. */
    free(history->baseline);
    history->baseline = data;
    history->baseline_state = state;
    history->baseline_valid = valid && complete;
    if (ambiguous) {
        for (usize i = 0; i < history->editor_history.count; i++)
            history->entries[entry_slot(history, i)].saved_identifier = 0;
        return MG_ERR_RETRY;
    }
    return MG_OK;
}

static mg_result_t save_file(shoot_history_t *history, bool overwrite)
{
    if (!history || !history->available || !history->path[0]) return MG_ERR_BAD_ARGUMENT;
    if (!overwrite && history->write_uncertain) return MG_ERR_UNCERTAIN;
    usize size = 0;
    for (usize i = 0; i < history->editor_history.count; i++) {
        if (overwrite || history->entries[entry_slot(history, i)].pending)
            size += strlen(line_editor_history_at(&history->editor_history, i)) + 1U;
    }
    if (!overwrite && !size) return MG_OK;
    char *data = malloc(size ? size : 1U);
    if (!data) return MG_ERR_NO_MEMORY;
    usize used = 0;
    for (usize i = 0; i < history->editor_history.count; i++) {
        if (!overwrite && !history->entries[entry_slot(history, i)].pending) continue;
        const char *text = line_editor_history_at(&history->editor_history, i);
        usize length = strlen(text);
        memcpy(data + used, text, length); used += length; data[used++] = '\n';
    }
    mg_result_t result = MG_ERR_IO;
    if (!ensure_directory(history->directory)) goto cleanup;
    result = file_create(history->path);
    if (result != MG_OK && result != MG_ERR_ALREADY_EXISTS) goto cleanup;
    result = file_open(history->path, MG_OPEN_RDWR);
    if (result < 0) goto cleanup;
    mg_handle_t handle = (mg_handle_t)result;
    mg_file_lines_state_t state;
    result = file_append_lines_state(handle, data, size, history->config.history_limit,
                                    overwrite ? MG_FILE_LINES_RESET : 0, &state);
    (void)handle_close(handle);
    if (result == MG_ERR_UNCERTAIN) history->write_uncertain = true;
    if (result == MG_OK) {
        usize offset = state.append_offset;
        for (usize i = 0; i < history->editor_history.count; i++) {
            shoot_history_entry_t *entry = &history->entries[entry_slot(history, i)];
            if (!overwrite && !entry->pending) continue;
            entry->pending = false;
            entry->saved_identifier = state.identifier;
            entry->saved_offset = offset;
            offset += strlen(line_editor_history_at(&history->editor_history, i)) + 1U;
        }
        if (overwrite) {
            history->write_uncertain = false;
            free(history->baseline);
            history->baseline = data;
            data = NULL;
            history->baseline_state = state;
            history->baseline_valid = true;
        }
    }
cleanup:
    free(data);
    return result;
}

mg_result_t shoot_history_append(shoot_history_t *history) { return save_file(history, false); }
mg_result_t shoot_history_write(shoot_history_t *history) { return save_file(history, true); }

bool shoot_history_save(shoot_history_t *history)
{
    if (!history || !history->config.history_persist) return true;
    return shoot_history_append(history) == MG_OK;
}

bool shoot_history_init(shoot_history_t *history, const shoot_config_t *config)
{
    char config_path[SHOOT_HISTORY_PATH_CAPACITY];
    if (!history || !config) return false;
    memset(history, 0, sizeof(*history));
    history->config = *config;
    history->storage = calloc(SHOOT_HISTORY_MAX_ENTRIES, SHOOT_HISTORY_ENTRY_CAPACITY);
    history->entries = calloc(SHOOT_HISTORY_MAX_ENTRIES, sizeof(*history->entries));
    history->draft = calloc(1, SHOOT_HISTORY_ENTRY_CAPACITY);
    if (!history->storage || !history->entries || !history->draft) {
        shoot_history_destroy(history); return false;
    }
    line_editor_history_init(&history->editor_history, history->storage,
                             SHOOT_HISTORY_ENTRY_CAPACITY, SHOOT_HISTORY_MAX_ENTRIES);
    line_editor_history_set_automatic_recording(&history->editor_history, false);
    (void)line_editor_history_set_capacity(&history->editor_history, config->history_limit);
    line_editor_history_set_enabled(&history->editor_history, config->history_enabled);
    line_editor_history_set_draft(&history->editor_history, history->draft, SHOOT_HISTORY_ENTRY_CAPACITY);
    history->available = true;
    if (shoot_config_paths(history->directory, sizeof(history->directory), config_path,
                           sizeof(config_path), history->path, sizeof(history->path)) && config->history_persist)
        (void)shoot_history_read(history, false);
    return true;
}

void shoot_history_destroy(shoot_history_t *history)
{
    if (!history) return;
    /* Exit saving is explicit in shell_exit; destroying storage never retries. */
    free(history->storage); free(history->entries); free(history->draft); free(history->baseline);
    memset(history, 0, sizeof(*history));
}

void shoot_history_apply_config(shoot_history_t *history, const shoot_config_t *config)
{
    if (!history || !config) return;
    history->config = *config;
    (void)line_editor_history_set_capacity(&history->editor_history, config->history_limit);
    line_editor_history_set_enabled(&history->editor_history, config->history_enabled);
}

void shoot_history_record(shoot_history_t *history, const char *line)
{
    if (!history || !history->available || !history->config.history_enabled || !line || !*line) return;
    bool whitespace_only = true;
    for (const char *p = line; *p; p++)
        if (*p != ' ' && *p != '\t' && *p != '\r') { whitespace_only = false; break; }
    if (whitespace_only || (history->config.history_ignore_space && line[0] == ' ')) return;
    const char *last = line_editor_history_last(&history->editor_history);
    if (history->config.history_ignore_duplicates && last && !strcmp(last, line)) return;
    (void)add_entry(history, line, true, 0, 0);
}

void shoot_history_clear(shoot_history_t *history)
{
    if (!history) return;
    history->editor_history.count = 0;
    history->editor_history.first = 0;
    history->editor_history.index = -1;
    history->editor_history.draft_cursor = 0;
    history->editor_history.draft_valid = false;
    if (history->draft) history->draft[0] = '\0';
    /* Baseline and uncertain-write state belong to the saved file, not memory. */
}

bool shoot_history_delete(shoot_history_t *history, usize number)
{
    if (!history || !number || number > history->editor_history.count) return false;
    for (usize i = number - 1U; i + 1U < history->editor_history.count; i++) {
        usize to = entry_slot(history, i), from = entry_slot(history, i + 1U);
        memcpy(history->storage + to * SHOOT_HISTORY_ENTRY_CAPACITY,
               history->storage + from * SHOOT_HISTORY_ENTRY_CAPACITY, SHOOT_HISTORY_ENTRY_CAPACITY);
        history->entries[to] = history->entries[from];
    }
    history->editor_history.count--;
    history->editor_history.index = -1;
    history->editor_history.draft_valid = false;
    return true;
}

mg_line_history_t *shoot_history_editor(shoot_history_t *history)
{
    return history ? &history->editor_history : NULL;
}
