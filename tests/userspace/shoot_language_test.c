/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host integration fixture for the production Shoot language and dispatcher. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mangrove.h>
#include <mg/error.h>
#include <mg/filesystem.h>
#include <mg/identity.h>
#include <mg/object.h>
#include <mg/process.h>
#include <userspace/shoot/builtin.h>
#include <userspace/shoot/commands/commands.h>
#include <userspace/shoot/history.h>
#include <userspace/shoot/language.h>
#include <userspace/shoot/script.h>
#include <userspace/shoot/shell.h>

#define MOCK_FILE_COUNT 8U
#define MOCK_HANDLE_COUNT 16U
#define MOCK_FILE_CAPACITY 32768U
#define MOCK_OUTPUT_CAPACITY 32768U

typedef struct mock_file {
    bool exists;
    char path[256];
    char contents[MOCK_FILE_CAPACITY];
    usize length;
} mock_file_t;

typedef struct mock_handle {
    bool active;
    mg_handle_t value;
    mock_file_t *file;
    FILE *host_file;
    const char *memory;
    usize memory_length;
    usize position;
} mock_handle_t;

static mock_file_t mock_files[MOCK_FILE_COUNT];
static mock_handle_t mock_handles[MOCK_HANDLE_COUNT];
static mock_file_t *redirected_output;
static char console_output[MOCK_OUTPUT_CAPACITY];
static usize console_output_length;
static const char *mock_script_data;
static usize mock_script_length;
static usize spawn_count;
static usize legacy_spawn_count;
static usize wait_count;
static usize child_close_count;
static usize file_close_count;
static usize redirect_count;
static usize restore_count;
static usize direct_output_spawn_count;
static i32 next_child_status;
static mg_result_t next_spawn_result;
static char last_argv[10][512];
static usize last_argc;
static char mark_events[32][128];
static usize mark_event_count;
static usize builtin_jump_count;
static usize builtin_exit_count;
static char builtin_jump_argument[256];

bool shoot_history_save(shoot_history_t *history)
{
    (void)history;
    return true;
}

void shoot_history_destroy(shoot_history_t *history)
{
    (void)history;
}

void process_exit(i32 status)
{
    (void)status;
    abort();
}

static mock_file_t *file_find(const char *path)
{
    for (usize index = 0; index < MOCK_FILE_COUNT; index++)
        if (mock_files[index].exists && !strcmp(mock_files[index].path, path))
            return &mock_files[index];
    return NULL;
}

static mock_file_t *file_create_mock(const char *path)
{
    mock_file_t *file = file_find(path);

    if (file) return file;
    for (usize index = 0; index < MOCK_FILE_COUNT; index++) {
        if (mock_files[index].exists) continue;
        file = &mock_files[index];
        memset(file, 0, sizeof(*file));
        file->exists = true;
        assert(strlen(path) < sizeof(file->path));
        strcpy(file->path, path);
        return file;
    }
    return NULL;
}

static mock_handle_t *handle_allocate(void)
{
    for (usize index = 0; index < MOCK_HANDLE_COUNT; index++) {
        if (!mock_handles[index].active) {
            mock_handle_t *handle = &mock_handles[index];
            memset(handle, 0, sizeof(*handle));
            handle->active = true;
            handle->value = (mg_handle_t)(100U + index);
            return handle;
        }
    }
    return NULL;
}

static mock_handle_t *handle_find(mg_handle_t value)
{
    for (usize index = 0; index < MOCK_HANDLE_COUNT; index++)
        if (mock_handles[index].active && mock_handles[index].value == value)
            return &mock_handles[index];
    return NULL;
}

static void reset_observations(void)
{
    redirected_output = NULL;
    console_output_length = 0;
    console_output[0] = '\0';
    spawn_count = 0;
    legacy_spawn_count = 0;
    wait_count = 0;
    child_close_count = 0;
    file_close_count = 0;
    redirect_count = 0;
    restore_count = 0;
    direct_output_spawn_count = 0;
    next_child_status = 0;
    next_spawn_result = MG_OK;
    last_argc = 0;
    mark_event_count = 0;
    builtin_jump_count = 0;
    builtin_exit_count = 0;
    builtin_jump_argument[0] = '\0';
    memset(last_argv, 0, sizeof(last_argv));
}

