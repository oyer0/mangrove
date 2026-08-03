#include <mg/error.h>
#include <mg/filesystem.h>
#include <mg/line_editor.h>
#include <mg/object.h>
#include <mg/process.h>
#include <stdio.h>
#include <string.h>

#define SHOOT_LINE_CAPACITY 256U
#define SHOOT_HISTORY_CAPACITY 32U
#define SHELL_MAX_ARGUMENTS 8U

typedef struct shell_command {
    char *name;
    char *arguments[SHELL_MAX_ARGUMENTS];
    usize argument_count;
} shell_command_t;

typedef enum shell_parse_result {
    SHELL_PARSE_EMPTY,
    SHELL_PARSE_OK,
    SHELL_PARSE_TOO_MANY_ARGUMENTS,
} shell_parse_result_t;

typedef struct shell_state {
    char cwd[256];
} shell_state_t;

typedef bool (*shell_builtin_handler_t)(shell_state_t *state,
                                        const shell_command_t *command);

typedef struct shell_builtin {
    const char *name;
    const char *usage;
    const char *description;
    const char *help;
    usize minimum_arguments;
    usize maximum_arguments;
    shell_builtin_handler_t handler;
} shell_builtin_t;

typedef struct shell_external {
    const char *name;
    const char *usage;
    const char *description;
    const char *help;
    bool visible_in_help;
} shell_external_t;

static const shell_builtin_t shell_builtins[];
static const shell_external_t shell_externals[];

static bool execute_help(shell_state_t *state,
                         const shell_command_t *command);
static bool execute_jump(shell_state_t *state,
                         const shell_command_t *command);
static bool execute_exit(shell_state_t *state,
                         const shell_command_t *command);
static bool execute_list(shell_state_t *state,
                         const shell_command_t *command);
static bool execute_read(shell_state_t *state,
                         const shell_command_t *command);
static bool execute_plant(shell_state_t *state,
                          const shell_command_t *command);
static bool execute_remove(shell_state_t *state,
                           const shell_command_t *command);
static bool execute_move(shell_state_t *state,
                         const shell_command_t *command);
static bool execute_where(shell_state_t *state,
                          const shell_command_t *command);
static bool execute_locate(shell_state_t *state,
                           const shell_command_t *command);
static const shell_builtin_t *find_builtin(const char *name);
static const shell_external_t *find_external(const char *name);

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

        if (command->argument_count == SHELL_MAX_ARGUMENTS) {
            return SHELL_PARSE_TOO_MANY_ARGUMENTS;
        }
        argument = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
            cursor++;
        }
        if (*cursor != '\0') *cursor++ = '\0';
        if (!command->name) {
            command->name = argument;
        } else {
            command->arguments[command->argument_count++] = argument;
        }
        cursor = skip_spaces(cursor);
    }
    return SHELL_PARSE_OK;
}

static bool build_canonical_path(const char *cwd, const char *input,
                                 char *output, usize output_size)
{
    char raw[512];
    char *components[32];
    usize top = 0;
    usize length;
    const char *base;
    char *cursor;

    if (!cwd || !input || !output || output_size < 2 ||
        strlen(input) >= 256) return false;

    raw[0] = '\0';
    if (input[0] != '/') {
        base = cwd[0] ? cwd : "/";
        strncpy(raw, base, sizeof(raw) - 1);
        raw[sizeof(raw) - 1] = '\0';
        length = strlen(raw);
        if (length > 0 && raw[length - 1] != '/') {
            if (length + 1 >= sizeof(raw)) return false;
            raw[length++] = '/';
            raw[length] = '\0';
        }
        if (length + strlen(input) >= sizeof(raw)) return false;
        strcpy(raw + length, input);
    } else {
        if (strlen(input) >= sizeof(raw)) return false;
        strcpy(raw, input);
    }

    cursor = raw;
    while (*cursor) {
        char *component;
        while (*cursor == '/') cursor++;
        if (!*cursor) break;
        component = cursor;
        while (*cursor && *cursor != '/') cursor++;
        if (*cursor) *cursor++ = '\0';
        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            if (top > 0) top--;
            continue;
        }
        if (top == sizeof(components) / sizeof(components[0])) return false;
        components[top++] = component;
    }

    output[0] = '/';
    output[1] = '\0';
    length = 1;
    for (usize i = 0; i < top; i++) {
        usize component_length = strlen(components[i]);
        if (length > 1) {
            if (length + 1 >= output_size) return false;
            output[length++] = '/';
        }
        if (length + component_length >= output_size) return false;
        memcpy(output + length, components[i], component_length);
        length += component_length;
        output[length] = '\0';
    }
    return true;
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

