#pragma once

#include <mg/error.h>
#include <mg/types.h>

#define MG_HELP_NAME_CAPACITY 64U
#define MG_HELP_CATEGORY_CAPACITY 32U
#define MG_HELP_DESCRIPTION_CAPACITY 192U
#define MG_HELP_USAGE_CAPACITY 192U
#define MG_HELP_OPTION_CAPACITY 192U
#define MG_HELP_MAX_USAGES 12U
#define MG_HELP_MAX_OPTIONS 8U
#define MG_HELP_MAX_CATEGORIES 32U

typedef struct {
    char name[MG_HELP_NAME_CAPACITY];
    char category[MG_HELP_CATEGORY_CAPACITY];
    char description[MG_HELP_DESCRIPTION_CAPACITY];
    char usages[MG_HELP_MAX_USAGES][MG_HELP_USAGE_CAPACITY];
    char options[MG_HELP_MAX_OPTIONS][MG_HELP_OPTION_CAPACITY];
    usize usage_count;
    usize option_count;
} mg_help_record_t;

typedef struct {
    char name[MG_HELP_CATEGORY_CAPACITY];
    usize command_count;
} mg_help_category_t;

bool command_help_requested(int argc, char **argv);
int command_print_help(const char *argv0);
void command_usage_error(const char *argv0, const char *usage,
                         const char *option);

mg_result_t help_load_record(const char *name, mg_help_record_t *record);
void help_print_record(const mg_help_record_t *record);
mg_result_t help_list_category(const char *category,
                               mg_help_record_t *records,
                               usize capacity, usize *out_count);
mg_result_t help_list_categories(mg_help_category_t *categories,
                                 usize capacity, usize *out_count);