bool result_is_error(mg_result_t result) { return result < 0; }
const char *error_string(mg_result_t result)
{
    (void)result;
    return "mock error";
}

void shoot_config_defaults(shoot_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->history_enabled = true;
    config->history_persist = true;
    config->history_limit = 256U;
}

bool shoot_config_reload(shoot_config_t *config)
{
    (void)config;
    return true;
}

mg_result_t process_getcwd(char *buffer, usize capacity, usize *out_size)
{
    static const char cwd[] = "/home/developer";
    if (!buffer || capacity < sizeof(cwd)) return MG_ERR_BAD_ARGUMENT;
    memcpy(buffer, cwd, sizeof(cwd));
    if (out_size) *out_size = sizeof(cwd);
    return MG_OK;
}

mg_result_t process_get_identity(mg_identity_t *identity)
{
    if (!identity) return MG_ERR_BAD_ARGUMENT;
    memset(identity, 0, sizeof(*identity));
    identity->uid = 1U;
    strcpy(identity->username, "developer");
    strcpy(identity->home, "/home/developer");
    return MG_OK;
}

mg_result_t path_info(const char *path, mg_path_info_t *out_info)
{
    if (!path || !out_info) return MG_ERR_BAD_ARGUMENT;
    memset(out_info, 0, sizeof(*out_info));
    if (!strncmp(path, "/bin/", 5U)) {
        out_info->type = MG_PATH_TYPE_FILE;
        return MG_OK;
    }
    mock_file_t *file = file_find(path);
    if (!file) return MG_ERR_NOT_FOUND;
    out_info->type = MG_PATH_TYPE_FILE;
    out_info->size = file->length;
    return MG_OK;
}

mg_result_t file_create(const char *path)
{
    return file_create_mock(path) ? MG_OK : MG_ERR_NO_MEMORY;
}

mg_result_t file_open(const char *path, u32 flags)
{
    mock_handle_t *handle;

    if (!strcmp(path, "tests/userspace/shoot_v1.shoot")) {
        FILE *source = fopen(path, "rb");
        if (!source) return MG_ERR_NOT_FOUND;
        handle = handle_allocate();
        if (!handle) {
            fclose(source);
            return MG_ERR_NO_MEMORY;
        }
        handle->host_file = source;
        return (mg_result_t)handle->value;
    }
    if (!strcmp(path, "mock-script")) {
        handle = handle_allocate();
        if (!handle) return MG_ERR_NO_MEMORY;
        handle->memory = mock_script_data;
        handle->memory_length = mock_script_length;
        return (mg_result_t)handle->value;
    }
    mock_file_t *file = file_find(path);
    if (!file || (!(flags & MG_OPEN_READ) && !(flags & MG_OPEN_WRITE)))
        return MG_ERR_NOT_FOUND;
    handle = handle_allocate();
    if (!handle) return MG_ERR_NO_MEMORY;
    handle->file = file;
    return (mg_result_t)handle->value;
}

mg_result_t object_read(mg_handle_t value, void *buffer, usize length)
{
    mock_handle_t *handle = handle_find(value);
    if (!handle || !buffer) return MG_ERR_INVALID_HANDLE;
    if (handle->host_file) {
        size_t count = fread(buffer, 1U, length, handle->host_file);
        if (count == 0 && ferror(handle->host_file)) return MG_ERR_IO;
        return (mg_result_t)count;
    }
    if (handle->memory) {
        usize remaining = handle->memory_length - handle->position;
        usize count = remaining < length ? remaining : length;
        memcpy(buffer, handle->memory + handle->position, count);
        handle->position += count;
        return (mg_result_t)count;
    }
    if (!handle->file) return MG_ERR_INVALID_HANDLE;
    usize remaining = handle->file->length - handle->position;
    usize count = remaining < length ? remaining : length;
    memcpy(buffer, handle->file->contents + handle->position, count);
    handle->position += count;
    return (mg_result_t)count;
}

mg_result_t object_write(mg_handle_t value, const void *buffer, usize length)
{
    mock_handle_t *handle = handle_find(value);
    if (!handle || !handle->file || !buffer) return MG_ERR_INVALID_HANDLE;
    if (handle->position + length > sizeof(handle->file->contents))
        return MG_ERR_BUFFER_TOO_SMALL;
    memcpy(handle->file->contents + handle->position, buffer, length);
    handle->position += length;
    if (handle->file->length < handle->position)
        handle->file->length = handle->position;
    return (mg_result_t)length;
}

