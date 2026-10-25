/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host integration fixture for the production Shoot language and dispatcher. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

#define MOCK_FILE_COUNT 80U
#define MOCK_DIRECTORY_COUNT 16U
#define MOCK_HANDLE_COUNT 16U
#define MOCK_DIRECTORY_HANDLE_COUNT 8U
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

typedef struct mock_directory_handle {
    bool active;
    mg_handle_t value;
    char path[256];
    usize cursor;
} mock_directory_handle_t;

static mock_file_t mock_files[MOCK_FILE_COUNT];
static char mock_directories[MOCK_DIRECTORY_COUNT][256];
static usize mock_directory_count;
static char mock_cwd[256] = "/home/developer";
static mock_handle_t mock_handles[MOCK_HANDLE_COUNT];
static mock_directory_handle_t mock_directory_handles[
    MOCK_DIRECTORY_HANDLE_COUNT];
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
static i32 spawned_child_status;
static mg_result_t next_spawn_result;
static char last_argv[MG_PROCESS_MAX_ARGUMENTS][512];
static usize last_argc;
static char mark_events[32][128];
static usize mark_event_count;
static usize builtin_jump_count;
static usize builtin_exit_count;
static char builtin_jump_argument[256];
static usize directory_read_count;
static usize directory_fail_after = (usize)-1;
static usize glob_allocation_count;
static usize glob_fail_at = (usize)-1;
static usize glob_free_count;
static const char *path_info_disappear_path;

int del_command_main(int argc, char **argv);

void *shoot_glob_test_malloc(size_t size)
{
    if (glob_allocation_count++ == glob_fail_at) return NULL;
    return malloc(size);
}

void shoot_glob_test_free(void *pointer)
{
    if (pointer) glob_free_count++;
    free(pointer);
}

static void fail_glob_allocation_after(usize count)
{
    glob_allocation_count = 0;
    glob_free_count = 0;
    glob_fail_at = count;
}

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