static const shell_builtin_t shell_builtins[] = {
    {"help", "help [command]", "show available commands",
     "Show available commands or detailed help for one command.\n"
     "Examples: help, help jump",
     0, 1, execute_help},
    {"jump", "jump <path>", "change the current directory",
     "Change Shoot's current directory.\n"
     "Example: jump /bin",
     1, 1, execute_jump},
    {"exit", "exit", "leave Shoot",
     "Exit the current Shoot session.", 0, 0, execute_exit},
    {"list", "list [path]", "list directory contents",
     "List the contents of a directory.\n"
     "Directories are shown with a trailing slash.\n"
     "Examples: list, list /, list /bin",
     0, 1, execute_list},
    {"read", "read <file>", "print file contents",
     "Print the contents of a text file.\n"
     "Example: read notes.txt",
     1, 1, execute_read},
    {"plant", "plant <name>", "create an empty file",
     "Create a new empty file.\n"
     "Example: plant notes.txt",
     1, 1, execute_plant},
    {"remove", "remove <path>", "remove a file or empty directory",
     "Remove a file or an empty directory.\n"
     "Non-empty directories cannot be removed.\n"
     "Example: remove notes.txt",
     1, 1, execute_remove},
    {"move", "move <source> <destination>", "rename or move a file",
     "Rename or move a file or directory.\n"
     "Example: move old.txt new.txt",
     2, 2, execute_move},
    {"where", "where", "print the current directory",
     "Print the full path of the current working directory.",
     0, 0, execute_where},
    {"locate", "locate <name>", "show how a command resolves",
     "Show the executable path Shoot would use for a command.\n"
     "Example: locate hello",
     1, 1, execute_locate},
};

static const shell_external_t shell_externals[] = {
    {"hello", "hello", "run the Hello validation program",
     "Run the Hello validation program from /bin.\n"
     "Example: hello", true},
    {"fstest", "fstest", "filesystem API validation",
     "Temporary filesystem API validation program.", false},
};

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

static bool execute_help(shell_state_t *state, const shell_command_t *command)
{
    const shell_builtin_t *topic;

    (void)state;
    if (!command || command->argument_count == 0) {
        puts("Available commands:");
        for (usize i = 0; i < sizeof(shell_builtins) /
                             sizeof(shell_builtins[0]); i++) {
            printf("  %s - %s\n", shell_builtins[i].usage,
                   shell_builtins[i].description);
        }
        for (usize i = 0; i < sizeof(shell_externals) /
                             sizeof(shell_externals[0]); i++) {
            if (!shell_externals[i].visible_in_help) continue;
            printf("  %s - %s\n", shell_externals[i].name,
                   shell_externals[i].description);
        }
        return true;
    }

    topic = find_builtin(command->arguments[0]);
    if (topic) {
        printf("%s\nUsage: %s\n%s\n", topic->description,
               topic->usage, topic->help);
        return true;
    }
    const shell_external_t *external =
        find_external(command->arguments[0]);
    if (external) {
        printf("%s\nUsage: %s\n%s\n", external->description,
               external->usage, external->help);
        return true;
    }
    {
        printf("Unknown help topic: %s\n", command->arguments[0]);
        return true;
    }
}

static bool execute_jump(shell_state_t *state,
                         const shell_command_t *command)
{
    mg_result_t result;
    char new_cwd[256];
    const char *path;

    if (!state || !command || command->argument_count != 1 ||
        !command->arguments[0] || *command->arguments[0] == '\0') {
        puts("Usage: jump <path>");
        return true;
    }
    path = command->arguments[0];
    if (!build_canonical_path(state->cwd, path, new_cwd,
                              sizeof(new_cwd))) {
        printf("Could not jump to \"%s\": invalid path.\n", path);
        return true;
    }
    result = process_chdir(path);
    if (result_is_error(result)) {
        printf("Could not jump to \"%s\": %s.\n", path,
               error_string(result));
        return true;
    }
    strcpy(state->cwd, new_cwd);
    puts("Directory changed.");
    return true;
}

