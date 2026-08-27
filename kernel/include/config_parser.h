#pragma once

#include <types.h>

#define KERNEL_CONFIG_MAX_ENTRIES 32U
#define KERNEL_CONFIG_KEY_MAX     32U
#define KERNEL_CONFIG_VALUE_MAX   128U

typedef struct {
    char key[KERNEL_CONFIG_KEY_MAX];
    char value[KERNEL_CONFIG_VALUE_MAX];
} kernel_config_entry_t;

typedef struct {
    kernel_config_entry_t entries[KERNEL_CONFIG_MAX_ENTRIES];
    u32 count;
} kernel_config_document_t;

/* Parse the deliberately small key=value configuration language.  Comments
 * begin with // outside quoted values.  The parser never allocates memory. */
bool kernel_config_parse(const char *text, usize length,
                         kernel_config_document_t *document,
                         u32 *error_line);
const char *kernel_config_find(const kernel_config_document_t *document,
                               const char *key);
