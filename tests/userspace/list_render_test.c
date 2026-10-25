/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <string.h>

#include <mg/account.h>
#include <mg/filesystem.h>
#include <mg/terminal.h>

#define main list_command_main
#include "../../userspace/list/main.c"
#undef main

#define TEST_ENTRY_COUNT 16U
#define TEST_OUTPUT_CAPACITY 8192U

static mg_directory_entry_t directory_entries[TEST_ENTRY_COUNT];
static usize directory_entry_count;
static usize directory_position;
static usize terminal_columns = 80U;
static char captured_output[TEST_OUTPUT_CAPACITY];
static usize captured_length;

bool command_help_requested(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return false;
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

bool command_resolve_path(const char *input, char *output, usize capacity)
{
    usize length;

    if (!input || !output) return false;
    length = strlen(input);
    if (length >= capacity) return false;
    memcpy(output, input, length + 1U);
    return true;
}

bool result_is_error(mg_result_t result) { return result < 0; }

const char *error_string(mg_result_t result)
{
    (void)result;
    return "test error";
}

mg_result_t terminal_get_capabilities(mg_terminal_capabilities_t *out)
{
    if (!out) return MG_ERR_BAD_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->version = MG_TERMINAL_API_VERSION;
    return MG_OK;
}

mg_result_t terminal_get_size(mg_terminal_size_t *out)
{
    if (!out) return MG_ERR_BAD_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->version = MG_TERMINAL_API_VERSION;
    out->rows = 25U;
    out->columns = (u32)terminal_columns;
    return MG_OK;
}

mg_result_t terminal_write_semantic(const void *text, usize length,
                                    mg_terminal_style_role_t role)
{
    (void)role;
    return console_write(text, length);
}

mg_result_t console_begin_transaction(void) { return MG_OK; }
mg_result_t console_end_transaction(void) { return MG_OK; }

mg_result_t console_write(const void *buffer, usize length)
{
    if (!buffer || length > TEST_OUTPUT_CAPACITY - captured_length)
        return MG_ERR_BUFFER_TOO_SMALL;
    memcpy(captured_output + captured_length, buffer, length);
    captured_length += length;
    return (mg_result_t)length;
}

mg_result_t path_info(const char *path, mg_path_info_t *out)
{
    if (!path || !out) return MG_ERR_BAD_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!strncmp(path, "file", 4U)) { out->type = MG_PATH_TYPE_FILE; return MG_OK; }
    if (!strcmp(path, ".") || !strcmp(path, "dir name")) {
        out->type = MG_PATH_TYPE_DIRECTORY;
        return MG_OK;
    }
    return MG_ERR_NOT_FOUND;
}

mg_result_t directory_open(const char *path)
{
    if (!path || (strcmp(path, ".") && strcmp(path, "dir name")))
        return MG_ERR_NOT_FOUND;
    directory_position = 0U;
    return 42;
}

mg_result_t directory_read_batch(mg_handle_t handle,
                                 mg_directory_entry_t *out_entries,
                                 usize capacity, usize *out_count)
{
    usize remaining;
    usize count;

    if (handle != 42 || !out_entries || !out_count)
        return MG_ERR_INVALID_HANDLE;
    if (directory_position == directory_entry_count)
        return MG_ERR_END_OF_FILE;
    remaining = directory_entry_count - directory_position;
    count = remaining < capacity ? remaining : capacity;
    memcpy(out_entries, directory_entries + directory_position,
           count * sizeof(*out_entries));
    directory_position += count;
    *out_count = count;
    return MG_OK;
}

mg_result_t handle_close(mg_handle_t handle)
{
    return handle == 42 ? MG_OK : MG_ERR_INVALID_HANDLE;
}

mg_result_t account_list(mg_account_info_t *accounts, usize capacity,
                         usize *out_count)
{
    (void)accounts;
    (void)capacity;
    if (out_count) *out_count = 0U;
    return MG_OK;
}

static void set_entry(usize index, const char *name, usize length)
{
    assert(index < TEST_ENTRY_COUNT && length < sizeof(directory_entries[index].name));
    memset(&directory_entries[index], 0, sizeof(directory_entries[index]));
    memcpy(directory_entries[index].name, name, length);
    directory_entries[index].name[length] = '\0';
    directory_entries[index].type = MG_PATH_TYPE_FILE;
}

static int list_current_directory(void)
{
    char *argv[] = { "list", "." };
    return list_command_main(2, argv);
}

static void reset_fixture(usize count, usize columns)
{
    memset(directory_entries, 0, sizeof(directory_entries));
    memset(captured_output, 0, sizeof(captured_output));
    directory_entry_count = count;
    directory_position = 0U;
    terminal_columns = columns;
    captured_length = 0U;
}