static bool mock_path_exists(const char *path)
{
    mg_path_info_t info;
    return path_info(path, &info) == MG_OK;
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

static void directory_create_mock(const char *path)
{
    for (usize index = 0; index < mock_directory_count; index++)
        if (!strcmp(mock_directories[index], path)) return;
    assert(mock_directory_count < MOCK_DIRECTORY_COUNT);
    assert(strlen(path) < sizeof(mock_directories[0]));
    strcpy(mock_directories[mock_directory_count++], path);
}

static bool mock_path_resolve(const char *input, char output[256])
{
    usize length;

    if (!input || !*input) return false;
    if (input[0] == '/') {
        length = strlen(input);
        if (length >= 256U) return false;
        memcpy(output, input, length + 1U);
    } else if (!strcmp(input, ".")) {
        strcpy(output, mock_cwd);
    } else {
        int written = snprintf(output, 256U, "%s/%s", mock_cwd, input);
        if (written < 0 || written >= 256) return false;
    }
    length = strlen(output);
    while (length > 1U && output[length - 1U] == '/')
        output[--length] = '\0';
    return true;
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

static mock_directory_handle_t *directory_handle_find(mg_handle_t value)
{
    for (usize index = 0; index < MOCK_DIRECTORY_HANDLE_COUNT; index++)
        if (mock_directory_handles[index].active &&
            mock_directory_handles[index].value == value)
            return &mock_directory_handles[index];
    return NULL;
}

static usize active_directory_handle_count(void)
{
    usize count = 0;
    for (usize index = 0; index < MOCK_DIRECTORY_HANDLE_COUNT; index++)
        if (mock_directory_handles[index].active) count++;
    return count;
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
    spawned_child_status = 0;
    next_spawn_result = MG_OK;
    last_argc = 0;
    mark_event_count = 0;
    builtin_jump_count = 0;
    builtin_exit_count = 0;
    builtin_jump_argument[0] = '\0';
    directory_read_count = 0;
    directory_fail_after = (usize)-1;
    path_info_disappear_path = NULL;
    memset(last_argv, 0, sizeof(last_argv));
}

bool result_is_error(mg_result_t result) { return result < 0; }
const char *error_string(mg_result_t result)
{
    (void)result;
    return "mock error";
}

bool command_help_requested(int argc, char **argv)
{
    return argc == 2 && argv && argv[1] &&
        (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"));
}

int command_print_help(const char *argv0)
{
    (void)argv0;
    return 0;
}

void command_usage_error(const char *argv0, const char *usage,
                         const char *option)
{
    (void)argv0;
    (void)usage;
    (void)option;
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
    usize length = strlen(mock_cwd) + 1U;
    if (!buffer || capacity < length) return MG_ERR_BAD_ARGUMENT;
    memcpy(buffer, mock_cwd, length);
    if (out_size) *out_size = length;
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
    char resolved[256];
    if (!path || !out_info) return MG_ERR_BAD_ARGUMENT;
    if (path_info_disappear_path &&
        !strcmp(path, path_info_disappear_path)) return MG_ERR_NOT_FOUND;
    memset(out_info, 0, sizeof(*out_info));
    if (!strcmp(path, "/bin/command-that-does-not-exist"))
        return MG_ERR_NOT_FOUND;
    if (!strncmp(path, "/bin/", 5U)) {
        out_info->type = MG_PATH_TYPE_FILE;
        return MG_OK;
    }
    if (!mock_path_resolve(path, resolved)) return MG_ERR_NAME_TOO_LONG;
    mock_file_t *file = file_find(resolved);
    if (file) {
        out_info->type = MG_PATH_TYPE_FILE;
        out_info->size = file->length;
        return MG_OK;
    }
    if (!strcmp(resolved, "/") || !strcmp(resolved, "/home") ||
        !strcmp(resolved, "/home/developer")) {
        out_info->type = MG_PATH_TYPE_DIRECTORY;
        return MG_OK;
    }
    for (usize index = 0; index < mock_directory_count; index++) {
        if (!strcmp(mock_directories[index], resolved)) {
            out_info->type = MG_PATH_TYPE_DIRECTORY;
            return MG_OK;
        }
    }
    return MG_ERR_NOT_FOUND;
}

mg_result_t directory_open(const char *path)
{
    char resolved[256];
    mg_path_info_t info;
    if (!mock_path_resolve(path, resolved)) return MG_ERR_NAME_TOO_LONG;
    mg_result_t result = path_info(resolved, &info);
    if (result_is_error(result)) return result;
    if (info.type != MG_PATH_TYPE_DIRECTORY) return MG_ERR_NOT_DIRECTORY;
    for (usize index = 0; index < MOCK_DIRECTORY_HANDLE_COUNT; index++) {
        mock_directory_handle_t *handle = &mock_directory_handles[index];
        if (handle->active) continue;
        memset(handle, 0, sizeof(*handle));
        handle->active = true;
        handle->value = (mg_handle_t)(300U + index);
        strcpy(handle->path, resolved);
        return (mg_result_t)handle->value;
    }
    return MG_ERR_NO_MEMORY;
}

static bool mock_direct_child(const char *directory, const char *path,
                              const char **out_name)
{
    usize parent_length = strlen(directory);
    const char *name;
    bool root = parent_length == 1U && directory[0] == '/';

    if (strncmp(directory, path, parent_length) != 0) return false;
    if (root) {
        if (path[0] != '/' || !path[1]) return false;
        name = path + 1U;
    } else {
        if (path[parent_length] != '/') return false;
        name = path + parent_length + 1U;
    }
    if (!*name || strchr(name, '/')) return false;
    *out_name = name;
    return true;
}

mg_result_t directory_read(mg_handle_t value, mg_directory_entry_t *entry)
{
    mock_directory_handle_t *handle = directory_handle_find(value);
    usize total = MOCK_FILE_COUNT + mock_directory_count + 2U;

    if (!handle || !entry) return MG_ERR_INVALID_HANDLE;
    if (directory_read_count >= directory_fail_after) return MG_ERR_IO;
    while (handle->cursor < total) {
        usize index = handle->cursor++;
        const char *path;
        u32 type;
        const char *name;
        if (index < 2U) {
            memset(entry, 0, sizeof(*entry));
            strcpy(entry->name, index == 0U ? "." : "..");
            entry->type = MG_PATH_TYPE_DIRECTORY;
            directory_read_count++;
            return MG_OK;
        }
        index -= 2U;
        if (index < MOCK_FILE_COUNT) {
            if (!mock_files[index].exists) continue;
            path = mock_files[index].path;
            type = MG_PATH_TYPE_FILE;
        } else {
            usize dir_index = index - MOCK_FILE_COUNT;
            path = mock_directories[dir_index];
            type = MG_PATH_TYPE_DIRECTORY;
        }
        if (!mock_direct_child(handle->path, path, &name)) continue;
        if (strlen(name) >= sizeof(entry->name)) return MG_ERR_NAME_TOO_LONG;
        memset(entry, 0, sizeof(*entry));
        strcpy(entry->name, name);
        entry->type = type;
        directory_read_count++;
        return MG_OK;
    }
    return MG_ERR_END_OF_FILE;
}

mg_result_t directory_read_batch(mg_handle_t value,
                                 mg_directory_entry_t *entries,
                                 usize capacity, usize *out_count)
{
    if (!entries || !capacity || !out_count) return MG_ERR_BAD_ARGUMENT;
    *out_count = 0U;
    while (*out_count < capacity) {
        mg_directory_entry_t entry;
        mg_result_t result = directory_read(value, &entry);
        if (result == MG_ERR_END_OF_FILE)
            return *out_count ? MG_OK : MG_ERR_END_OF_FILE;
        if (result_is_error(result)) return result;
        /* The production batch API omits the synthetic dot entries that the
         * single-entry shell iterator exposes. */
        if (!strcmp(entry.name, ".") || !strcmp(entry.name, "..")) continue;
        entries[(*out_count)++] = entry;
    }
    return MG_OK;
}

mg_result_t path_remove(const char *path)
{
    char resolved[256];
    if (!mock_path_resolve(path, resolved)) return MG_ERR_NAME_TOO_LONG;
    if (!strcmp(resolved, "/home/developer/del-fixture/.protected"))
        return MG_ERR_ACCESS_DENIED;
    mock_file_t *file = file_find(resolved);
    if (file) {
        file->exists = false;
        return MG_OK;
    }
    for (usize index = 0; index < mock_directory_count; index++) {
        if (strcmp(mock_directories[index], resolved)) continue;
        for (usize child = 0; child < MOCK_FILE_COUNT; child++) {
            const char *name;
            if (mock_files[child].exists &&
                mock_direct_child(resolved, mock_files[child].path, &name))
                return MG_ERR_NOT_EMPTY;
        }
        for (usize child = 0; child < mock_directory_count; child++) {
            const char *name;
            if (mock_directories[child][0] &&
                mock_direct_child(resolved, mock_directories[child], &name))
                return MG_ERR_NOT_EMPTY;
        }
        mock_directories[index][0] = '\0';
        return MG_OK;
    }
    return MG_ERR_NOT_FOUND;
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
    mock_directory_handle_t *directory = directory_handle_find(value);
    if (directory) {
        directory->active = false;
        return MG_OK;
    }
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
    assert(argv && argc > 0U && argc <= MG_PROCESS_MAX_ARGUMENTS);
    spawned_child_status = 0;
    spawn_count++;
    last_argc = argc;
    for (usize index = 0; index < argc; index++) {
        assert(argv[index]);
        assert(strlen(argv[index]) < sizeof(last_argv[index]));
        strcpy(last_argv[index], argv[index]);
    }
    if (next_spawn_result < 0) return next_spawn_result;

    if (!strcmp(argv[0], "/bin/del")) {
        char *mutable_argv[MG_PROCESS_MAX_ARGUMENTS];
        for (usize index = 0; index < argc; index++)
            mutable_argv[index] = (char *)argv[index];
        spawned_child_status = del_command_main((int)argc, mutable_argv);
    } else if (!strcmp(argv[0], "/bin/say")) {
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
    } else if (!strcmp(argv[0], "/bin/capture")) {
        /* Captures the largest legal argv without synthesizing child output. */
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
    *status = next_child_status ? next_child_status : spawned_child_status;
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
    assert(shell_parse_command(state, "say hello\\ world!", &command) ==
           SHELL_PARSE_OK);
    assert(command.argument_count == 1U &&
           !strcmp(command.arguments[0], "hello world!"));
    assert(shell_parse_command(state, "say \\~/literal", &command) ==
           SHELL_PARSE_OK);
    assert(command.argument_count == 1U &&
           !strcmp(command.arguments[0], "~/literal") &&
           !command.argument_home_expand[0]);
    assert(shell_parse_command(state, "say trailing\\", &command) ==
           SHELL_PARSE_INVALID_SYNTAX);
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
           SHELL_ASSIGNMENT_INVALID);
    assert(shell_try_assignment(state,
        "averyverylongidentifierthatisdefinitelytoo_long") ==
           SHELL_ASSIGNMENT_NOT_PRESENT);
    assert(shell_try_assignment(state,
        "averyverylongidentifierthatisdefinitelytoo_long = value") ==
        SHELL_ASSIGNMENT_INVALID);

    {
        char long_command[302];
        memset(long_command, 'x', sizeof(long_command) - 2U);
        long_command[sizeof(long_command) - 2U] = '\0';
        assert(shell_try_assignment(state, long_command) ==
               SHELL_ASSIGNMENT_NOT_PRESENT);
        reset_observations();
        assert(!shell_execute_line(state, long_command, NULL, 0, false));
        assert(spawn_count == 0U);
    }

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

static void expect_say_arguments(const char *const *expected, usize count)
{
    assert(last_argc == count + 1U);
    assert(!strcmp(last_argv[0], "/bin/say"));
    for (usize index = 0; index < count; index++)
        assert(!strcmp(last_argv[index + 1U], expected[index]));
}

static void add_glob_file(const char *name)
{
    char path[256];
    int length = snprintf(path, sizeof(path), "/home/developer/glob/%s", name);
    assert(length > 0 && (usize)length < sizeof(path));
    assert(file_create_mock(path));
}

static void test_pathname_expansion(shell_state_t *state)
{
    static const char long_directory_prefix[] = "/home/developer/";
    static const char *const sorted_text[] = {
        "glob/a.txt", "glob/b.txt", "glob/café.txt",
        "glob/hello world!.txt", "glob/star*.txt", "glob/z.txt"
    };
    static const char *const bracket_text[] = {"glob/a.txt", "glob/b.txt"};
    static const char *const range_text[] = {"glob/a.txt", "glob/b.txt"};
    static const char *const negated_text[] = {"glob/b.txt", "glob/z.txt"};
    static const char *const hidden_text[] = {
        "glob/.hidden.txt", "glob/.vault"
    };
    static const char *const unicode_text[] = {"glob/café.txt"};
    static const char *const nested_text[] = {"glob/sub/nested.tmp"};
    static const char *const del_text[] = {
        "empty", "hello world!", "nested", "one"
    };

    directory_create_mock("/home/developer/glob");
    directory_create_mock("/home/developer/glob/sub");
    directory_create_mock("/home/developer/glob/sub/deeper");
    directory_create_mock("/home/developer/glob/.vault");
    char long_directory[MG_PATH_MAX_BYTES + 1U];
    usize long_prefix_length = sizeof(long_directory_prefix) - 1U;
    memcpy(long_directory, long_directory_prefix, long_prefix_length);
    memset(long_directory + long_prefix_length, 'p',
           MG_PATH_MAX_BYTES - long_prefix_length);
    long_directory[MG_PATH_MAX_BYTES] = '\0';
    directory_create_mock(long_directory);
    add_glob_file("z.txt");
    add_glob_file("café.txt");
    add_glob_file("b.txt");
    add_glob_file("a.txt");
    add_glob_file(".hidden.txt");
    add_glob_file("hello world!.txt");
    add_glob_file("a1");
    add_glob_file("a2");
    add_glob_file("abc");
    add_glob_file("ac");
    add_glob_file("star*.txt");
    add_glob_file("a1.cfg");
    add_glob_file("sub/nested.tmp");
    add_glob_file("sub/deeper/deep.tmp");
    for (unsigned index = 0; index < 16U; index++) {
        char name[32];
        (void)snprintf(name, sizeof(name), "batch%02u", index);
        add_glob_file(name);
    }
    directory_create_mock("/home/developer/maxargs");
    for (unsigned index = 0; index < SHOOT_MAX_ARGUMENTS; index++) {
        char name[221];
        char path[256];
        (void)snprintf(name, 4U, "%02u", index);
        memset(name + 2U, 'x', sizeof(name) - 3U);
        name[sizeof(name) - 1U] = '\0';
        int length = snprintf(path, sizeof(path),
                              "/home/developer/maxargs/%s", name);
        assert(length > 0 && (usize)length < sizeof(path));
        assert(file_create_mock(path));
    }
    directory_create_mock("/h");
    for (unsigned index = 0; index < SHOOT_MAX_ARGUMENTS; index++) {
        char name[253];
        char path[256];
        name[0] = (char)('0' + index / 10U);
        name[1] = (char)('0' + index % 10U);
        memset(name + 2U, 'y', sizeof(name) - 3U);
        name[sizeof(name) - 1U] = '\0';
        int length = snprintf(path, sizeof(path), "/h/%s", name);
        assert(length > 0 && (usize)length < sizeof(path));
        assert((usize)length == MG_PATH_MAX_BYTES);
        assert(file_create_mock(path));
    }
    char long_executable[MG_PATH_MAX_BYTES + 1U];
    strcpy(long_executable, "/exec/");
    memset(long_executable + strlen(long_executable), 'e',
           MG_PATH_MAX_BYTES - strlen("/exec/"));
    long_executable[MG_PATH_MAX_BYTES] = '\0';
    assert(file_create_mock(long_executable));
    directory_create_mock("/home/developer/del-fixture");
    directory_create_mock("/home/developer/del-fixture/empty");
    directory_create_mock("/home/developer/del-fixture/nested");
    assert(file_create_mock("/home/developer/del-fixture/one"));
    assert(file_create_mock("/home/developer/del-fixture/hello world!"));
    assert(file_create_mock("/home/developer/del-fixture/delete.tmp"));
    assert(file_create_mock("/home/developer/del-fixture/.protected"));
    assert(file_create_mock("/home/developer/del-fixture/nested/keep"));

    reset_observations();
    assert(shell_execute_line(state, "say glob/*.txt", NULL, 0, false));
    expect_say_arguments(sorted_text,
                         sizeof(sorted_text) / sizeof(sorted_text[0]));

    reset_observations();
    assert(shell_execute_line(state, "say glob/[ab].txt", NULL, 0, false));
    expect_say_arguments(bracket_text,
                         sizeof(bracket_text) / sizeof(bracket_text[0]));

    reset_observations();
    assert(shell_execute_line(state, "say glob/[a-b].txt", NULL, 0, false));
    expect_say_arguments(range_text,
                         sizeof(range_text) / sizeof(range_text[0]));

    reset_observations();
    assert(shell_execute_line(state, "say glob/[!a].txt", NULL, 0, false));
    expect_say_arguments(negated_text,
                         sizeof(negated_text) / sizeof(negated_text[0]));

    reset_observations();
    assert(shell_execute_line(state, "say glob/caf?.txt", NULL, 0, false));
    expect_say_arguments(unicode_text,
                         sizeof(unicode_text) / sizeof(unicode_text[0]));

    reset_observations();
    assert(shell_execute_line(state, "say glob/*/*.tmp", NULL, 0, false));
    expect_say_arguments(nested_text,
                         sizeof(nested_text) / sizeof(nested_text[0]));

    reset_observations();
    assert(shell_execute_line(state, "capture glob/*/", NULL, 0, false));
    assert(last_argc == 2U && !strcmp(last_argv[1], "glob/sub/"));

    reset_observations();
    assert(shell_execute_line(state, "say glob/**/deep.tmp",
                              NULL, 0, false));
    assert(last_argc == 2U &&
           !strcmp(last_argv[1], "glob/**/deep.tmp"));

    reset_observations();
    assert(shell_execute_line(state,
        "say /home/developer/glob/[ab].txt glob/caf?.txt",
        NULL, 0, false));
    assert(last_argc == 4U &&
           !strcmp(last_argv[1], "/home/developer/glob/a.txt") &&
           !strcmp(last_argv[2], "/home/developer/glob/b.txt") &&
           !strcmp(last_argv[3], "glob/café.txt"));

    reset_observations();
    assert(shell_execute_line(state, "say glob/.*", NULL, 0, false));
    expect_say_arguments(hidden_text,
                         sizeof(hidden_text) / sizeof(hidden_text[0]));
    for (usize index = 1; index < last_argc; index++) {
        assert(strcmp(last_argv[index], "glob/.") != 0);
        assert(strcmp(last_argv[index], "glob/..") != 0);
    }

    reset_observations();
    assert(!shell_execute_line(state, "say /home/developer/*/",
                               NULL, 0, false));
    assert(spawn_count == 0U);

    reset_observations();
    assert(shell_execute_line(state, "say glob/a?", NULL, 0, false));
    assert(last_argc == 4U && !strcmp(last_argv[1], "glob/a1") &&
           !strcmp(last_argv[2], "glob/a2") &&
           !strcmp(last_argv[3], "glob/ac"));

    reset_observations();
    assert(shell_execute_line(state, "jump ~/glob/a?.cfg",
                              NULL, 0, false));
    assert(builtin_jump_count == 1U &&
           !strcmp(builtin_jump_argument,
                   "/home/developer/glob/a1.cfg"));

    reset_observations();
    assert(shell_execute_line(state, "capture maxargs/*", NULL, 0, false));
    assert(last_argc == MG_PROCESS_MAX_ARGUMENTS);
    usize argument_bytes = 0;
    for (usize index = 0; index < last_argc; index++)
        argument_bytes += strlen(last_argv[index]) + 1U;
    assert(argument_bytes <= MG_PROCESS_ARGUMENT_BYTES_MAX);

    reset_observations();
    char long_argv_line[512];
    int long_line_length = snprintf(long_argv_line, sizeof(long_argv_line),
                                    "%s /h/* > /temp/oversized-argv.txt",
                                    long_executable);
    assert(long_line_length > 0 &&
           (usize)long_line_length < sizeof(long_argv_line));
    assert(!shell_execute_line(state, long_argv_line, NULL, 0, false));
    assert(spawn_count == 0U &&
           !file_find("/temp/oversized-argv.txt"));

    reset_observations();
    assert(shell_execute_line(state, "say 'glob/*.txt'", NULL, 0, false));
    assert(last_argc == 2U && !strcmp(last_argv[1], "glob/*.txt"));

    reset_observations();
    assert(shell_execute_line(state, "say glob/\\*.txt", NULL, 0, false));
    assert(last_argc == 2U && !strcmp(last_argv[1], "glob/*.txt"));

    reset_observations();
    assert(shell_execute_line(state, "say glob/star\\*.txt",
                              NULL, 0, false));
    assert(last_argc == 2U && !strcmp(last_argv[1], "glob/star*.txt"));

    reset_observations();
    assert(shell_execute_line(state, "say \"glob/*.txt\"", NULL, 0, false));
    assert(last_argc == 2U && !strcmp(last_argv[1], "glob/*.txt"));

    reset_observations();
    assert(shell_try_assignment(state, "wildcard = glob/*.txt") ==
           SHELL_ASSIGNMENT_OK);
    assert(shell_execute_line(state, "say $wildcard", NULL, 0, false));
    expect_say_arguments(sorted_text,
                         sizeof(sorted_text) / sizeof(sorted_text[0]));

    reset_observations();
    assert(shell_execute_line(state, "say \"$wildcard\"", NULL, 0, false));
    assert(last_argc == 2U && !strcmp(last_argv[1], "glob/*.txt"));

    reset_observations();
    assert(shell_execute_line(state, "say glob/no-match*.missing",
                              NULL, 0, false));
    assert(last_argc == 2U &&
           !strcmp(last_argv[1], "glob/no-match*.missing"));

    reset_observations();
    assert(shell_execute_line(state, "say glob/a.txt/*.txt",
                              NULL, 0, false));
    assert(last_argc == 2U &&
           !strcmp(last_argv[1], "glob/a.txt/*.txt"));

    reset_observations();
    assert(!shell_execute_line(state, "say glob/[abc", NULL, 0, false));
    assert(spawn_count == 0U);

    reset_observations();
    assert(!shell_execute_line(state,
        "say glob/*.txt glob/[abc", NULL, 0, false));
    assert(spawn_count == 0U);

    reset_observations();
    assert(!shell_execute_line(state,
        "say glob/*.txt glob/[abc > /temp/partial-glob.txt",
        NULL, 0, false));
    assert(spawn_count == 0U && !file_find("/temp/partial-glob.txt"));

    reset_observations();
    directory_fail_after = 3U;
    assert(!shell_execute_line(state, "say glob/*.txt", NULL, 0, false));
    assert(spawn_count == 0U && active_directory_handle_count() == 0U);
    directory_fail_after = (usize)-1;

    reset_observations();
    add_glob_file("race.txt");
    path_info_disappear_path = "glob/race.txt";
    assert(!shell_execute_line(state, "say glob/race*", NULL, 0, false));
    assert(spawn_count == 0U && active_directory_handle_count() == 0U);
    path_info_disappear_path = NULL;

    reset_observations();
    fail_glob_allocation_after(0U);
    assert(!shell_execute_line(state, "say glob/*.txt", NULL, 0, false));
    assert(spawn_count == 0U);
    glob_fail_at = (usize)-1;

    reset_observations();
    fail_glob_allocation_after(2U);
    assert(!shell_execute_line(state, "say glob/*.txt", NULL, 0, false));
    assert(spawn_count == 0U && glob_free_count >= 2U &&
           active_directory_handle_count() == 0U);
    glob_fail_at = (usize)-1;

    reset_observations();
    assert(!shell_execute_line(state, "say glob/batch*", NULL, 0, false));
    assert(spawn_count == 0U);

    strcpy(mock_cwd, "/home/developer/del-fixture");
    strcpy(state->cwd, mock_cwd);
    reset_observations();
    assert(shell_execute_line(state, "del *.tmp", NULL, 0, false));
    assert(last_argc == 2U && !strcmp(last_argv[1], "delete.tmp"));
    assert(!file_find("/home/developer/del-fixture/delete.tmp"));

    /* Exercise the actual del program after Shoot has expanded a mixed
     * non-hidden directory listing. Nonempty directories fail without -r,
     * while files and empty directories are still removed. */
    reset_observations();
    assert(!shell_execute_line(state, "del *", NULL, 0, false));
    assert(last_argc == 5U && !strcmp(last_argv[0], "/bin/del"));
    for (usize index = 1; index < last_argc; index++)
        assert(strstr(last_argv[index], ".protected") == NULL);
    for (usize index = 0; index < sizeof(del_text) / sizeof(del_text[0]); index++) {
        bool found = false;
        for (usize arg = 1; arg < last_argc; arg++)
            if (!strcmp(last_argv[arg], del_text[index])) found = true;
        assert(found);
    }
    assert(!mock_path_exists("empty"));
    assert(!file_find("/home/developer/del-fixture/hello world!"));
    assert(!file_find("/home/developer/del-fixture/one"));
    assert(file_find("/home/developer/del-fixture/.protected"));
    assert(file_find("/home/developer/del-fixture/nested/keep"));

    reset_observations();
    assert(shell_execute_line(state, "del -r *", NULL, 0, false));
    assert(last_argc == 3U && !strcmp(last_argv[0], "/bin/del") &&
           !strcmp(last_argv[1], "-r") &&
           !strcmp(last_argv[2], "nested"));
    assert(!file_find("/home/developer/del-fixture/nested/keep"));
    assert(!mock_path_exists("nested"));
    assert(file_find("/home/developer/del-fixture/.protected"));

    reset_observations();
    assert(!shell_execute_line(state, "del .*", NULL, 0, false));
    assert(last_argc == 2U &&
           !strcmp(last_argv[1], ".protected"));
    strcpy(mock_cwd, "/home/developer");
    strcpy(state->cwd, mock_cwd);
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

    reset_observations();
    assert(shell_execute_line(state,
        "reed hello\\ world!", NULL, 0, false));
    assert(spawn_count == 1U && last_argc == 2U);
    assert(!strcmp(last_argv[0], "/bin/reed"));
    assert(!strcmp(last_argv[1], "hello world!"));

    reset_observations();
    assert(shell_execute_line(state,
        "reed \"hello world!\"", NULL, 0, false));
    assert(spawn_count == 1U && last_argc == 2U);
    assert(!strcmp(last_argv[0], "/bin/reed"));
    assert(!strcmp(last_argv[1], "hello world!"));

    reset_observations();
    assert(shell_execute_line(state,
        "say hello\\ world! \"hello world!\" three\\ words\\? \"left\\right\" \\$name",
        NULL, 0, false));
    assert(last_argc == 6U);
    assert(!strcmp(last_argv[1], "hello world!"));
    assert(!strcmp(last_argv[2], "hello world!"));
    assert(!strcmp(last_argv[3], "three words?"));
    assert(!strcmp(last_argv[4], "left\\right"));
    assert(!strcmp(last_argv[5], "$name"));

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
    FILE *capture = tmpfile();
    assert(capture);
    fflush(stdout);
    int saved_stdout = dup(STDOUT_FILENO);
    assert(saved_stdout >= 0 && dup2(fileno(capture), STDOUT_FILENO) >= 0);
    assert(!shell_execute_line(state, "say failed", NULL, 0, false));
    fflush(stdout);
    assert(fseek(capture, 0, SEEK_END) == 0);
    long captured_length = ftell(capture);
    assert(captured_length == 0L);
    assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    close(saved_stdout);
    fclose(capture);
    assert(wait_count == 1U && child_close_count == 1U);
    next_child_status = 0;
    assert(shell_execute_line(state, "say recovered", NULL, 0, false));

    reset_observations();
    next_spawn_result = MG_ERR_NOT_FOUND;
    FILE *spawn_failure_capture = tmpfile();
    assert(spawn_failure_capture);
    fflush(stdout);
    int saved_spawn_stdout = dup(STDOUT_FILENO);
    assert(saved_spawn_stdout >= 0 &&
           dup2(fileno(spawn_failure_capture), STDOUT_FILENO) >= 0);
    assert(!shell_execute_line(state,
        "say no-child > /temp/spawn-failure.txt", NULL, 0, false));
    fflush(stdout);
    assert(fseek(spawn_failure_capture, 0, SEEK_SET) == 0);
    char spawn_failure_output[256];
    usize spawn_failure_length = fread(spawn_failure_output, 1U,
        sizeof(spawn_failure_output) - 1U, spawn_failure_capture);
    spawn_failure_output[spawn_failure_length] = '\0';
    assert(dup2(saved_spawn_stdout, STDOUT_FILENO) >= 0);
    close(saved_spawn_stdout);
    fclose(spawn_failure_capture);
    assert(strstr(spawn_failure_output, "Could not run \"say\":") != NULL);
    assert(direct_output_spawn_count == 1U);
    assert(redirect_count == 0U && restore_count == 0U);
    assert(file_close_count == 1U && wait_count == 0U &&
           child_close_count == 0U && !redirected_output);
}

static void test_command_diagnostic_classification(shell_state_t *state)
{
    char long_command[302];
    char output[1024];
    FILE *capture = tmpfile();
    int saved_stdout;

    assert(capture);
    memset(long_command, 'x', sizeof(long_command) - 2U);
    long_command[sizeof(long_command) - 2U] = '\0';
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    assert(saved_stdout >= 0 && dup2(fileno(capture), STDOUT_FILENO) >= 0);
    assert(!shell_execute_line(state, long_command, NULL, 0, false));
    assert(!shell_execute_line(state, "command-that-does-not-exist",
                               NULL, 0, false));
    assert(!shell_execute_line(state, "bad-name = value", NULL, 0, false));
    assert(!shell_execute_line(state, "jump one two", NULL, 0, false));
    fflush(stdout);
    assert(fseek(capture, 0, SEEK_SET) == 0);
    usize length = fread(output, 1U, sizeof(output) - 1U, capture);
    output[length] = '\0';
    assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    close(saved_stdout);
    fclose(capture);

    assert(strstr(output, "command name is too long") != NULL);
    assert(strstr(output, "Unknown command: command-that-does-not-exist") != NULL);
    assert(strstr(output, "invalid assignment") != NULL);
    assert(strstr(output, "Usage: jump [path]") != NULL);
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

    assert(shell_script_init(&script));
    append_script(&script, "mark before", 1U);
    append_script(&script, "say child-failure", 2U);
    append_script(&script, "mark after", 3U);
    reset_observations();
    next_child_status = 17;
    assert(!shell_script_execute(&script, state, "host-test"));
    assert(mark_event_count == 1U && !strcmp(mark_events[0], "before"));
    next_child_status = 0;
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
    test_pathname_expansion(&state);
    test_external_argv_redirection_and_failure(&state);
    test_command_diagnostic_classification(&state);
    test_builtin_arity_and_recovery(&state);
    test_script_blocks_and_failure_stops(&state);
    test_existing_shoot_script();
    test_script_source_line_boundary();
    puts("Shoot: parser, execution, script, redirection and failure regressions passed");
    return 0;
}
