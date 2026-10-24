/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <mg/filesystem.h>
#include <mg/identity.h>
#include <mg/object.h>
#include "../../userspace/reed/config.h"

static mg_identity_t test_identity;
static mg_result_t test_path_result;
static char test_contents[REED_CONFIG_FILE_CAPACITY];
static usize test_contents_length;
static usize test_contents_offset;
static char observed_path[REED_CONFIG_PATH_CAPACITY];

bool result_is_error(mg_result_t result)
{
    return result < 0;
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
    return MG_OK;
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

    reed_config_defaults(&config);
    assert(config.wrap && config.tab_width == 4U);
    assert(reed_config_parse(valid, sizeof(valid) - 1U, &config,
                             diagnostic, sizeof(diagnostic)));
    assert(!config.wrap && config.tab_width == 8U);
    assert(diagnostic[0] == '\0');

    config.wrap = true;
    config.tab_width = 3U;
    assert(!reed_config_parse(invalid, sizeof(invalid) - 1U, &config,
                              diagnostic, sizeof(diagnostic)));
    assert(config.wrap && config.tab_width == 3U);
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
}

static void test_paths(void)
{
    char path[REED_CONFIG_PATH_CAPACITY];

    assert(reed_config_path_for_home("/home/mangrove", path,
                                     sizeof(path)));
    assert(strcmp(path, "/home/mangrove/.config/reed/config") == 0);
    assert(reed_config_path_for_home("/", path, sizeof(path)));
    assert(strcmp(path, "/.config/reed/config") == 0);
    assert(!reed_config_path_for_home("relative", path, sizeof(path)));
    assert(!reed_config_path_for_home("/home/user", path, 8U));
}

static void test_load_and_fallback(void)
{
    reed_config_t config = {false, 12U};
    char diagnostic[128];
    const char valid[] = "wrap = false\ntab_width = 2\n";
    const char invalid[] = "wrap = false\ntab_width = 0\n";

    memset(&test_identity, 0, sizeof(test_identity));
    test_identity.uid = 100U;
    (void)snprintf(test_identity.home, sizeof(test_identity.home),
                   "/home/tester");
    test_path_result = MG_ERR_NOT_FOUND;
    test_contents_length = 0;
    assert(reed_config_load(&config, diagnostic, sizeof(diagnostic)) == MG_OK);
    assert(config.wrap && config.tab_width == 4U);
    assert(diagnostic[0] == '\0');
    assert(strcmp(observed_path, "/home/tester/.config/reed/config") == 0);

    memcpy(test_contents, valid, sizeof(valid) - 1U);
    test_contents_length = sizeof(valid) - 1U;
    test_path_result = MG_OK;
    assert(reed_config_load(&config, diagnostic, sizeof(diagnostic)) == MG_OK);
    assert(!config.wrap && config.tab_width == 2U);

    memcpy(test_contents, invalid, sizeof(invalid) - 1U);
    test_contents_length = sizeof(invalid) - 1U;
    assert(reed_config_load(&config, diagnostic, sizeof(diagnostic)) ==
           MG_ERR_BAD_ARGUMENT);
    assert(config.wrap && config.tab_width == 4U);
    assert(diagnostic[0] != '\0');
}

int main(void)
{
    test_parser();
    test_paths();
    test_load_and_fallback();
    return 0;
}