static void expect_output(const char *expected)
{
    usize expected_length = strlen(expected);

    if (captured_length != expected_length ||
        memcmp(captured_output, expected, expected_length)) {
        fprintf(stderr, "list output mismatch (got %llu bytes, expected %llu):\n",
                (unsigned long long)captured_length,
                (unsigned long long)expected_length);
        fwrite(captured_output, 1U, captured_length, stderr);
        fputc('\n', stderr);
    }
    assert(captured_length == expected_length);
    assert(!memcmp(captured_output, expected, expected_length));
}

static void test_ambiguous_and_invisible_names(void)
{
    static const char cafe[] = "caf\xc3\xa9";
    static const char nonbreaking_space[] = "x\xc2\xa0y";
    static const char zero_width_space[] = "x\xe2\x80\x8by";
    static const char variation_selector[] = "x\xf3\xa0\x84\x80y";
    static const char invalid_utf8[] = { 'b', 'a', 'd', (char)0xff, 'n', 'm' };
    static const char expected[] =
        "plain.txt\n"
        "'hello world!'\n"
        "'O\\'Brien'\n"
        "'quote\\\"name'\n"
        "'a\\\\b'\n"
        "'tab\\tname'\n"
        "'line\\nname'\n"
        "'esc\\u{001B}[31m'\n"
        "caf\xc3\xa9\n"
        "'x\\u{00A0}y'\n"
        "'x\\u{200B}y'\n"
        "'x\\u{E0100}y'\n"
        "'bad\\xFFnm'\n";

    reset_fixture(13U, 1U);
    set_entry(0U, "plain.txt", strlen("plain.txt"));
    set_entry(1U, "hello world!", strlen("hello world!"));
    set_entry(2U, "O'Brien", strlen("O'Brien"));
    set_entry(3U, "quote\"name", strlen("quote\"name"));
    set_entry(4U, "a\\b", strlen("a\\b"));
    set_entry(5U, "tab\tname", strlen("tab\tname"));
    set_entry(6U, "line\nname", strlen("line\nname"));
    set_entry(7U, "esc\x1b[31m", strlen("esc\x1b[31m"));
    set_entry(8U, cafe, sizeof(cafe) - 1U);
    set_entry(9U, nonbreaking_space, sizeof(nonbreaking_space) - 1U);
    set_entry(10U, zero_width_space, sizeof(zero_width_space) - 1U);
    set_entry(11U, variation_selector, sizeof(variation_selector) - 1U);
    set_entry(12U, invalid_utf8, sizeof(invalid_utf8));
    assert(list_current_directory() == 0);
    expect_output(expected);
}

static void test_quote_width_controls_column_count(void)
{
    static const char expected[] = "a         g\n'bc def'\n";

    reset_fixture(3U, 13U);
    set_entry(0U, "a", 1U);
    set_entry(1U, "bc def", 6U);
    set_entry(2U, "g", 1U);
    assert(list_current_directory() == 0);
    expect_output(expected);
}

static void test_quoted_directory_suffix(void)
{
    static const char expected[] = "'dir name'/\n";
    char *argv[] = { "list", "--tree", "1", "dir name" };

    reset_fixture(0U, 80U);
    assert(list_command_main(4, argv) == 0);
    expect_output(expected);
}

static void test_many_operands(void)
{
    char names[1000][16];
    char *argv[1001] = {"list"};
    reset_fixture(0U, 80U);
    for (unsigned i = 0; i < 1000U; i++) {
        (void)snprintf(names[i], sizeof(names[i]), "file%03u", i);
        argv[i + 1U] = names[i];
    }
    assert(list_command_main(1001, argv) == 0);
    assert(captured_length == 1000U * 8U);
    for (unsigned i = 0; i < 1000U; i++) {
        assert(!memcmp(captured_output + i * 8U, names[i], 7U));
        assert(captured_output[i * 8U + 7U] == '\n');
    }
    reset_fixture(0U, 80U);
    argv[2] = "missing";
    assert(list_command_main(4, argv) == 1 && captured_length == 16U);
    assert(!memcmp(captured_output, "file000\nfile002\n", 16U));
    argv[2] = names[1];
    /* An invalid option is rejected before any operand is displayed. */
    reset_fixture(0U, 80U);
    argv[1000] = "--unsupported";
    assert(list_command_main(1001, argv) == 1 && !captured_length);
}

int main(void)
{
    test_ambiguous_and_invisible_names();
    test_quote_width_controls_column_count();
    test_quoted_directory_suffix();
    test_many_operands();
    puts("list: filename quoting, UTF-8 safety, and column-width regressions passed");
    return 0;
}
