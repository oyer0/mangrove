/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <string.h>
#include <mg/filesystem.h>
#include <mg/identity.h>
#include <mg/object.h>
#include "../../userspace/shoot/config.h"

#define FILE_CAP 16384U
static mg_identity_t identity;
static struct { char path[256]; char data[FILE_CAP]; usize length; bool exists; } files[2];
static usize file_count;
static struct { char path[256]; bool exists; } directories[4];
static usize directory_count;
static usize directory_creates;
static usize file_creates;
static bool short_read;
static mg_result_t create_result;
static mg_result_t open_result;
static mg_result_t close_result;
static bool fail_next_allocation;
static usize opened_file;
static usize opened_offset;

void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size)
{
    if (fail_next_allocation) {
        fail_next_allocation = false;
        return NULL;
    }
    return __real_malloc(size);
}

bool result_is_error(mg_result_t result) { return result < 0; }
const char *error_string(mg_result_t result)
{
    if (result == MG_ERR_ACCESS_DENIED) return "access denied";
    if (result == MG_ERR_IO) return "I/O failure";
    if (result == MG_ERR_NO_MEMORY) return "out of memory";
    return "controlled filesystem error";
}

mg_result_t process_get_identity(mg_identity_t *out)
{
    if (!out) return MG_ERR_BAD_ARGUMENT;
    *out = identity;
    return MG_OK;
}

mg_result_t process_getcwd(char *buffer, usize capacity, usize *out_size)
{
    static const char working_directory[] = "/";
    if (!buffer || capacity < sizeof(working_directory))
        return MG_ERR_BAD_ARGUMENT;
    memcpy(buffer, working_directory, sizeof(working_directory));
    if (out_size) *out_size = sizeof(working_directory);
    return MG_OK;
}

static int find_file(const char *path)
{
    for (usize i = 0; i < file_count; i++)
        if (files[i].exists && !strcmp(files[i].path, path)) return (int)i;
    return -1;
}

mg_result_t path_info(const char *path, mg_path_info_t *info)
{
    if (!path || !info) return MG_ERR_BAD_ARGUMENT;
    if (!strcmp(path, identity.home)) {
        *info = (mg_path_info_t){.type=MG_PATH_TYPE_DIRECTORY,
                                .owner_uid=identity.uid};
        return MG_OK;
    }
    for (usize i = 0; i < directory_count; i++)
        if (directories[i].exists && !strcmp(directories[i].path, path)) {
            *info = (mg_path_info_t){.type=MG_PATH_TYPE_DIRECTORY,
                                    .owner_uid=identity.uid};
            return MG_OK;
        }
    int index = find_file(path);
    if (index < 0) return MG_ERR_NOT_FOUND;
    *info = (mg_path_info_t){.type=MG_PATH_TYPE_FILE,
                            .size=files[index].length,
                            .owner_uid=identity.uid};
    return MG_OK;
}

mg_result_t directory_create(const char *path)
{
    assert(directory_count < sizeof(directories) / sizeof(directories[0]));
    (void)strcpy(directories[directory_count].path, path);
    directories[directory_count++].exists = true;
    directory_creates++;
    return MG_OK;
}

mg_result_t file_open(const char *path, u32 flags)
{
    (void)flags;
    if (open_result < 0) return open_result;
    int index = find_file(path);
    if (index < 0) return MG_ERR_NOT_FOUND;
    opened_file = (usize)index;
    opened_offset = 0;
    return (mg_result_t)(index + 1);
}

mg_result_t object_read(mg_handle_t handle, void *buffer, usize length)
{
    assert(handle == opened_file + 1U);
    usize available = files[opened_file].length - opened_offset;
    if (!available || short_read) return MG_ERR_END_OF_FILE;
    if (length > available) length = available;
    memcpy(buffer, files[opened_file].data + opened_offset, length);
    opened_offset += length;
    return (mg_result_t)length;
}

mg_result_t handle_close(mg_handle_t handle)
{
    assert(handle == opened_file + 1U || handle == 90U);
    return close_result;
}

