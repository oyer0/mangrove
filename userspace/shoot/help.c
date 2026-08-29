#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/help.h"
#include "help.h"

static const char *category_description(const char *category)
{
    if (!strcmp(category, "files")) return "file and directory commands";
    if (!strcmp(category, "system")) return "system information and control";
    if (!strcmp(category, "network")) return "network configuration and diagnostics";
    if (!strcmp(category, "users")) return "user identity and account commands";
    return "commands in this category";
}

static bool category_is_alias(const char *category)
{
    return category && !strcmp(category, "users");
}

static void print_name_column(const char *name, usize width)
{
    usize length = name ? strlen(name) : 0;

    printf("%s", name ? name : "");
    while (length++ < width) printf(" ");
}

static void print_builtin_category(void)
{
    const shell_command_info_t *builtin;

    builtin = find_builtin("cd");
    printf("  ");
    print_name_column(builtin->name, 8);
    printf("%s\n", builtin->description);
    builtin = find_builtin("exit");
    printf("  ");
    print_name_column(builtin->name, 8);
    printf("%s\n", builtin->description);
    builtin = find_builtin("help");
    printf("  ");
    print_name_column(builtin->name, 8);
    printf("%s\n", builtin->description);
}

static bool print_category(const char *category)
{
    mg_help_record_t *records;
    usize count = 0;
    mg_result_t result;

    records = (mg_help_record_t *)malloc(sizeof(*records) * 64U);
    if (!records) {
        printf("Help unavailable for category %s.\n", category);
        return true;
    }

    if (category_is_alias(category)) category = "system";
    result = help_list_category(category, records,
                                64U, &count);
    if (result != MG_OK) {
        printf("Help unavailable for category %s.\n", category);
        free(records);
        return true;
    }
    printf("%s commands\n\n", category);
    for (usize index = 0; index < count; index++) {
        printf("  ");
        print_name_column(records[index].name, 8);
        printf("%s\n", records[index].description);
    }
    if (!strcmp(category, "system")) print_builtin_category();
    free(records);
    return true;
}

static bool known_category(const char *name)
{
    mg_help_category_t categories[MG_HELP_MAX_CATEGORIES];
    usize count = 0;

    if (!name) return false;
    if (!strcmp(name, "users")) return true;
    if (help_list_categories(categories,
                             sizeof(categories) / sizeof(categories[0]),
                             &count) != MG_OK) return false;
    for (usize index = 0; index < count; index++)
        if (!strcmp(categories[index].name, name)) return true;
    return false;
}

static void print_categories(void)
{
    mg_help_category_t categories[MG_HELP_MAX_CATEGORIES];
    usize count = 0;

    printf("Commands are grouped by purpose.\n\n");
    if (help_list_categories(categories,
                             sizeof(categories) / sizeof(categories[0]),
                             &count) != MG_OK) {
        printf("Use:\n  help <category>\n  help <command>\n");
        return;
    }
    for (usize index = 0; index < count; index++) {
        print_name_column(categories[index].name, 11);
        printf("%s\n", category_description(categories[index].name));
    }
    printf("\nUse:\n  help <category>\n  help <command>\n");
    printf("If a name is both, command help takes precedence; use:\n");
    printf("  help category <name>\n");
}

bool render_help(const shell_command_t *command)
{
    const shell_command_info_t *builtin;
    mg_help_record_t record;
    mg_result_t result;

    if (!command || command->argument_count == 0) {
        print_categories();
        return true;
    }
    if (command->argument_count == 2 &&
        !strcmp(command->arguments[0], "category"))
        return print_category(command->arguments[1]);
    if (command->argument_count != 1) {
        printf("Usage: help [category|command]\n");
        return true;
    }

    builtin = find_builtin(command->arguments[0]);
    if (builtin) {
        printf("%s - %s\n\nUsage:\n  %s\n\n%s\n",
               builtin->name, builtin->description, builtin->usage,
               builtin->help);
        return true;
    }
    result = help_load_record(command->arguments[0], &record);
    if (result == MG_OK) {
        help_print_record(&record);
        return true;
    }
    if (known_category(command->arguments[0]))
        return print_category(command->arguments[0]);
    printf("Unknown help topic: %s\n", command->arguments[0]);
    return true;
}
