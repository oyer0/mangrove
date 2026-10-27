/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mg/filesystem.h>
#include <mg/identity.h>
#include <mg/object.h>
#include "../../userspace/common/app_directory.h"
#include "../../userspace/reed/config.h"

static mg_identity_t test_identity;
static mg_result_t test_path_result;
static char *test_contents;
static usize test_contents_length;
static usize test_contents_offset;
static bool short_read;
static mg_result_t close_result = MG_OK;
static char observed_path[MG_PATH_MAX_BYTES + 1U];
static int fail_realloc_after = -1;

void *__real_realloc(void *pointer, size_t size);
void *__wrap_realloc(void *pointer, size_t size)
{
    if (fail_realloc_after == 0) {
        fail_realloc_after = -1;
        return NULL;
    }
    if (fail_realloc_after > 0) fail_realloc_after--;
    return __real_realloc(pointer, size);
}

bool result_is_error(mg_result_t result)
{
    return result < 0;
}

const char *error_string(mg_result_t result)
{
    if (result == MG_ERR_ACCESS_DENIED) return "access denied";
    if (result == MG_ERR_IO) return "I/O failure";
    if (result == MG_ERR_NO_MEMORY) return "out of memory";
    return "controlled filesystem error";
}

mg_result_t process_get_identity(mg_identity_t *identity)
{
    if (!identity) return MG_ERR_BAD_ARGUMENT;
    *identity = test_identity;
    return MG_OK;
}

mg_result_t path_info(const char *path, mg_path_info_t *out_info)
{
    if (!path || !out_info) return MG_ERR_BAD_ARGUMENT;
    (void)snprintf(observed_path, sizeof(observed_path), "%s", path);
    if (!strcmp(path, test_identity.home)) {
        memset(out_info, 0, sizeof(*out_info));
        out_info->type = MG_PATH_TYPE_DIRECTORY;
        out_info->owner_uid = test_identity.uid;
        return MG_OK;
    }
    if (test_path_result != MG_OK) return test_path_result;
    memset(out_info, 0, sizeof(*out_info));
    out_info->type = MG_PATH_TYPE_FILE;
    out_info->size = test_contents_length;
    return MG_OK;
}

mg_result_t file_open(const char *path, u32 flags)
{
    (void)path;
    (void)flags;
    test_contents_offset = 0;
    return 7;
}

mg_result_t object_read(mg_handle_t handle, void *buffer, usize length)
{
    usize available;
    usize copied;

    assert(handle == 7);
    if (short_read) return MG_ERR_END_OF_FILE;
    if (test_contents_offset >= test_contents_length) return MG_ERR_END_OF_FILE;
    available = test_contents_length - test_contents_offset;
    copied = available < length ? available : length;
    memcpy(buffer, test_contents + test_contents_offset, copied);
    test_contents_offset += copied;
    return (mg_result_t)copied;
}

mg_result_t handle_close(mg_handle_t handle)
{
    assert(handle == 7);
    return close_result;
}

