/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

typedef enum {
    ENTROPY_SOURCE_RDSEED = 1,
    ENTROPY_SOURCE_RDRAND = 2,
} entropy_source_t;

typedef enum {
    ENTROPY_WORD_UNSUPPORTED = 0,
    ENTROPY_WORD_NOT_READY,
    ENTROPY_WORD_READY,
} entropy_word_result_t;

/* Architecture interface kept separate so source-selection and failure
 * behavior can be tested without executing privileged or CPU-specific code. */
bool entropy_platform_source_supported(entropy_source_t source);
entropy_word_result_t entropy_platform_read_word(entropy_source_t source,
                                                 u64 *value);
