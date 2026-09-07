/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <string.h>
#include <mg/error.h>
#include <mg/filesystem.h>
#include <mg/line_editor.h>
#include <mangrove.h>
#include <mg/object.h>
#include <mg/process.h>
#include "../common/path.h"
#include "shell.h"
#include "builtin.h"

#define SHOOT_LINE_CAPACITY 512
#define SHOOT_HISTORY_CAPACITY 16

typedef enum shell_parse_result {
    SHELL_PARSE_OK,
    SHELL_PARSE_EMPTY,
    SHELL_PARSE_TOO_MANY_ARGUMENTS,
    SHELL_PARSE_INVALID_SYNTAX,
} shell_parse_result_t;

static const char *skip_spaces(const char *text)
{
    while (*text == ' ' || *text == '\t') text++;
    return text;
}

static bool parse_word(const char **cursor, char *storage, usize capacity,
                       usize *used, const char **out_word,
                       bool *out_home_expand)
{
    const char *source;
    usize start;
    char quote = '\0';
    bool saw_character = false;
    bool first_value = true;
    bool home_expand = true;

    if (!cursor || !*cursor || !storage || !used || !out_word ||
        !out_home_expand ||
        *used >= capacity) return false;
    source = *cursor;
    start = *used;
    while (*source != '\0') {
        char value = *source;

        if (quote != '\0') {
            if (value == quote) {
                quote = '\0';
                source++;
                saw_character = true;
                continue;
            }
        } else if (value == '\'' || value == '"') {
            quote = value;
            source++;
            saw_character = true;
            continue;
        } else if (value == ' ' || value == '\t' || value == '>') {
            break;
        }

        if (*used + 1U >= capacity) return false;
        if (first_value) {
            home_expand = quote != '\'';
            first_value = false;
        }
        storage[(*used)++] = value;
        source++;
        saw_character = true;
    }
    if (quote != '\0' || !saw_character || *used >= capacity) return false;
    storage[(*used)++] = '\0';
    *cursor = source;
    *out_word = storage + start;
    *out_home_expand = home_expand;
    return true;
}

static shell_parse_result_t parse_command(const char *line,
                                           shell_command_t *command)
{
    const char *cursor;
    usize storage_length = 0;

    if (!line || !command) return SHELL_PARSE_EMPTY;
    memset(command->storage, 0, sizeof(command->storage));
    cursor = skip_spaces(line);
    if (*cursor == '\0') return SHELL_PARSE_EMPTY;
    command->name = NULL;
    command->argument_count = 0;
    command->output_path = NULL;
    command->output_append = false;

    while (*cursor != '\0') {
        const char *word;

        if (*cursor == '>') {
            bool append = cursor[1] == '>';

            if (command->output_path) return SHELL_PARSE_INVALID_SYNTAX;
            cursor += append ? 2 : 1;
            cursor = skip_spaces(cursor);
            if (*cursor == '\0' || *cursor == '>')
                return SHELL_PARSE_INVALID_SYNTAX;
            if (!parse_word(&cursor, command->storage,
                            sizeof(command->storage), &storage_length,
                            &word, &command->output_home_expand) ||
                word[0] == '\0') {
                return SHELL_PARSE_INVALID_SYNTAX;
            }
            command->output_path = word;
            command->output_append = append;
            cursor = skip_spaces(cursor);
            continue;
        }

        if (command->name && command->argument_count == SHOOT_MAX_ARGUMENTS)
            return SHELL_PARSE_TOO_MANY_ARGUMENTS;
        if (!parse_word(&cursor, command->storage,
                        sizeof(command->storage), &storage_length, &word,
                        &command->argument_home_expand[
                            command->argument_count])) {
            return SHELL_PARSE_INVALID_SYNTAX;
        }
        if (!command->name) {
            command->name = word;
        } else {
            command->arguments[command->argument_count++] = word;
        }
        cursor = skip_spaces(cursor);
    }
    return command->name ? SHELL_PARSE_OK : SHELL_PARSE_INVALID_SYNTAX;
}

static bool prompt_location(const char *cwd, const char *home,
                            char *location, usize capacity)
{
    usize cwd_length;
    usize home_length;

    if (!cwd || !home || !location || capacity == 0) return false;
    if (cwd[0] != '/') return false;

    cwd_length = strlen(cwd);
    home_length = strlen(home);

    if (home_length != 0 && strcmp(cwd, home) == 0) {
        if (capacity < 2) return false;
        location[0] = '~';
        location[1] = '\0';
        return true;
    }

    if (home_length != 0 && home_length < cwd_length &&
        strncmp(cwd, home, home_length) == 0 &&
        home[home_length - 1] != '/' &&
        cwd[home_length] == '/') {
        usize suffix_length = cwd_length - home_length;
        if (suffix_length + 2 > capacity) return false;
        location[0] = '~';
        memcpy(location + 1, cwd + home_length, suffix_length + 1);
        return true;
    }

    if (cwd_length + 1 > capacity) return false;
    memcpy(location, cwd, cwd_length + 1);
    return true;
}