static void execute_external_process(const shell_external_t *external)
{
    mg_result_t child_result;
    mg_result_t wait_result;
    mg_result_t close_result;
    mg_handle_t child;
    char path[256];
    i32 status = 0;

    if (!build_external_path(external->name, path, sizeof(path))) {
        printf("Could not run \"%s\": invalid executable name.\n",
               external->name);
        return;
    }
    child_result = process_spawn(path);
    if (result_is_error(child_result)) {
        printf("Could not run \"%s\": %s.\n", external->name,
               error_string(child_result));
        return;
    }
    child = (mg_handle_t)child_result;
    wait_result = process_wait(child, &status);
    close_result = handle_close(child);
    if (result_is_error(wait_result)) {
        printf("Could not wait for \"%s\": %s.\n", external->name,
               error_string(wait_result));
        return;
    }
    if (result_is_error(close_result)) {
        printf("Could not close \"%s\": %s.\n", external->name,
               error_string(close_result));
        return;
    }
    if (status != 0) {
        printf("Could not run \"%s\": exited with status %d.\n",
               external->name, status);
        return;
    }
}

static bool execute_exit(shell_state_t *state, const shell_command_t *command)
{
    (void)state;
    (void)command;
    process_exit(0);
}

static bool execute_list(shell_state_t *state, const shell_command_t *command)
{
    const char *target;
    char resolved[256];
    mg_result_t result;
    mg_handle_t dir;
    mg_directory_entry_t entry;

    if (command->argument_count == 0) {
        target = state->cwd;
    } else {
        target = command->arguments[0];
        if (target[0] != '/') {
            if (!build_canonical_path(state->cwd, target, resolved,
                                      sizeof(resolved))) {
                printf("Could not list \"%s\": invalid path.\n", target);
                return true;
            }
            target = resolved;
        }
    }

    result = directory_open(target);
    if (result_is_error(result)) {
        printf("Could not list \"%s\": %s.\n",
               command->argument_count == 0 ? "." : command->arguments[0],
               error_string(result));
        return true;
    }
    dir = (mg_handle_t)result;

    for (;;) {
        result = directory_read(dir, &entry);
        if (result == MG_ERR_END_OF_FILE) break;
        if (result_is_error(result)) {
            printf("Could not read directory: %s.\n", error_string(result));
            break;
        }
        if (entry.type == MG_PATH_TYPE_DIRECTORY) {
            printf("%s/\n", entry.name);
        } else {
            puts(entry.name);
        }
    }

    handle_close(dir);
    return true;
}

static bool execute_read(shell_state_t *state, const shell_command_t *command)
{
    const char *target;
    char resolved[256];
    mg_result_t result;
    mg_handle_t file;
    char buffer[512];

    target = command->arguments[0];
    if (target[0] != '/') {
        if (!build_canonical_path(state->cwd, target, resolved,
                                  sizeof(resolved))) {
            printf("Could not read \"%s\": invalid path.\n", target);
            return true;
        }
        target = resolved;
    }

    result = file_open(target, MG_OPEN_READ);
    if (result_is_error(result)) {
        printf("Could not read \"%s\".\n", command->arguments[0]);
        return true;
    }
    file = (mg_handle_t)result;

    for (;;) {
        result = object_read(file, buffer, sizeof(buffer) - 1);
        if (result == MG_ERR_END_OF_FILE || result == 0) break;
        if (result_is_error(result)) {
            printf("\nRead error: %s.\n", error_string(result));
            break;
        }
        buffer[result] = '\0';
        printf("%s", buffer);
    }

    handle_close(file);
    return true;
}

static bool execute_plant(shell_state_t *state, const shell_command_t *command)
{
    const char *target;
    char resolved[256];
    mg_result_t result;

    target = command->arguments[0];
    if (target[0] != '/') {
        if (!build_canonical_path(state->cwd, target, resolved,
                                  sizeof(resolved))) {
            printf("Could not create \"%s\": invalid path.\n", target);
            return true;
        }
        target = resolved;
    }

    result = file_create(target);
    if (result == MG_ERR_ALREADY_EXISTS) {
        printf("Could not create \"%s\": already exists.\n",
               command->arguments[0]);
        return true;
    }
    if (result_is_error(result)) {
        printf("Could not create \"%s\": %s.\n", command->arguments[0],
               error_string(result));
        return true;
    }
    return true;
}

