/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mg/error.h>
#include <mg/filesystem.h>
#include <mg/line_editor.h>
#include <mangrove.h>
#include <mg/object.h>
#include <mg/process.h>
#include "../common/path.h"

static void shell_byte_buffer_release(u8 **buffer)
{
    if (buffer && *buffer) {
        free(*buffer);
        *buffer = NULL;
    }
}

#define SHELL_BYTE_BUFFER_AUTO __attribute__((cleanup(shell_byte_buffer_release)))
#include "shell.h"
#include "builtin.h"
#include "completion.h"
#include "history.h"
#include "language.h"
#include "glob.h"
#include "script.h"

static bool prompt_location(const char *cwd, const char *home,
                            char *location, usize capacity)
{
    const char *display;
    usize display_length;
    usize start;

    if (!home || !location || capacity < 2U) return false;
    if (!cwd || cwd[0] != '/') cwd = "/";
    usize home_length = strlen(home);
    if (home_length && strcmp(cwd, home) == 0) {
        if (capacity < 2) return false;
        location[0] = '~';
        location[1] = '\0';
        return true;
    }

    display = cwd;
    if (home_length != 0 && home_length < strlen(cwd) &&
        strncmp(cwd, home, home_length) == 0 &&
        home[home_length - 1] != '/' &&
        cwd[home_length] == '/') display = cwd + home_length;
    display_length = strlen(display);
    if (display == cwd && display_length + 1U > capacity) {
        static const char prefix[] = "...";
        usize suffix_capacity = capacity - sizeof(prefix);
        start = display_length > suffix_capacity
            ? display_length - suffix_capacity : 0U;
        while (start < display_length &&
               (((u8)display[start] & 0xc0U) == 0x80U)) start++;
        memcpy(location, prefix, sizeof(prefix) - 1U);
        memcpy(location + sizeof(prefix) - 1U, display + start,
               display_length - start + 1U);
        return true;
    }
    if (display != cwd && display_length + 2U > capacity) {
        usize suffix_capacity = capacity - 5U;
        start = display_length > suffix_capacity
            ? display_length - suffix_capacity : 0U;
        while (start < display_length &&
               (((u8)display[start] & 0xc0U) == 0x80U)) start++;
        location[0] = '~';
        location[1] = '.';
        location[2] = '.';
        location[3] = '.';
        memcpy(location + 4U, display + start, display_length - start + 1U);
        return true;
    }
    if (display != cwd) {
        location[0] = '~';
        memcpy(location + 1U, display, display_length + 1U);
        return true;
    }
    if (display_length + 1U > capacity) return false;
    memcpy(location, display, display_length + 1U);
    return true;
}