mg_result_t file_truncate(mg_handle_t value)
{
    mock_handle_t *handle = handle_find(value);
    if (!handle || !handle->file) return MG_ERR_INVALID_HANDLE;
    handle->file->length = 0;
    handle->position = 0;
    return MG_OK;
}

mg_result_t file_seek(mg_handle_t value, i64 offset, mg_seek_whence_t whence)
{
    mock_handle_t *handle = handle_find(value);
    if (!handle || !handle->file || offset < 0 || whence != MG_SEEK_END)
        return MG_ERR_BAD_ARGUMENT;
    handle->position = handle->file->length + (usize)offset;
    return (mg_result_t)handle->position;
}

mg_result_t handle_close(mg_handle_t value)
{
    mock_handle_t *handle = handle_find(value);
    if (handle) {
        if (handle->host_file) fclose(handle->host_file);
        handle->active = false;
        file_close_count++;
        return MG_OK;
    }
    if (value >= 700U) {
        child_close_count++;
        return MG_OK;
    }
    return MG_ERR_INVALID_HANDLE;
}

mg_result_t console_begin_transaction(void) { return MG_OK; }
mg_result_t console_end_transaction(void) { return MG_OK; }
mg_result_t console_write_string(const char *text)
{
    usize length = strlen(text);
    if (length > sizeof(console_output) - console_output_length - 1U)
        return MG_ERR_BUFFER_TOO_SMALL;
    memcpy(console_output + console_output_length, text, length);
    console_output_length += length;
    console_output[console_output_length] = '\0';
    return MG_OK;
}

static bool output_bytes(const char *text, usize length)
{
    if (redirected_output) {
        if (length > sizeof(redirected_output->contents) -
                     redirected_output->length) return false;
        memcpy(redirected_output->contents + redirected_output->length,
               text, length);
        redirected_output->length += length;
        return true;
    }
    if (length > sizeof(console_output) - console_output_length - 1U)
        return false;
    memcpy(console_output + console_output_length, text, length);
    console_output_length += length;
    console_output[console_output_length] = '\0';
    return true;
}

mg_result_t process_redirect_output(mg_handle_t output_handle,
                                    mg_handle_t *saved_handle)
{
    mock_handle_t *handle = handle_find(output_handle);
    if (!handle || !handle->file || !saved_handle || redirected_output)
        return MG_ERR_INVALID_HANDLE;
    redirected_output = handle->file;
    *saved_handle = 999U;
    redirect_count++;
    return MG_OK;
}

mg_result_t process_restore_output(mg_handle_t saved_handle)
{
    if (saved_handle != 999U || !redirected_output)
        return MG_ERR_INVALID_HANDLE;
    redirected_output = NULL;
    restore_count++;
    return MG_OK;
}

mg_result_t process_spawn_argv(const char *const *argv, usize argc)
{
    assert(argv && argc > 0U && argc <= 9U);
    spawn_count++;
    last_argc = argc;
    for (usize index = 0; index < argc; index++) {
        assert(argv[index]);
        assert(strlen(argv[index]) < sizeof(last_argv[index]));
        strcpy(last_argv[index], argv[index]);
    }
    if (next_spawn_result < 0) return next_spawn_result;

    if (!strcmp(argv[0], "/bin/say")) {
        char output[2048];
        usize used = 0;
        for (usize index = 1; index < argc; index++) {
            if (index > 1U) output[used++] = ' ';
            usize length = strlen(argv[index]);
            assert(length < sizeof(output) - used - 1U);
            memcpy(output + used, argv[index], length);
            used += length;
        }
        output[used++] = '\n';
        assert(output_bytes(output, used));
    } else if (!strcmp(argv[0], "/bin/wait")) {
        assert(argc == 2U && !strcmp(argv[1], "1"));
    } else if (!strcmp(argv[0], "/bin/type")) {
        assert(argc == 2U);
        mock_file_t *file = file_find(argv[1]);
        assert(file && output_bytes(file->contents, file->length));
    } else if (!strcmp(argv[0], "/bin/mark")) {
        assert(argc == 2U && mark_event_count < 32U);
        assert(strlen(argv[1]) < sizeof(mark_events[0]));
        strcpy(mark_events[mark_event_count++], argv[1]);
    }
    return (mg_result_t)(700U + spawn_count);
}