mg_result_t file_create_contents(const char *path, const void *data,
                                 usize length, mg_handle_t *out_handle)
{
    file_creates++;
    if (create_result < 0) return create_result;
    if (find_file(path) >= 0) return MG_ERR_ALREADY_EXISTS;
    assert(file_count < sizeof(files) / sizeof(files[0]));
    usize index = file_count++;
    (void)strcpy(files[index].path, path);
    assert(length < sizeof(files[index].data));
    memcpy(files[index].data, data, length);
    files[index].length = length;
    files[index].exists = true;
    *out_handle = 90U;
    return MG_OK;
}

static void setup(void)
{
    memset(&identity, 0, sizeof(identity));
    identity.uid = 42U;
    strcpy(identity.username, "tester");
    strcpy(identity.home, "/home/tester");
    memset(files, 0, sizeof(files));
    memset(directories, 0, sizeof(directories));
    file_count = directory_count = directory_creates = file_creates = 0;
    short_read = false;
    create_result = MG_OK;
    open_result = MG_OK;
    close_result = MG_OK;
    fail_next_allocation = false;
}

static void seed(const char *path, const char *text)
{
    usize index = file_count++;
    assert(index < sizeof(files) / sizeof(files[0]));
    strcpy(files[index].path, path);
    files[index].length = strlen(text);
    memcpy(files[index].data, text, files[index].length);
    files[index].exists = true;
}

static void check_value(const shoot_config_t *config,
                        mg_terminal_color_t prompt)
{
    assert(config->prompt_color == prompt);
}