static bool make_prompt(const shell_state_t *state,
                        const mg_identity_t *identity, char *prompt,
                        usize capacity)
{
    char location[240];
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

static mg_result_t read_command(mg_line_editor_t *editor, const char *prompt)
{
    if (!editor || !prompt) return MG_ERR_BAD_ARGUMENT;
    line_editor_set_prompt(editor, prompt);
    return line_editor_read_line(editor);
}

static bool open_redirect_target(const shell_command_t *command,
                                 mg_handle_t *out_handle)
{
    COMMAND_PATH_AUTO char *expanded_path = NULL;
    mg_path_info_t info;
    mg_result_t result;

    if (!command || !command->output_path || !out_handle) return false;
    if (!command_expand_home_path_alloc(command->output_path,
                                        command->output_home_expand,
                                        &expanded_path)) {
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

static bool execute_external(shell_state_t *state,
                             const shell_command_t *command)
{
    mg_result_t child_result;
    mg_result_t wait_result;
    mg_result_t close_result;
    mg_handle_t child;
    mg_handle_t output_handle = 0;
    mg_path_info_t info;
    COMMAND_PATH_AUTO char *path = NULL;
    const char **argv;
    i32 status = 0;
    if (command->name[0] == '/') {
        if (!command_path_copy_alloc(command->name, &path)) {
            printf("Could not run \"%s\": invalid executable path.\n",
                   command->name);
            return false;
        }
    } else {
        if (!command_build_executable_path_alloc(command->name, &path)) {
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
    if (command->argument_count >= MG_PROCESS_MAX_ARGUMENTS) return false;
    usize argc = command->argument_count + 1U;
    usize argument_bytes = (argc + 1U) * sizeof(uintptr_t);
    usize executable_bytes = strlen(path) + 1U;
    if (executable_bytes > MG_PROCESS_ARGUMENT_BYTES_MAX - argument_bytes)
        return false;
    argument_bytes += executable_bytes;
    for (usize index = 0; index < command->argument_count; index++) {
        usize length = strlen(command->arguments[index]);
        if (length >= MG_PROCESS_ARGUMENT_BYTES_MAX - argument_bytes) {
            printf("Could not run \"%s\": argument vector exceeds the native limit.\n",
                   command->name);
            return false;
        }
        argument_bytes += length + 1U;
    }
    argv = malloc(argc * sizeof(*argv));
    if (!argv) {
        printf("Could not run \"%s\": not enough memory.\n", command->name);
        return false;
    }
    argv[0] = path;
    for (usize index = 0; index < command->argument_count; index++)
        argv[index + 1U] = command->arguments[index];
    if (command->output_path && !open_redirect_target(command, &output_handle)) {
        free(argv);
        return false;
    }
    child_result = command->output_path
        ? process_spawn_argv_with_output(argv,
                                         command->argument_count + 1U,
                                         output_handle)
        : process_spawn_argv(argv, command->argument_count + 1U);
    free(argv);
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
        if (wait_result == MG_ERR_SHUTDOWN)
            shell_exit(state, 0);
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
    /* A child that ran and returned nonzero owns its ordinary diagnostic.
     * Keep the failure result for script control flow without printing a
     * second, generic message over the child's useful output. */
    return status == 0;
}

static const char *glob_error_text(mg_result_t result)
{
    if (result == MG_ERR_BAD_ARGUMENT) return "invalid bracket pattern";
    if (result == MG_ERR_BUFFER_TOO_SMALL)
        return "expanded arguments exceed the native argument budget";
    if (result == MG_ERR_NAME_TOO_LONG)
        return "expanded pathname exceeds the native path limit";
    if (result == MG_ERR_NO_MEMORY) return "not enough memory";
    return error_string(result);
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
    if (!state) return false;
    memset(state, 0, sizeof(*state));
    if (result_is_error(command_getcwd_alloc(&state->cwd))) return false;
    shoot_config_defaults(&state->config);
    if (!shoot_config_reload(&state->config))
        console_write_string("Shoot: invalid configuration; using defaults.\n");
    return true;
}

static bool shell_refresh_cwd(shell_state_t *state)
{
    char *cwd = NULL;
    if (!state || result_is_error(command_getcwd_alloc(&cwd))) return false;
    free(state->cwd);
    state->cwd = cwd;
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
    shell_command_t *command;
    shell_expanded_arguments_t expanded_arguments;
    COMMAND_PATH_AUTO char *home_glob_path = NULL;
    SHELL_BYTE_BUFFER_AUTO u8 *home_glob_flags = NULL;
    shell_parse_result_t parse_result;
    const char *failed_pattern = NULL;
    mg_result_t expansion_result;

    if (!state || !line) return false;
    assignment = shell_try_assignment(state, line);
    if (assignment == SHELL_ASSIGNMENT_INVALID) {
        language_error(source, line_number, script_mode,
                       "invalid assignment");
        return false;
    }
    if (assignment == SHELL_ASSIGNMENT_OK) return true;

    command = malloc(sizeof(*command));
    if (!command) {
        language_error(source, line_number, script_mode, "not enough memory");
        return false;
    }
    parse_result = shell_parse_command(state, line, command);
    if (parse_result == SHELL_PARSE_EMPTY) {
        free(command);
        return true;
    }
    if (parse_result == SHELL_PARSE_TOO_MANY_ARGUMENTS) {
        language_error(source, line_number, script_mode,
                       "too many arguments");
        free(command);
        return false;
    }
    if (parse_result != SHELL_PARSE_OK) {
        language_error(source, line_number, script_mode,
                       "invalid command syntax");
        free(command);
        return false;
    }
    {
        usize name_length = strlen(command->name);
        usize maximum = MG_PATH_MAX_BYTES;
        if (!strchr(command->name, '/')) maximum -= 5U;
        if (name_length > maximum) {
            language_error(source, line_number, script_mode,
                           "command name is too long");
            free(command);
            return false;
        }
    }
    if (!strcmp(command->name, "jump") && command->argument_count == 1U &&
        command->argument_home_expand[0] && command->arguments[0][0] == '~') {
        const char *path = command->arguments[0];
        const u8 *flags = command->argument_glob_flags[0];
        bool active_pattern = false;
        for (usize index = 0; path[index]; index++)
            if (flags[index] && (path[index] == '*' || path[index] == '?' ||
                                 path[index] == '[')) active_pattern = true;
        if (active_pattern) {
            usize source_length = strlen(path);
            usize expanded_length;
            if (!command_expand_home_path_alloc(path, true,
                                                &home_glob_path)) {
                printf("Could not expand pathname pattern \"%s\": invalid path.\n",
                       path);
                free(command);
                return false;
            }
            expanded_length = strlen(home_glob_path);
            if (expanded_length < source_length - 1U) {
                printf("Could not expand pathname pattern \"%s\": invalid path.\n",
                       path);
                free(command);
                return false;
            }
            usize home_length = expanded_length - (source_length - 1U);
            home_glob_flags = (u8 *)calloc(expanded_length, sizeof(*home_glob_flags));
            if (!home_glob_flags) {
                printf("Could not expand pathname pattern \"%s\": not enough memory.\n",
                       path);
                free(command);
                return false;
            }
            memcpy(home_glob_flags + home_length, flags + 1U,
                   source_length - 1U);
            command->arguments[0] = home_glob_path;
            command->argument_glob_flags[0] = home_glob_flags;
        }
    }
    expansion_result = shell_expand_arguments(command,
                                              &expanded_arguments,
                                              &failed_pattern);
    if (result_is_error(expansion_result)) {
        printf("Could not expand pathname pattern \"%s\": %s.\n",
               failed_pattern ? failed_pattern : "",
               glob_error_text(expansion_result));
        free(command);
        return false;
    }
    command->argument_count = expanded_arguments.count;
    command->arguments = expanded_arguments.arguments;
    bool success = find_builtin(command->name)
        ? execute_builtin_command(state, command)
        : execute_external(state, command);
    shell_expanded_arguments_destroy(&expanded_arguments);
    free(command);
    return success;
}

static void interactive_block_error(const char *message)
{
    printf("Shoot: %s.\n", message);
}

void shell_exit(shell_state_t *state, i32 status)
{
    if (state && state->history) {
        if (!shoot_history_save(state->history))
            console_write_string(state->history->write_uncertain
                ? "Shoot: history write uncertain; append blocked. Unsaved entries will be lost on exit.\n"
                : "Shoot: could not save command history; unsaved entries will be lost on exit.\n");
        shoot_history_destroy(state->history);
        state->history = NULL;
    }
    if (state) {
        free(state->cwd);
        state->cwd = NULL;
    }
    process_exit(status);
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

    if (!shell_state_init(&state)) shell_exit(&state, 1);
    history_ready = shoot_history_init(&history, &state.config);
    if (history_ready) {
        state.history = &history;
        if (state.config.history_persist && !history.baseline_valid)
            console_write_string("Shoot: saved history has no reliable import baseline; use history -r to inspect it.\n");
    }
    line_editor_init(&editor, line, sizeof(line), "");
    line_editor_set_key_event_input(&editor, true);
    if (history_ready)
        line_editor_set_history(&editor, shoot_history_editor(&history));
    line_editor_set_completion(&editor, shell_complete_line, &state);

    for (;;) {
        shell_script_t script = {0};
        const char *error = NULL;
        bool block_start;
        mg_result_t input_result;

        if ((!state.cwd && !shell_refresh_cwd(&state)) ||
            result_is_error(process_get_identity(&identity)) ||
            !make_prompt(&state, &identity, prompt, sizeof(prompt)))
            shell_exit(&state, 1);
        line_editor_set_prompt_style(&editor, state.config.prompt_color,
                                     MG_TERMINAL_COLOR_BLACK);
        input_result = read_command(&editor, prompt);
        if (input_result == MG_ERR_CANCELLED) continue;
        if (input_result == MG_ERR_SHUTDOWN) shell_exit(&state, 0);
        if (input_result == MG_ERR_END_OF_FILE) shell_exit(&state, 0);
        if (input_result < 0) shell_exit(&state, 1);
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
            input_result = read_command(&editor, "...> ");
            if (input_result == MG_ERR_CANCELLED) {
                shell_script_destroy(&script);
                break;
            }
            if (input_result == MG_ERR_SHUTDOWN) {
                shell_script_destroy(&script);
                shell_exit(&state, 0);
            }
            if (input_result == MG_ERR_END_OF_FILE) {
                shell_script_destroy(&script);
                shell_exit(&state, 0);
            }
            if (input_result < 0) {
                interactive_block_error("incomplete block");
                shell_script_destroy(&script);
                shell_exit(&state, 1);
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
