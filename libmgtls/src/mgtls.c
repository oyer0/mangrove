/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/tls.h>
#include <mg/random.h>
#include <mg/time.h>

#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include "mgtls_platform.h"
#include <time.h>

static void mgtls_clear(void *buffer, usize length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)buffer;
    usize i;

    if (!bytes) return;
    for (i = 0; i < length; ++i) bytes[i] = 0;
}

static bool mgtls_initialized;
static unsigned int mgtls_contexts;
static mg_result_t mgtls_callback_error;
static mg_result_t mgtls_clock_error;

static mg_result_t mgtls_trusted_unix_time(mg_unix_time_t *out)
{
    mg_realtime_info_t info;
    mg_result_t result;

    if (!out) return MG_ERR_BAD_ARGUMENT;
    result = mg_clock_realtime_info(&info);
    if (result != MG_OK) return result;
    if (info.state != MG_REALTIME_TRUSTED ||
        info.time.nanoseconds >= MG_TIME_NANOSECONDS_PER_SEC)
        return MG_ERR_TIME_UNAVAILABLE;
    result = mangrove_time_to_unix(&info.time, out);
    if (result != MG_OK) return MG_ERR_TIME_UNAVAILABLE;
    return MG_OK;
}

mg_result_t mgtls_platform_check_trusted_time(void)
{
    mg_unix_time_t now;
    mg_result_t result = mgtls_trusted_unix_time(&now);
    if (result != MG_OK) {
        mgtls_clock_error = result;
        mgtls_callback_error = result;
    }
    return result;
}

/* wolfSSL's wc_time_cb cannot return an error code. Record unavailable time
 * separately and make the certificate verify callback fail closed. */
static time_t mgtls_wolfssl_time(time_t *out_time)
{
    mg_unix_time_t value;
    mg_result_t result = mgtls_trusted_unix_time(&value);
    time_t converted;

    if (result != MG_OK || (i64)(time_t)value.seconds != value.seconds) {
        mgtls_clock_error = result != MG_OK ? result : MG_ERR_TIME_UNAVAILABLE;
        mgtls_callback_error = mgtls_clock_error;
        if (out_time) *out_time = (time_t)-1;
        return (time_t)-1;
    }
    converted = (time_t)value.seconds;
    if (out_time) *out_time = converted;
    return converted;
}

mg_result_t mgtls_platform_initialize_time(void)
{
    return wc_SetTimeCb(mgtls_wolfssl_time) == 0 ? MG_OK : MG_ERR_IO;
}

bool mgtls_platform_is_initialized(void)
{
    return mgtls_initialized;
}

mg_result_t mgtls_platform_callback_error(void)
{
    return mgtls_callback_error;
}

mg_result_t mgtls_platform_clock_error(void)
{
    return mgtls_clock_error;
}

void mgtls_platform_note_certificate_failure(mg_result_t reason)
{
    mgtls_callback_error = reason == MG_ERR_CERTIFICATE_SIGNATURE
        ? reason : MG_ERR_CERTIFICATE_INVALID;
}

void mgtls_platform_begin_tls_operation(void)
{
    mgtls_callback_error = MG_OK;
    mgtls_clock_error = MG_OK;
}

mg_result_t mgtls_platform_context_acquire(void)
{
    if (!mgtls_initialized) return MG_ERR_SERVICE_UNAVAILABLE;
    if (mgtls_contexts == (unsigned int)~0U) return MG_ERR_NO_MEMORY;
    ++mgtls_contexts;
    return MG_OK;
}

void mgtls_platform_context_release(void)
{
    if (mgtls_contexts != 0U) --mgtls_contexts;
}

mg_result_t mgtls_init(void)
{
    mg_result_t random_result = mg_random_status();
    if (random_result != MG_OK) return random_result;
    if (mgtls_initialized) return MG_OK;
    if (mgtls_platform_initialize_time() != MG_OK) return MG_ERR_IO;
    if (wolfSSL_Init() != WOLFSSL_SUCCESS) return MG_ERR_IO;
    mgtls_initialized = true;
    return MG_OK;
}

mg_result_t mgtls_cleanup(void)
{
    if (mgtls_contexts != 0U) return MG_ERR_BUSY;
    if (!mgtls_initialized) return MG_OK;
    if (wolfSSL_Cleanup() != WOLFSSL_SUCCESS) return MG_ERR_IO;
    mgtls_initialized = false;
    return MG_OK;
}

mg_result_t mgtls_random_bytes(void *buffer, usize length)
{
    mg_result_t result;

    if (!buffer && length != 0U) return MG_ERR_BAD_ARGUMENT;
    if (length == 0U) return MG_OK;
    result = mg_random_bytes(buffer, length);
    if (result != MG_OK) mgtls_clear(buffer, length);
    return result;
}

int mgtls_wolfssl_random_block(unsigned char *output, unsigned int length)
{
    mg_result_t result;

    if (!output && length != 0U) return MG_ERR_BAD_ARGUMENT;
    if (length == 0U) return 0;
    result = mg_random_bytes(output, (usize)length);
    if (result != MG_OK) {
        mgtls_callback_error = result;
        mgtls_clear(output, (usize)length);
        return (int)result;
    }
    return 0;
}