static void test_parser(void)
{
    reed_config_t config = {false, 9U};
    char diagnostic[128];
    const char valid[] = "  # heading\n wrap = false ; no wrapping\n"
                         "tab_width\t=\t8\n// trailing comment\n";
    const char invalid[] = "wrap = false\ntab_width = 17\n";
    const char duplicate[] = "wrap = false\nwrap = true\n";
    const char unknown[] = "wrap = true\nindent_style = spaces\n";
    const char nul_value[] = {'w', 'r', 'a', 'p', ' ', '=', ' ', 't', 'r',
                              'u', 'e', '\0', 'x'};
    const char *invalid_positions[] = {
        "unknown=value\nwrap=false\n",
        "wrap=false\nunknown=value\ntab_width=8\n",
        "wrap=false\ntab_width=8\nunknown=value\n",
    };
    reed_config_t unchanged;

    reed_config_defaults(&config);
    assert(config.wrap && config.tab_width == 4U);
    assert(reed_config_parse(valid, sizeof(valid) - 1U, &config,
                             diagnostic, sizeof(diagnostic)));
    assert(!config.wrap && config.tab_width == 8U);
    assert(diagnostic[0] == '\0');

    config.wrap = true;
    config.tab_width = 3U;
    unchanged = config;
    assert(!reed_config_parse(invalid, sizeof(invalid) - 1U, &config,
                              diagnostic, sizeof(diagnostic)));
    assert(memcmp(&config, &unchanged, sizeof(config)) == 0);
    assert(strstr(diagnostic, "line 2") != NULL);
    assert(!reed_config_parse(duplicate, sizeof(duplicate) - 1U, &config,
                              diagnostic, sizeof(diagnostic)));
    assert(config.wrap && config.tab_width == 3U);
    assert(!reed_config_parse(unknown, sizeof(unknown) - 1U, &config,
                              diagnostic, sizeof(diagnostic)));
    assert(config.wrap && config.tab_width == 3U);
    assert(!reed_config_parse(nul_value, sizeof(nul_value), &config,
                              diagnostic, sizeof(diagnostic)));
    assert(config.wrap && config.tab_width == 3U);
    for (usize index = 0; index < sizeof(invalid_positions) /
                                sizeof(invalid_positions[0]); index++) {
        const char *invalid_text = invalid_positions[index];
        config.wrap = false;
        config.tab_width = 7U;
        unchanged = config;
        assert(!reed_config_parse(invalid_text, strlen(invalid_text),
                                  &config, diagnostic, sizeof(diagnostic)));
        assert(memcmp(&config, &unchanged, sizeof(config)) == 0);
        assert(strstr(diagnostic, index == 0U ? "line 1" :
                      index == 1U ? "line 2" : "line 3") != NULL);
    }

    {
        const usize comment_length = 16384U;
        const char suffix[] = "\ntab_width = 16\n";
        char *large = (char *)malloc(comment_length + sizeof(suffix));
        assert(large);
        large[0] = '#';
        memset(large + 1U, 'x', comment_length - 1U);
        memcpy(large + comment_length, suffix, sizeof(suffix));
        assert(reed_config_parse(large,
                                comment_length + sizeof(suffix) - 1U,
                                &config, diagnostic, sizeof(diagnostic)));
        assert(config.wrap && config.tab_width == 16U);
        free(large);
    }
}

static void test_paths(void)
{
    char path[MG_PATH_MAX_BYTES + 1U];

    memset(&test_identity, 0, sizeof(test_identity));
    test_identity.uid = 100U;
    (void)snprintf(test_identity.username, sizeof(test_identity.username),
                   "tester");
    (void)snprintf(test_identity.home, sizeof(test_identity.home),
                   "/home/tester");
    assert(app_directory_path(MG_APP_DIRECTORY_CONFIG, "reed", path,
                              sizeof(path)) == MG_OK);
    assert(strcmp(path, "/home/tester/.config/reed") == 0);
}

static void set_contents(const char *text, usize length)
{
    char *replacement = NULL;
    if (length) {
        replacement = (char *)malloc(length);
        assert(replacement);
        memcpy(replacement, text, length);
    }
    free(test_contents);
    test_contents = replacement;
    test_contents_length = length;
    test_contents_offset = 0;
    short_read = false;
    close_result = MG_OK;
}

