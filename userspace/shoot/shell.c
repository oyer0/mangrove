/* SPDX-License-Identifier: GPL-3.0-or-later */
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
#include "completion.h"
#include "history.h"
#include "language.h"
#include "script.h"

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

static bool execute_external(const shell_command_t *command)
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
            return false;
        }
    }

    /* Bare command names resolve from the installed /bin namespace.  This
     * keeps packages from requiring a Shoot rebuild just to become runnable. */
    if (result_is_error(path_info(path, &info)) ||
        info.type != MG_PATH_TYPE_FILE) {
        printf("Unknown command: %s\n", command->name);
        return false;
    }
    strncpy(cmdline, path, sizeof(cmdline) - 1);
    cmdline[sizeof(cmdline) - 1] = '\0';

    for (usize i = 0; i < command->argument_count; i++) {
        usize len = strlen(cmdline);
        bool needs_quotes = strchr(command->arguments[i], ' ') != NULL ||
                            strchr(command->arguments[i], '\t') != NULL;
        usize argument_length = strlen(command->arguments[i]);
        usize extra = needs_quotes ? 2 : 0;
        if (len + 1 + argument_length + extra >= sizeof(cmdline)) {
            printf("Could not run \"%s\": command is too long.\n",
                   command->name);
            return false;
        }
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

    if (command->output_path &&
        !open_redirect_target(command, &output_handle)) return false;
    child_result = command->output_path
        ? process_spawn_with_output(cmdline, output_handle)
        : process_spawn(cmdline);
    if (output_handle) (void)handle_close(output_handle);
    if (result_is_error(child_result)) {
        printf("Could not run \"%s\": %s.\n", command->name,
               error_string(child_result));
        return false;
    }

    child = (mg_handle_t)child_result;
    wait_result = process_wait(child, &status);
    close_result = handle_close(child);

    if (result_is_error(wait_result)) {
        printf("Could not wait for \"%s\": %s.\n", command->name,
               error_string(wait_result));
        return false;
    }
    if (result_is_error(close_result)) {
        printf("Could not close \"%s\": %s.\n", command->name,
               error_string(close_result));
        return false;
    }
    if (status == MG_PROCESS_STATUS_CRASHED) {
        printf("Could not run \"%s\": process crashed.\n", command->name);
        return false;
    }
    if (status != 0) {
        printf("Could not run \"%s\": exited with status %d.\n",
               command->name, status);
        return false;
    }
    return true;
}

static void language_error(const char *source, usize line_number,
                           bool script_mode, const char *message)
{
    if (script_mode && source && *source)
        printf("Shoot: %s:%u: %s.\n", source, (unsigned)line_number,
               message);
    else
        printf("%s.\n", message);
}

bool shell_state_init(shell_state_t *state)
{
    usize cwd_size = 0;

    if (!state) return false;
    memset(state, 0, sizeof(*state));
    if (result_is_error(process_getcwd(state->cwd, sizeof(state->cwd),
                                      &cwd_size))) return false;
    shoot_config_defaults(&state->config);
    if (!shoot_config_reload(&state->config))
        console_write_string("Shoot: invalid configuration; using defaults.\n");
    return true;
}

static bool execute_builtin_command(shell_state_t *state,
                                    const shell_command_t *command)
{
    mg_handle_t output_handle = 0;
    mg_handle_t saved_output_handle = 0;
    bool output_redirected = false;
    bool success;

    if (command->output_path &&
        !open_redirect_target(command, &output_handle)) return false;
    if (output_handle) {
        mg_result_t redirect_result = process_redirect_output(
            output_handle, &saved_output_handle);
        if (result_is_error(redirect_result)) {
            printf("Could not redirect output: %s.\n",
                   error_string(redirect_result));
            (void)handle_close(output_handle);
            return false;
        }
        output_redirected = true;
    }

    /* Builtins execute in Shoot, so keep their multi-write presentation
     * atomic. Their stdout handle is temporarily replaced when redirected. */
    (void)console_begin_transaction();
    success = execute_builtin(state, command);
    if (output_redirected) {
        (void)process_restore_output(saved_output_handle);
        (void)handle_close(output_handle);
    }
    (void)console_end_transaction();
    return success;
}