int main(void)
{
    shoot_config_t config;
    shoot_config_t unchanged;
    char diagnostic[512];
    const char valid[] = " prompt_color = blue\n\n// kept comments\n"
                         "history_enabled=false\ncompletion_case_sensitive=false\n";
    const char *invalid[] = {
        "unknown=true\nprompt_color=red\n",
        "prompt_color=blue\nhistory_limit=12\nhistory_limit=13\n",
        "prompt_color=blue\nhistory_enabled=true\nunknown=value\n",
        "prompt_color=purple\n",
        "history_limit=4097\n",
        "history_enabled=true\nhistory_enabled=false\n",
        "history_enabled true\n",
        "=true\n",
    };
    const char *legacy = "/home/tester/.shoot/config";
    const char *current = "/home/tester/.config/shoot/config";

    setup();
    memset(&config, 0, sizeof(config));
    assert(shoot_config_reload(&config, diagnostic, sizeof(diagnostic)));
    check_value(&config, MG_TERMINAL_COLOR_GREEN);
    assert(directory_creates == 0U && file_creates == 0U);

    setup(); seed(legacy, "prompt_color=blue\nhistory_limit=77\n");
    memset(&config, 0, sizeof(config));
    assert(shoot_config_reload(&config, diagnostic, sizeof(diagnostic)));
    check_value(&config, MG_TERMINAL_COLOR_BLUE);
    assert(config.history_limit == 77U);
    assert(find_file(legacy) >= 0 && find_file(current) >= 0);
    assert(!memcmp(files[find_file(legacy)].data,
                   files[find_file(current)].data,
                   files[find_file(legacy)].length));
    assert(directory_creates == 2U && file_creates == 1U);
    assert(shoot_config_reload(&config, diagnostic, sizeof(diagnostic)));
    assert(directory_creates == 2U && file_creates == 1U);

    setup(); seed(legacy, "prompt_color=blue\n");
    seed(current, "prompt_color=red\n");
    memset(&config, 0, sizeof(config));
    assert(shoot_config_reload(&config, diagnostic, sizeof(diagnostic)));
    check_value(&config, MG_TERMINAL_COLOR_RED);
    assert(file_creates == 0U);
    assert(!strcmp(files[find_file(legacy)].data, "prompt_color=blue\n"));

    setup(); seed(current, "prompt_color=cyan\n");
    memset(&config, 0, sizeof(config));
    assert(shoot_config_reload(&config, diagnostic, sizeof(diagnostic)));
    check_value(&config, MG_TERMINAL_COLOR_CYAN);
    assert(directory_creates == 0U && file_creates == 0U);

    setup(); seed(legacy, "prompt_color=purple\n");
    memset(&config, 0, sizeof(config));
    config.prompt_color = MG_TERMINAL_COLOR_YELLOW;
    assert(!shoot_config_reload(&config, diagnostic, sizeof(diagnostic)));
    check_value(&config, MG_TERMINAL_COLOR_GREEN);
    assert(find_file(current) < 0);
    assert(strstr(diagnostic, legacy) != NULL &&
           strstr(diagnostic, "line 1") != NULL &&
           strstr(diagnostic, "built-in defaults") != NULL);

    setup(); seed(current, "");
    assert(shoot_config_reload(&config, diagnostic, sizeof(diagnostic)));
    check_value(&config, MG_TERMINAL_COLOR_GREEN);
    assert(config.history_limit == 256U && diagnostic[0] == '\0');

    setup(); seed(current, "prompt_color=blue\n");
    short_read = true;
    memset(&config, 0, sizeof(config));
    config.prompt_color = MG_TERMINAL_COLOR_YELLOW;
    assert(!shoot_config_reload(&config, diagnostic, sizeof(diagnostic)));
    check_value(&config, MG_TERMINAL_COLOR_GREEN);
    assert(strstr(diagnostic, current) != NULL &&
           strstr(diagnostic, "I/O failure") != NULL);

    setup(); seed(current, "prompt_color=blue\n");
    open_result = MG_ERR_ACCESS_DENIED;
    assert(!shoot_config_reload(&config, diagnostic, sizeof(diagnostic)));
    check_value(&config, MG_TERMINAL_COLOR_GREEN);
    assert(strstr(diagnostic, current) != NULL &&
           strstr(diagnostic, "access denied") != NULL);

    setup(); seed(current, "prompt_color=blue\n");
    fail_next_allocation = true;
    assert(!shoot_config_reload(&config, diagnostic, sizeof(diagnostic)));
    check_value(&config, MG_TERMINAL_COLOR_GREEN);
    assert(strstr(diagnostic, current) != NULL &&
           strstr(diagnostic, "out of memory") != NULL);

    setup(); seed(current, "prompt_color=blue\n");
    close_result = MG_ERR_IO;
    assert(!shoot_config_reload(&config, diagnostic, sizeof(diagnostic)));
    check_value(&config, MG_TERMINAL_COLOR_GREEN);
    assert(strstr(diagnostic, current) != NULL &&
           strstr(diagnostic, "I/O failure") != NULL);

    setup(); seed(legacy, "prompt_color=blue\n");
    create_result = MG_ERR_ACCESS_DENIED;
    memset(&config, 0, sizeof(config));
    assert(shoot_config_reload(&config, diagnostic, sizeof(diagnostic)));
    check_value(&config, MG_TERMINAL_COLOR_BLUE);
    assert(find_file(legacy) >= 0 && find_file(current) < 0);
    assert(strstr(diagnostic, "original is preserved") != NULL);

    for (usize index = 0; index < sizeof(invalid) / sizeof(invalid[0]);
         index++) {
        shoot_config_defaults(&config);
        config.prompt_color = MG_TERMINAL_COLOR_YELLOW;
        unchanged = config;
        assert(!shoot_config_parse(invalid[index], strlen(invalid[index]),
                                   &config, diagnostic, sizeof(diagnostic)));
        assert(memcmp(&config, &unchanged, sizeof(config)) == 0);
        assert(strstr(diagnostic, "line ") != NULL);
    }

    shoot_config_defaults(&config);
    assert(shoot_config_parse(valid, sizeof(valid) - 1U, &config,
                              diagnostic, sizeof(diagnostic)));
    check_value(&config, MG_TERMINAL_COLOR_BLUE);
    assert(!config.history_enabled && !config.completion_case_sensitive);

    {
        char large[8193];
        const char suffix[] = "\nhistory_limit=1024\n";
        memset(large, 'x', sizeof(large) - 1U);
        large[0] = '/';
        large[1] = '/';
        memcpy(large + sizeof(large) - sizeof(suffix), suffix,
               sizeof(suffix));
        assert(shoot_config_parse(large, sizeof(large) - 1U, &config,
                                  diagnostic, sizeof(diagnostic)));
        assert(config.history_limit == 1024U);
        setup();
        seed(current, large);
        assert(files[0].length > 2048U);
        assert(shoot_config_reload(&config, diagnostic,
                                   sizeof(diagnostic)));
        assert(config.history_limit == 1024U);
    }
    return 0;
}