static bool make_prompt(const shell_state_t *state,
                        const mg_identity_t *identity, char *prompt,
                        usize capacity)
{
    char location[256];
    usize username_length;
    usize location_length;

    if (!state || !identity || !prompt || capacity == 0 ||
        identity->username[0] == '\0' ||
        !prompt_location(state->cwd, identity->home, location,
                         sizeof(location))) return false;
    username_length = strlen(identity->username);
    location_length = strlen(location);
    if (username_length + location_length + 4U > capacity) return false;
    strcpy(prompt, identity->username);
    prompt[username_length] = ' ';
    strcpy(prompt + username_length + 1U, location);
    prompt[username_length + 1U + location_length] = ':';
    prompt[username_length + 2U + location_length] = ' ';
    prompt[username_length + 3U + location_length] = '\0';
    return true;
}

static bool read_command(mg_line_editor_t *editor, const char *prompt)
{
    mg_result_t result;

    if (!editor || !prompt) return false;
    line_editor_set_prompt(editor, prompt);
    result = line_editor_read_line(editor);
    return result >= 0;
}

static bool open_redirect_target(const shell_command_t *command,
                                 mg_handle_t *out_handle)
{
    char expanded_path[256];
    mg_path_info_t info;
    mg_result_t result;

    if (!command || !command->output_path || !out_handle) return false;
    if (!command_expand_home_path(command->output_path,
                                  command->output_home_expand,
                                  expanded_path, sizeof(expanded_path))) {
        printf("Could not redirect output to \"%s\": invalid path.\n",
               command->output_path);
        return false;
    }
    result = path_info(expanded_path, &info);
    if (result == MG_ERR_NOT_FOUND) {
        result = file_create(expanded_path);
        if (result != MG_OK) {
            printf("Could not redirect output to \"%s\": %s.\n",
                   command->output_path, error_string(result));
            return false;
        }
    } else if (result_is_error(result)) {
        printf("Could not redirect output to \"%s\": %s.\n",
               command->output_path, error_string(result));
        return false;
    } else if (info.type != MG_PATH_TYPE_FILE) {
        printf("Could not redirect output to \"%s\": not a file.\n",
               command->output_path);
        return false;
    }
    result = file_open(expanded_path, MG_OPEN_WRITE);
    if (result_is_error(result)) {
        printf("Could not redirect output to \"%s\": %s.\n",
               command->output_path, error_string(result));
        return false;
    }
    *out_handle = (mg_handle_t)result;
    if (command->output_append) {
        result = file_seek(*out_handle, 0, MG_SEEK_END);
    } else {
        result = file_truncate(*out_handle);
    }
    if (result_is_error(result)) {
        printf("Could not redirect output to \"%s\": %s.\n",
               command->output_path, error_string(result));
        (void)handle_close(*out_handle);
        *out_handle = 0;
        return false;
    }
    return true;
}