static bool execute_remove(shell_state_t *state, const shell_command_t *command)
{
    const char *target;
    char resolved[256];
    mg_result_t result;

    target = command->arguments[0];
    if (target[0] != '/') {
        if (!build_canonical_path(state->cwd, target, resolved,
                                  sizeof(resolved))) {
            printf("Could not remove \"%s\": invalid path.\n", target);
            return true;
        }
        target = resolved;
    }

    result = path_remove(target);
    if (result == MG_ERR_NOT_EMPTY) {
        printf("Could not remove \"%s\": directory is not empty.\n",
               command->arguments[0]);
        return true;
    }
    if (result_is_error(result)) {
        printf("Could not remove \"%s\": %s.\n", command->arguments[0],
               error_string(result));
        return true;
    }
    return true;
}

static bool execute_move(shell_state_t *state, const shell_command_t *command)
{
    char source[256];
    char destination[256];
    const char *src;
    const char *dst;
    mg_result_t result;

    src = command->arguments[0];
    dst = command->arguments[1];

    if (src[0] != '/') {
        if (!build_canonical_path(state->cwd, src, source, sizeof(source))) {
            printf("Could not move \"%s\": invalid path.\n", src);
            return true;
        }
        src = source;
    }
    if (dst[0] != '/') {
        if (!build_canonical_path(state->cwd, dst, destination,
                                  sizeof(destination))) {
            printf("Could not move to \"%s\": invalid path.\n",
                   command->arguments[1]);
            return true;
        }
        dst = destination;
    }

    result = path_move(src, dst);
    if (result_is_error(result)) {
        printf("Could not move \"%s\" to \"%s\": %s.\n",
               command->arguments[0], command->arguments[1],
               error_string(result));
        return true;
    }
    return true;
}

static bool execute_where(shell_state_t *state, const shell_command_t *command)
{
    (void)command;
    puts(state->cwd);
    return true;
}

static bool execute_locate(shell_state_t *state,
                           const shell_command_t *command)
{
    char path[256];
    mg_path_info_t info;
    mg_result_t result;

    (void)state;
    if (!build_external_path(command->arguments[0], path, sizeof(path))) {
        puts("not found");
        return true;
    }
    result = path_info(path, &info);
    if (result_is_error(result) || info.type != MG_PATH_TYPE_FILE) {
        puts("not found");
        return true;
    }
    puts(path);
    return true;
}

static const shell_builtin_t *find_builtin(const char *name)
{
    for (usize i = 0; i < sizeof(shell_builtins) / sizeof(shell_builtins[0]); i++) {
        if (strcmp(shell_builtins[i].name, name) == 0) return &shell_builtins[i];
    }
    return NULL;
}

static const shell_external_t *find_external(const char *name)
{
    for (usize i = 0; i < sizeof(shell_externals) / sizeof(shell_externals[0]); i++) {
        if (strcmp(shell_externals[i].name, name) == 0) return &shell_externals[i];
    }
    return NULL;
}

static bool command_arity_is_valid(const char *usage, usize count,
    usize minimum, usize maximum)
{
    if (count >= minimum && count <= maximum) return true;
    printf("Usage: %s\n", usage);
    return false;
}

static bool execute_builtin(shell_state_t *state,
                            const shell_command_t *command)
{
    const shell_builtin_t *builtin = find_builtin(command->name);

    if (!builtin) return false;
    if (!command_arity_is_valid(builtin->usage,
                                command->argument_count,
                                builtin->minimum_arguments,
                                builtin->maximum_arguments)) return true;
    return builtin->handler(state, command);
}

static void execute_external(const shell_command_t *command)
{
    const shell_external_t *external = find_external(command->name);

    if (!external) {
        printf("Unknown command: %s\n", command->name);
        return;
    }
    if (!command_arity_is_valid(external->name,
                                command->argument_count, 0, 0)) return;
    execute_external_process(external);
}

static void shell_loop(void)
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
            continue;
        case SHELL_PARSE_TOO_MANY_ARGUMENTS:
            printf("Too many arguments: maximum %u.\n",
                   (unsigned)SHELL_MAX_ARGUMENTS);
            continue;
        case SHELL_PARSE_OK:
            break;
        }
        if (!execute_builtin(&state, &command)) execute_external(&command);
    }
}

int main(void)
{
    shell_loop();
    process_exit(0);
}