bool shell_execute_line(shell_state_t *state, const char *line,
                        const char *source, usize line_number,
                        bool script_mode)
{
    shell_assignment_result_t assignment;
    shell_command_t command;
    shell_parse_result_t parse_result;

    if (!state || !line) return false;
    assignment = shell_try_assignment(state, line);
    if (assignment == SHELL_ASSIGNMENT_INVALID) {
        language_error(source, line_number, script_mode,
                       "invalid assignment");
        return false;
    }
    if (assignment == SHELL_ASSIGNMENT_OK) return true;

    parse_result = shell_parse_command(state, line, &command);
    if (parse_result == SHELL_PARSE_EMPTY) return true;
    if (parse_result == SHELL_PARSE_TOO_MANY_ARGUMENTS) {
        language_error(source, line_number, script_mode,
                       "too many arguments");
        return false;
    }
    if (parse_result != SHELL_PARSE_OK) {
        language_error(source, line_number, script_mode,
                       "invalid command syntax");
        return false;
    }
    if (find_builtin(command.name))
        return execute_builtin_command(state, &command);
    return execute_external(&command);
}

static void interactive_block_error(const char *message)
{
    printf("Shoot: %s.\n", message);
}

void shell_run(void)
{
    char line[SHOOT_LINE_CAPACITY];
    char prompt[280];
    shell_state_t state;
    mg_identity_t identity;
    mg_line_editor_t editor;
    shoot_history_t history;
    bool history_ready;

    if (!shell_state_init(&state)) process_exit(1);
    history_ready = shoot_history_init(&history, &state.config);
    if (history_ready) state.history = &history;
    line_editor_init(&editor, line, sizeof(line), "");
    if (history_ready)
        line_editor_set_history(&editor, shoot_history_editor(&history));
    line_editor_set_completion(&editor, shell_complete_line, &state);

    for (;;) {
        shell_script_t script = {0};
        const char *error = NULL;
        bool block_start;

        if (result_is_error(process_get_identity(&identity)) ||
            !make_prompt(&state, &identity, prompt, sizeof(prompt)))
            process_exit(1);
        line_editor_set_prompt_style(&editor, state.config.prompt_color,
                                     MG_TERMINAL_COLOR_BLACK);
        if (!read_command(&editor, prompt)) process_exit(0);
        if (history_ready) shoot_history_record(&history, line);

        block_start = shell_line_keyword(line, "if", NULL) ||
            shell_line_keyword(line, "while", NULL);
        if (shell_line_keyword(line, "else", NULL) ||
            shell_line_keyword(line, "end", NULL)) {
            interactive_block_error("unexpected block terminator");
            continue;
        }
        if (!block_start) {
            (void)shell_execute_line(&state, line, NULL, 0, false);
            continue;
        }
        if (!shell_script_init(&script)) {
            interactive_block_error("unable to allocate script state");
            continue;
        }
        if (!shell_script_append(&script, line, 1, &error)) {
            interactive_block_error(error ? error : "invalid block");
            shell_script_destroy(&script);
            continue;
        }
        usize interactive_line_number = 2;
        while (!shell_script_complete(&script)) {
            if (!read_command(&editor, "...> ")) {
                interactive_block_error("incomplete block");
                shell_script_destroy(&script);
                process_exit(1);
            }
            if (history_ready) shoot_history_record(&history, line);
            if (!shell_script_append(&script, line, interactive_line_number,
                                     &error)) {
                interactive_block_error(error ? error : "invalid block");
                shell_script_destroy(&script);
                script.statements = NULL;
                break;
            }
            interactive_line_number++;
        }
        if (script.statements) {
            (void)shell_script_execute(&script, &state, "interactive");
            shell_script_destroy(&script);
        }
    }
}