static void execute_external(const shell_command_t *command)
{
    mg_result_t child_result;
    mg_result_t wait_result;
    mg_result_t close_result;
    mg_handle_t child;
    mg_handle_t output_handle = 0;
    mg_path_info_t info;
    char path[256];
    char cmdline[512];
    i32 status = 0;
    if (command->name[0] == '/') {
        strncpy(path, command->name, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        if (!command_build_executable_path(command->name, path,
                                           sizeof(path))) {
            printf("Unknown command: %s\n", command->name);
            return;
        }
    }

    /* Bare command names resolve from the installed /bin namespace.  This
     * keeps packages from requiring a Shoot rebuild just to become runnable. */
    if (result_is_error(path_info(path, &info)) ||
        info.type != MG_PATH_TYPE_FILE) {
        printf("Unknown command: %s\n", command->name);
        return;
    }
    strncpy(cmdline, path, sizeof(cmdline) - 1);
    cmdline[sizeof(cmdline) - 1] = '\0';

    for (usize i = 0; i < command->argument_count; i++) {
        usize len = strlen(cmdline);
        bool needs_quotes = strchr(command->arguments[i], ' ') != NULL ||
                            strchr(command->arguments[i], '\t') != NULL;
        usize argument_length = strlen(command->arguments[i]);
        usize extra = needs_quotes ? 2 : 0;
        if (len + 1 + argument_length + extra < sizeof(cmdline)) {
            cmdline[len] = ' ';
            if (needs_quotes) {
                cmdline[len + 1] = '"';
                memcpy(cmdline + len + 2, command->arguments[i], argument_length);
                cmdline[len + 2 + argument_length] = '"';
                cmdline[len + 3 + argument_length] = '\0';
            } else {
                strcpy(cmdline + len + 1, command->arguments[i]);
            }
        }
    }

    if (command->output_path &&
        !open_redirect_target(command, &output_handle)) return;
    child_result = command->output_path
        ? process_spawn_with_output(cmdline, output_handle)
        : process_spawn(cmdline);
    if (output_handle) (void)handle_close(output_handle);
    if (result_is_error(child_result)) {
        printf("Could not run \"%s\": %s.\n", command->name,
               error_string(child_result));
        return;
    }

    child = (mg_handle_t)child_result;
    wait_result = process_wait(child, &status);
    close_result = handle_close(child);

    if (result_is_error(wait_result)) {
        printf("Could not wait for \"%s\": %s.\n", command->name,
               error_string(wait_result));
        return;
    }
    if (result_is_error(close_result)) {
        printf("Could not close \"%s\": %s.\n", command->name,
               error_string(close_result));
        return;
    }
    if (status == MG_PROCESS_STATUS_CRASHED) {
        printf("Could not run \"%s\": process crashed.\n", command->name);
        return;
    }
    if (status != 0) {
        printf("Could not run \"%s\": exited with status %d.\n",
               command->name, status);
        return;
    }
}

void shell_run(void)
{
    char line[SHOOT_LINE_CAPACITY];
    char history_entries[SHOOT_HISTORY_CAPACITY][SHOOT_LINE_CAPACITY];
    char prompt[280];
    shell_command_t command;
    shell_state_t state;
    mg_identity_t identity;
    usize cwd_size = 0;
    mg_line_editor_t editor;
    mg_line_history_t history;

    if (result_is_error(process_getcwd(state.cwd, sizeof(state.cwd),
                                      &cwd_size)))
        process_exit(1);
    line_editor_init(&editor, line, sizeof(line), "");
    line_editor_history_init(&history, &history_entries[0][0],
                             sizeof(history_entries[0]),
                             SHOOT_HISTORY_CAPACITY);
    line_editor_set_history(&editor, &history);

    for (;;) {
        if (result_is_error(process_get_identity(&identity)) ||
            !make_prompt(&state, &identity, prompt, sizeof(prompt)))
            process_exit(1);
        if (!read_command(&editor, prompt)) process_exit(0);

        switch (parse_command(line, &command)) {
        case SHELL_PARSE_EMPTY:
            break;
        case SHELL_PARSE_TOO_MANY_ARGUMENTS:
            console_begin_transaction();
            printf("Too many arguments: maximum %u.\n",
                   (unsigned)SHOOT_MAX_ARGUMENTS);
            if (result_is_error(process_get_identity(&identity)) ||
                !make_prompt(&state, &identity, prompt, sizeof(prompt)))
                process_exit(1);
            line_editor_set_prompt(&editor, prompt);
            line_editor_prepare_next_prompt(&editor);
            console_end_transaction();
            break;
        case SHELL_PARSE_INVALID_SYNTAX:
            console_begin_transaction();
            printf("Invalid command syntax.\n");
            if (result_is_error(process_get_identity(&identity)) ||
                !make_prompt(&state, &identity, prompt, sizeof(prompt)))
                process_exit(1);
            line_editor_set_prompt(&editor, prompt);
            line_editor_prepare_next_prompt(&editor);
            console_end_transaction();
            break;
        case SHELL_PARSE_OK:
            if (find_builtin(command.name)) {
                mg_handle_t output_handle = 0;
                mg_handle_t saved_output_handle = 0;
                bool output_redirected = false;

                if (command.output_path &&
                    !open_redirect_target(&command, &output_handle)) break;
                if (output_handle) {
                    mg_result_t redirect_result = process_redirect_output(
                        output_handle, &saved_output_handle);
                    if (result_is_error(redirect_result)) {
                        printf("Could not redirect output: %s.\n",
                               error_string(redirect_result));
                        (void)handle_close(output_handle);
                        break;
                    }
                    output_redirected = true;
                }

                /* Builtins execute in Shoot, so keep their multi-write
                 * presentation atomic.  Their stdout handle is temporarily
                 * replaced when redirection was requested. */
                console_begin_transaction();
                execute_builtin(&state, &command);
                if (output_redirected) {
                    (void)process_restore_output(saved_output_handle);
                    (void)handle_close(output_handle);
                }
                if (result_is_error(process_get_identity(&identity)) ||
                    !make_prompt(&state, &identity, prompt, sizeof(prompt)))
                    process_exit(1);
                line_editor_set_prompt(&editor, prompt);
                line_editor_prepare_next_prompt(&editor);
                console_end_transaction();
            } else {
                execute_external(&command);
                if (result_is_error(process_get_identity(&identity)) ||
                    !make_prompt(&state, &identity, prompt, sizeof(prompt)))
                    process_exit(1);
                line_editor_set_prompt(&editor, prompt);
                editor.prompt_drawn = false;
            }
            break;
        }
    }
}
