#include <stdio.h>
#include <string.h>
#include <mg/error.h>
#include <mg/filesystem.h>
#include <mg/line_editor.h>
#include <mangrove.h>
#include <mg/object.h>
#include <mg/process.h>
#include "shell.h"
#include "builtin.h"

#define SHOOT_LINE_CAPACITY 512
#define SHOOT_HISTORY_CAPACITY 16

typedef enum shell_parse_result {
    SHELL_PARSE_OK,
    SHELL_PARSE_EMPTY,
    SHELL_PARSE_TOO_MANY_ARGUMENTS,
} shell_parse_result_t;

static char *skip_spaces(char *text)
{
    while (*text == ' ' || *text == '\t') text++;
    return text;
}

static void trim_trailing_spaces(char *text)
{
    usize length = strlen(text);
    while (length != 0 && (text[length - 1] == ' ' ||
                           text[length - 1] == '\t')) {
        text[--length] = '\0';
    }
}

static shell_parse_result_t parse_command(char *line,
                                           shell_command_t *command)
{
    char *cursor;

    if (!line || !command) return SHELL_PARSE_EMPTY;
    trim_trailing_spaces(line);
    cursor = skip_spaces(line);
    if (*cursor == '\0') return SHELL_PARSE_EMPTY;
    command->name = NULL;
    command->argument_count = 0;

    while (*cursor != '\0') {
        char *argument;
        char *write;
        char quote = '\0';

        if (command->argument_count == SHOOT_MAX_ARGUMENTS) {
            return SHELL_PARSE_TOO_MANY_ARGUMENTS;
        }
        argument = cursor;
        write = cursor;
        while (*cursor != '\0') {
            if (quote != '\0') {
                if (*cursor == quote) {
                    quote = '\0';
                    cursor++;
                } else {
                    *write++ = *cursor++;
                }
            } else if (*cursor == '\'' || *cursor == '"') {
                quote = *cursor++;
            } else if (*cursor == ' ' || *cursor == '\t') {
                break;
            } else {
                *write++ = *cursor++;
            }
        }
        if (*cursor != '\0') cursor++;
        *write = '\0';
        if (!command->name) {
            command->name = argument;
        } else {
            command->arguments[command->argument_count++] = argument;
        }
        cursor = skip_spaces(cursor);
    }
    return SHELL_PARSE_OK;
}

static bool prompt_location(const char *cwd, char *location, usize capacity)
{
    const char *components[32];
    usize lengths[32];
    usize count = 0;
    const char *cursor;
    usize length = 0;

    if (!cwd || !location || capacity == 0) return false;
    cursor = cwd;
    while (*cursor) {
        const char *component;
        while (*cursor == '/') cursor++;
        if (!*cursor) break;
        component = cursor;
        while (*cursor && *cursor != '/') cursor++;
        if (count == sizeof(components) / sizeof(components[0])) return false;
        components[count] = component;
        lengths[count++] = (usize)(cursor - component);
    }

    if (count == 0) {
        if (capacity < 2) return false;
        location[0] = '/';
        location[1] = '\0';
        return true;
    }

    usize first = count > 1 ? count - 2 : count - 1;
    for (usize i = first; i < count; i++) {
        if (i != first) {
            if (length + 1 >= capacity) return false;
            location[length++] = '/';
        }
        if (length + lengths[i] >= capacity) return false;
        memcpy(location + length, components[i], lengths[i]);
        length += lengths[i];
    }
    location[length] = '\0';
    return true;
}

static bool make_prompt(const shell_state_t *state, char *prompt,
                        usize capacity)
{
    char location[256];
    usize location_length;

    if (!state || !prompt || capacity == 0 ||
        !prompt_location(state->cwd, location, sizeof(location))) return false;
    location_length = strlen(location);
    if (location_length + 9 > capacity) return false;
    strcpy(prompt, "shoot ");
    strcpy(prompt + 6, location);
    prompt[6 + location_length] = ':';
    prompt[7 + location_length] = ' ';
    prompt[8 + location_length] = '\0';
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

static bool build_external_path(const char *name, char *path,
                                usize path_capacity)
{
    usize name_length;

    if (!name || !path || path_capacity < 6) return false;
    name_length = strlen(name);
    if (name_length == 0 || name_length + 6 > path_capacity) return false;
    strcpy(path, "/bin/");
    strcpy(path + 5, name);
    return true;
}

static void execute_external(const shell_command_t *command)
{
    mg_result_t child_result;
    mg_result_t wait_result;
    mg_result_t close_result;
    mg_handle_t child;
    mg_path_info_t info;
    char path[256];
    char cmdline[512];
    i32 status = 0;

    if ((strcmp(command->name, "shoot") == 0 || strcmp(command->name, "/bin/shoot") == 0) &&
        command->argument_count == 0) {
        printf("Shoot is already running.\n");
        return;
    }

    if (command->name[0] == '/') {
        strncpy(path, command->name, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        if (!build_external_path(command->name, path, sizeof(path))) {
            printf("Unknown command: %s\n", command->name);
            return;
        }
    }

    if (result_is_error(path_info(path, &info)) || info.type != MG_PATH_TYPE_FILE) {
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

    child_result = process_spawn(cmdline);
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
    mg_line_editor_t editor;
    mg_line_history_t history;

    strcpy(state.cwd, "/");
    line_editor_init(&editor, line, sizeof(line), "");
    line_editor_history_init(&history, &history_entries[0][0],
                             sizeof(history_entries[0]),
                             SHOOT_HISTORY_CAPACITY);
    line_editor_set_history(&editor, &history);

    for (;;) {
        if (!make_prompt(&state, prompt, sizeof(prompt))) process_exit(1);
        if (!read_command(&editor, prompt)) process_exit(0);

        switch (parse_command(line, &command)) {
        case SHELL_PARSE_EMPTY:
            break;
        case SHELL_PARSE_TOO_MANY_ARGUMENTS:
            console_begin_transaction();
            printf("Too many arguments: maximum %u.\n",
                   (unsigned)SHOOT_MAX_ARGUMENTS);
            if (!make_prompt(&state, prompt, sizeof(prompt))) process_exit(1);
            line_editor_set_prompt(&editor, prompt);
            line_editor_prepare_next_prompt(&editor);
            console_end_transaction();
            break;
        case SHELL_PARSE_OK:
            if (find_builtin(command.name)) {
                console_begin_transaction();
                execute_builtin(&state, &command);
                if (!make_prompt(&state, prompt, sizeof(prompt))) process_exit(1);
                line_editor_set_prompt(&editor, prompt);
                line_editor_prepare_next_prompt(&editor);
                console_end_transaction();
            } else {
                execute_external(&command);
                if (!make_prompt(&state, prompt, sizeof(prompt))) process_exit(1);
                line_editor_set_prompt(&editor, prompt);
                editor.prompt_drawn = false;
            }
            break;
        }
    }
}