static void test_load_and_fallback(void)
{
    reed_config_t config = {false, 12U};
    char diagnostic[512];
    const char valid[] = "wrap = false\ntab_width = 2\n";
    const char invalid[] = "wrap = false\ntab_width = 0\n";
    char *large;
    const usize large_length = 32768U;

    memset(&test_identity, 0, sizeof(test_identity));
    test_identity.uid = 100U;
    (void)snprintf(test_identity.username, sizeof(test_identity.username),
                   "tester");
    (void)snprintf(test_identity.home, sizeof(test_identity.home),
                   "/home/tester");
    test_path_result = MG_ERR_NOT_FOUND;
    set_contents(NULL, 0);
    assert(reed_config_load(&config, diagnostic, sizeof(diagnostic)) == MG_OK);
    assert(config.wrap && config.tab_width == 4U);
    assert(diagnostic[0] == '\0');
    assert(strcmp(observed_path, "/home/tester/.config/reed/config") == 0);

    test_path_result = MG_OK;
    set_contents("", 0U);
    assert(reed_config_load(&config, diagnostic, sizeof(diagnostic)) == MG_OK);
    assert(config.wrap && config.tab_width == 4U);
    assert(diagnostic[0] == '\0');

    set_contents(valid, sizeof(valid) - 1U);
    assert(reed_config_load(&config, diagnostic, sizeof(diagnostic)) == MG_OK);
    assert(!config.wrap && config.tab_width == 2U);

    set_contents(invalid, sizeof(invalid) - 1U);
    assert(reed_config_load(&config, diagnostic, sizeof(diagnostic)) ==
           MG_ERR_BAD_ARGUMENT);
    assert(config.wrap && config.tab_width == 4U);
    assert(strstr(diagnostic, "~/.config/reed/config") != NULL);
    assert(strstr(diagnostic, "line 2") != NULL);
    assert(strstr(diagnostic, "defaults used") != NULL);

    {
        const char *invalid_positions[] = {
            "unknown=value\nwrap=false\n",
            "wrap=false\nunknown=value\ntab_width=8\n",
            "wrap=false\ntab_width=8\nunknown=value\n",
        };
        for (usize index = 0; index < sizeof(invalid_positions) /
                                    sizeof(invalid_positions[0]); index++) {
            const char *contents = invalid_positions[index];
            set_contents(contents, strlen(contents));
            assert(reed_config_load(&config, diagnostic,
                                    sizeof(diagnostic)) == MG_ERR_BAD_ARGUMENT);
            assert(config.wrap && config.tab_width == 4U);
            assert(strstr(diagnostic, index == 0U ? "line 1" :
                          index == 1U ? "line 2" : "line 3") != NULL);
        }
    }

    large = (char *)malloc(large_length);
    assert(large);
    large[0] = '#';
    memset(large + 1U, 'c', large_length - 1U -
           (sizeof("\nwrap = false\n") - 1U));
    memcpy(large + large_length - (sizeof("\nwrap = false\n") - 1U),
           "\nwrap = false\n", sizeof("\nwrap = false\n") - 1U);
    set_contents(large, large_length);
    free(large);
    assert(reed_config_load(&config, diagnostic, sizeof(diagnostic)) == MG_OK);
    assert(!config.wrap && config.tab_width == 4U);

    short_read = true;
    assert(reed_config_load(&config, diagnostic, sizeof(diagnostic)) ==
           MG_ERR_IO);
    assert(config.wrap && config.tab_width == 4U);
    assert(strstr(diagnostic, "~/.config/reed/config") != NULL);
    assert(strstr(diagnostic, "I/O failure") != NULL);
    short_read = false;

    test_path_result = MG_ERR_ACCESS_DENIED;
    assert(reed_config_load(&config, diagnostic, sizeof(diagnostic)) ==
           MG_ERR_ACCESS_DENIED);
    assert(config.wrap && config.tab_width == 4U);
    assert(strstr(diagnostic, "~/.config/reed/config") != NULL);
    assert(strstr(diagnostic, "access denied") != NULL);
    test_path_result = MG_OK;

    short_read = true;
    assert(reed_config_load(&config, diagnostic, sizeof(diagnostic)) ==
           MG_ERR_IO);
    assert(config.wrap && config.tab_width == 4U);
    assert(strstr(diagnostic, "I/O failure") != NULL);
    short_read = false;

    close_result = MG_ERR_IO;
    assert(reed_config_load(&config, diagnostic, sizeof(diagnostic)) ==
           MG_ERR_IO);
    assert(config.wrap && config.tab_width == 4U);
    close_result = MG_OK;

    fail_realloc_after = 0;
    assert(reed_config_load(&config, diagnostic, sizeof(diagnostic)) ==
           MG_ERR_NO_MEMORY);
    assert(fail_realloc_after == -1 && config.wrap && config.tab_width == 4U);
    free(test_contents);
    test_contents = NULL;
}

int main(void)
{
    test_parser();
    test_paths();
    test_load_and_fallback();
    return 0;
}