mg_result_t process_spawn_argv_with_output(const char *const *argv,
                                           usize argc,
                                           mg_handle_t output_handle)
{
    mock_handle_t *handle = handle_find(output_handle);
    mock_file_t *previous_output = redirected_output;
    mg_result_t result;

    if (!handle || !handle->file) return MG_ERR_INVALID_HANDLE;
    direct_output_spawn_count++;
    redirected_output = handle->file;
    result = process_spawn_argv(argv, argc);
    redirected_output = previous_output;
    return result;
}

mg_result_t process_spawn(const char *command_line)
{
    (void)command_line;
    legacy_spawn_count++;
    return (mg_result_t)(700U + ++spawn_count);
}

mg_result_t process_spawn_with_output(const char *command_line,
                                      mg_handle_t output_handle)
{
    (void)command_line;
    (void)output_handle;
    legacy_spawn_count++;
    return (mg_result_t)(700U + ++spawn_count);
}

mg_result_t process_wait(mg_handle_t child, i32 *status)
{
    assert(child >= 700U && status);
    wait_count++;
    *status = next_child_status;
    return MG_OK;
}

bool execute_jump(shell_state_t *state, const shell_command_t *command)
{
    (void)state;
    builtin_jump_count++;
    if (command->argument_count) {
        assert(strlen(command->arguments[0]) < sizeof(builtin_jump_argument));
        strcpy(builtin_jump_argument, command->arguments[0]);
    }
    return true;
}
bool execute_help(shell_state_t *state, const shell_command_t *command)
{ (void)state; (void)command; return true; }
bool execute_reload(shell_state_t *state, const shell_command_t *command)
{ (void)state; (void)command; return true; }
bool execute_history(shell_state_t *state, const shell_command_t *command)
{ (void)state; (void)command; return true; }
bool execute_exit(shell_state_t *state, const shell_command_t *command)
{
    (void)state;
    builtin_exit_count++;
    assert(command->argument_count <= 1U);
    return true;
}

static void test_tokenizer_expansion_and_invalid_input(shell_state_t *state)
{
    shell_command_t command;
    const char *line = "say\t\"Hello $name\" '$name' \"\" pre\"fix\"post // comment";

    assert(shell_try_assignment(state, "name = Mangrove") ==
           SHELL_ASSIGNMENT_OK);
    assert(shell_parse_command(state, line, &command) == SHELL_PARSE_OK);
    assert(!strcmp(command.name, "say"));
    assert(command.argument_count == 4U);
    assert(!strcmp(command.arguments[0], "Hello Mangrove"));
    assert(!strcmp(command.arguments[1], "$name"));
    assert(!strcmp(command.arguments[2], ""));
    assert(!strcmp(command.arguments[3], "prefixpost"));

    assert(shell_parse_command(state, "say 'unterminated", &command) ==
           SHELL_PARSE_INVALID_SYNTAX);
    assert(shell_parse_command(state, "say >", &command) ==
           SHELL_PARSE_INVALID_SYNTAX);
    assert(shell_parse_command(state, "say > one > two", &command) ==
           SHELL_PARSE_INVALID_SYNTAX);
    assert(shell_parse_command(state, "say word//comment", &command) ==
           SHELL_PARSE_OK);
    assert(command.argument_count == 1U &&
           !strcmp(command.arguments[0], "word"));
    assert(shell_parse_command(state, "say \"left\\right\"",
                               &command) == SHELL_PARSE_OK);
    assert(command.argument_count == 1U &&
           !strcmp(command.arguments[0], "left\\right"));
}

