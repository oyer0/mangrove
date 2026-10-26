/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/types.h>

/* Private wolfCrypt custom random block callback. */
int mgtls_wolfssl_random_block(unsigned char *output, unsigned int length);

/* Process-local bridge shared by the native API implementation units. */
mg_result_t mgtls_platform_initialize_time(void);
mg_result_t mgtls_platform_check_trusted_time(void);
bool mgtls_platform_is_initialized(void);
mg_result_t mgtls_platform_callback_error(void);
mg_result_t mgtls_platform_clock_error(void);
void mgtls_platform_note_certificate_failure(void);
void mgtls_platform_begin_tls_operation(void);
mg_result_t mgtls_platform_context_acquire(void);
void mgtls_platform_context_release(void);
