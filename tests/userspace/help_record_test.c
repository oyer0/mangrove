/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mg/terminal.h>
#include "../../userspace/common/help.c"

static u32 terminal_columns = 80U;

mg_result_t terminal_get_size(mg_terminal_size_t *out_size)
{
    if (!out_size) return MG_ERR_BAD_ARGUMENT;
    out_size->version = MG_TERMINAL_API_VERSION;
    out_size->rows = 24U;
    out_size->columns = terminal_columns;
    return MG_OK;
}

static void test_schema_sections(void)
{
    char source[] =
        "name=demo\n"
        "category=system\n"
        "description=demonstrate help sections\n"
        "usage=demo [FILE]\n"
        "option=-h, --help    show quick help\n"
        "keybinding=q    quit the interactive viewer\n"
        "command=/save    save the current document\n"
        "note=The viewer is read-only.\n";
    mg_help_record_t record;

    assert(parse_record(source, "demo", &record));
    assert(record.usage_count == 1U && record.option_count == 1U);
    assert(record.keybinding_count == 1U && record.command_count == 1U);
    assert(record.note_count == 1U);
    assert(strcmp(record.options[0], "-h, --help    show quick help") == 0);
    assert(strcmp(record.keybindings[0],
                  "q    quit the interactive viewer") == 0);
    assert(strcmp(record.commands[0], "/save    save the current document") == 0);
}

static void test_legacy_and_empty_sections(void)
{
    char source[] =
        "name=oldtool\n"
        "category=system\n"
        "description=legacy record remains valid\n"
        "usage=oldtool\n"
        "option=-h, --help    show this help\n";
    mg_help_record_t record;
    FILE *capture;
    int saved_stdout;
    long length;
    char output[2048];

    assert(parse_record(source, "oldtool", &record));
    assert(record.keybinding_count == 0U && record.command_count == 0U &&
           record.note_count == 0U);
    capture = tmpfile();
    assert(capture != NULL);
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    assert(saved_stdout >= 0 && dup2(fileno(capture), STDOUT_FILENO) >= 0);
    help_print_record(&record);
    fflush(stdout);
    assert(fseek(capture, 0L, SEEK_END) == 0);
    length = ftell(capture);
    assert(length >= 0L && (usize)length < sizeof(output));
    assert(fseek(capture, 0L, SEEK_SET) == 0);
    assert(fread(output, 1U, (usize)length, capture) == (usize)length);
    output[length] = '\0';
    assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    close(saved_stdout);
    fclose(capture);

    assert(strstr(output, "Usage:\n  oldtool\n") != NULL);
    assert(strstr(output, "Options:\n  -h, --help") != NULL);
    assert(strstr(output, "Keybindings:") == NULL);
    assert(strstr(output, "Commands:") == NULL);
    assert(strstr(output, "Notes:") == NULL);
}

static void test_wrapping_and_all_sections(void)
{
    char source[] =
        "name=demo\n"
        "category=system\n"
        "description=show a longer description without dropping words\n"
        "usage=demo source destination and additional operands\n"
        "option=-h, --help    show help for this command\n"
        "keybinding=j / Down    move to the next visual row\n"
        "command=/save    save the current document\n"
        "note=Long notes wrap without truncating their final words.\n";
    mg_help_record_t record;
    FILE *capture;
    int saved_stdout;
    long length;
    char output[4096];

    assert(parse_record(source, "demo", &record));
    terminal_columns = 18U;
    capture = tmpfile();
    assert(capture != NULL);
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    assert(saved_stdout >= 0 && dup2(fileno(capture), STDOUT_FILENO) >= 0);
    help_print_record(&record);
    fflush(stdout);
    assert(fseek(capture, 0L, SEEK_END) == 0);
    length = ftell(capture);
    assert(length >= 0L && (usize)length < sizeof(output));
    assert(fseek(capture, 0L, SEEK_SET) == 0);
    assert(fread(output, 1U, (usize)length, capture) == (usize)length);
    output[length] = '\0';
    assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    close(saved_stdout);
    fclose(capture);
    terminal_columns = 80U;

    assert(strstr(output, "destination") != NULL);
    assert(strstr(output, "operands") != NULL);
    assert(strstr(output, "Options:\n  -h, --help") != NULL);
    assert(strstr(output, "Keybindings:\n  j / Down") != NULL);
    assert(strstr(output, "Commands:\n  /save") != NULL);
    assert(strstr(output, "Notes:\n  Long notes") != NULL);
    assert(strstr(output, "final") != NULL);
    assert(strstr(output, "words.") != NULL);
}

static void test_installed_records_parse(const char *directory)
{
    DIR *stream = opendir(directory);
    struct dirent *entry;
    usize parsed = 0U;

    assert(stream != NULL);
    while ((entry = readdir(stream)) != NULL) {
        char path[512];
        char source[4096];
        FILE *file;
        usize length;
        mg_help_record_t record;

        if (entry->d_name[0] == '.') continue;
        assert(snprintf(path, sizeof(path), "%s/%s", directory,
                        entry->d_name) > 0);
        file = fopen(path, "rb");
        assert(file != NULL);
        length = fread(source, 1U, sizeof(source) - 1U, file);
        assert(!ferror(file) && feof(file));
        source[length] = '\0';
        assert(fclose(file) == 0);
        assert(parse_record(source, entry->d_name, &record));
        assert(record.usage_count != 0U);
        parsed++;
    }
    assert(closedir(stream) == 0);
    assert(parsed >= 40U);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    test_schema_sections();
    test_legacy_and_empty_sections();
    test_wrapping_and_all_sections();
    test_installed_records_parse(argv[1]);
    return 0;
}