static void test_assignments_and_conditions(shell_state_t *state)
{
    bool value = false;

    assert(shell_try_assignment(state, "count = 1") == SHELL_ASSIGNMENT_OK);
    assert(shell_try_assignment(state, "count = $count + 1") ==
           SHELL_ASSIGNMENT_OK);
    assert(!strcmp(state->variables[1].value, "2"));
    assert(shell_try_assignment(state,
        "count = 9223372036854775807 + 1") == SHELL_ASSIGNMENT_INVALID);
    assert(!strcmp(state->variables[1].value, "2"));
    assert(shell_try_assignment(state, "bad-name = 3") ==
           SHELL_ASSIGNMENT_NOT_PRESENT);

    assert(shell_evaluate_condition(state, "$name == 'Mangrove'", &value));
    assert(value);
    assert(shell_evaluate_condition(state, "2 < 3", &value) && value);
    assert(shell_evaluate_condition(state, "3 <= 3", &value) && value);
    assert(shell_evaluate_condition(state, "4 > 3", &value) && value);
    assert(shell_evaluate_condition(state, "4 >= 4", &value) && value);
    assert(!shell_evaluate_condition(state, "4 != 4", &value));

    mock_file_t *file = file_create_mock("/temp/present");
    assert(file);
    assert(shell_evaluate_condition(state, "exists /temp/present", &value));
    assert(value);
    assert(shell_evaluate_condition(state, "exists /temp/absent", &value));
    assert(!value);
}

static void test_external_argv_redirection_and_failure(shell_state_t *state)
{
    mock_file_t *file;

    reset_observations();
    assert(shell_execute_line(state,
        "say 'quote\" and space' \"\" > /temp/quoted.txt",
        NULL, 0, false));
    file = file_find("/temp/quoted.txt");
    assert(file && file->length == strlen("quote\" and space \n"));
    assert(!memcmp(file->contents, "quote\" and space \n", file->length));
    assert(spawn_count == 1U && legacy_spawn_count == 0U);
    assert(last_argc == 3U && !strcmp(last_argv[0], "/bin/say"));
    assert(!strcmp(last_argv[1], "quote\" and space"));
    assert(!strcmp(last_argv[2], ""));
    assert(direct_output_spawn_count == 1U);
    assert(redirect_count == 0U && restore_count == 0U);
    assert(!redirected_output && file_close_count == 1U);
    assert(wait_count == 1U && child_close_count == 1U);

    file = file_create_mock("/temp/truncate.txt");
    assert(file);
    strcpy(file->contents, "stale contents\n");
    file->length = strlen(file->contents);
    assert(shell_execute_line(state,
        "say replacement > /temp/truncate.txt", NULL, 0, false));
    assert(file->length == strlen("replacement\n"));
    assert(!memcmp(file->contents, "replacement\n", file->length));

    mock_file_t *outer_output = file_create_mock("/temp/outer-output.txt");
    assert(outer_output);
    redirected_output = outer_output;
    reset_observations();
    redirected_output = outer_output;
    assert(shell_execute_line(state,
        "say nested > /temp/nested-output.txt", NULL, 0, false));
    file = file_find("/temp/nested-output.txt");
    assert(file && file->length == strlen("nested\n"));
    assert(!memcmp(file->contents, "nested\n", file->length));
    assert(outer_output->length == 0U && redirected_output == outer_output);
    redirected_output = NULL;

    reset_observations();
    next_child_status = 17;
    assert(!shell_execute_line(state, "say failed", NULL, 0, false));
    assert(wait_count == 1U && child_close_count == 1U);
    next_child_status = 0;
    assert(shell_execute_line(state, "say recovered", NULL, 0, false));

    reset_observations();
    next_spawn_result = MG_ERR_NOT_FOUND;
    assert(!shell_execute_line(state,
        "say no-child > /temp/spawn-failure.txt", NULL, 0, false));
    assert(direct_output_spawn_count == 1U);
    assert(redirect_count == 0U && restore_count == 0U);
    assert(file_close_count == 1U && wait_count == 0U &&
           child_close_count == 0U && !redirected_output);
}

static void test_builtin_arity_and_recovery(shell_state_t *state)
{
    assert(find_builtin("cd") == NULL);
    assert(find_builtin("jump") != NULL);
    assert(!shell_execute_line(state, "jump one two", NULL, 0, false));
    assert(builtin_jump_count == 0U);
    assert(shell_execute_line(state, "jump /temp", NULL, 0, false));
    assert(builtin_jump_count == 1U &&
           !strcmp(builtin_jump_argument, "/temp"));

    assert(!shell_execute_line(state, "say 'bad", NULL, 0, false));
    reset_observations();
    assert(shell_execute_line(state, "say still-running", NULL, 0, false));
    assert(spawn_count == 1U && wait_count == 1U);
}

