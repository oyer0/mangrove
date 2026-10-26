/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/tls.h>
#include <mg/random.h>

#include <wolfssl/ssl.h>
#include "mgtls_platform.h"

static void mgtls_clear(void *buffer, usize length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)buffer;
    usize i;

    if (!bytes) return;
    for (i = 0; i < length; ++i) bytes[i] = 0;
}

mg_result_t mgtls_init(void)
{
    mg_result_t random_result = mg_random_status();
    if (random_result != MG_OK) return random_result;
    return wolfSSL_Init() == WOLFSSL_SUCCESS ? MG_OK : MG_ERR_IO;
}

mg_result_t mgtls_cleanup(void)
{
    return wolfSSL_Cleanup() == WOLFSSL_SUCCESS ? MG_OK : MG_ERR_IO;
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
        mgtls_clear(output, (usize)length);
        return (int)result;
    }
    return 0;
}