static void append_script(shell_script_t *script, const char *line,
                          usize line_number)
{
    const char *error = NULL;
    assert(shell_script_append(script, line, line_number, &error));
}

static void test_script_blocks_and_failure_stops(shell_state_t *state)
{
    shell_script_t script;
    assert(shell_script_init(&script));
    append_script(&script, "count = 1", 1U);
    append_script(&script, "if $count == 1", 2U);
    append_script(&script, "mark chosen", 3U);
    append_script(&script, "else", 4U);
    append_script(&script, "mark wrong", 5U);
    append_script(&script, "end", 6U);
    append_script(&script, "while $count < 3", 7U);
    append_script(&script, "mark loop", 8U);
    append_script(&script, "count = $count + 1", 9U);
    append_script(&script, "end", 10U);
    assert(shell_script_complete(&script));
    reset_observations();
    assert(shell_script_execute(&script, state, "host-test"));
    assert(mark_event_count == 3U);
    assert(!strcmp(mark_events[0], "chosen"));
    assert(!strcmp(mark_events[1], "loop"));
    assert(!strcmp(mark_events[2], "loop"));
    shell_script_destroy(&script);

    assert(shell_script_init(&script));
    append_script(&script, "mark before", 1U);
    append_script(&script, "jump one two", 2U);
    append_script(&script, "mark after", 3U);
    reset_observations();
    assert(!shell_script_execute(&script, state, "host-test"));
    assert(mark_event_count == 1U && !strcmp(mark_events[0], "before"));
    shell_script_destroy(&script);
}

static void test_existing_shoot_script(void)
{
    static const char expected[] =
        "System report\n"
        "=============\n"
        "Literal expansion test: $name\n"
        "Expanded value: Mangrove\n"
        "string comparison: pass\n"
        "numeric comparison: pass\n"
        "round 1\n"
        "round 2\n"
        "round 3\n";

    reset_observations();
    assert(shell_run_script("tests/userspace/shoot_v1.shoot") == 0);
    mock_file_t *report = file_find("/temp/shoot-report.txt");
    assert(report && report->length == sizeof(expected) - 1U);
    assert(!memcmp(report->contents, expected, sizeof(expected) - 1U));
    assert(spawn_count == 15U && wait_count == 15U &&
           child_close_count == 15U);
    static const char console_prefix[] =
        "Shoot scripting test for Mangrove\nreport created\n";
    assert(console_output_length == sizeof(console_prefix) - 1U +
                                     sizeof(expected) - 1U);
    assert(!memcmp(console_output, console_prefix,
                   sizeof(console_prefix) - 1U));
    assert(!memcmp(console_output + sizeof(console_prefix) - 1U,
                   expected, sizeof(expected) - 1U));
    assert(builtin_exit_count == 1U);
}

static int run_mock_script(const char *data, usize length)
{
    mock_script_data = data;
    mock_script_length = length;
    return shell_run_script("mock-script");
}

static void test_script_source_line_boundary(void)
{
    char *source = malloc(SHOOT_MAX_SCRIPT_SOURCE_LINES + 1U);
    assert(source);
    memset(source, '\n', SHOOT_MAX_SCRIPT_SOURCE_LINES + 1U);
    assert(run_mock_script(source, SHOOT_MAX_SCRIPT_SOURCE_LINES) == 0);
    assert(run_mock_script(source, SHOOT_MAX_SCRIPT_SOURCE_LINES + 1U) != 0);
    free(source);
    mock_script_data = NULL;
    mock_script_length = 0;
}

int main(void)
{
    shell_state_t state;
    memset(&state, 0, sizeof(state));
    assert(shell_state_init(&state));
    test_tokenizer_expansion_and_invalid_input(&state);
    test_assignments_and_conditions(&state);
    test_external_argv_redirection_and_failure(&state);
    test_builtin_arity_and_recovery(&state);
    test_script_blocks_and_failure_stops(&state);
    test_existing_shoot_script();
    test_script_source_line_boundary();
    puts("Shoot: parser, execution, script, redirection and failure regressions passed");
    return 0;
}
