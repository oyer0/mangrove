/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/tls.h>
#include <mg/filesystem.h>
#include <mg/net.h>
#include <mg/object.h>
#include <mg/time.h>

#include <wolfssl/ssl.h>
#include <wolfssl/wolfio.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include "mgtls_platform.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifndef MGTLS_CA_BUNDLE_PATH
#define MGTLS_CA_BUNDLE_PATH MGTLS_SYSTEM_CA_BUNDLE
#endif
#ifndef MGTLS_CA_MANIFEST_PATH
#define MGTLS_CA_MANIFEST_PATH MGTLS_SYSTEM_CA_MANIFEST
#endif

#define MGTLS_CA_READ_CHUNK 4096U
#define MGTLS_SHA256_BYTES 32U
#define MGTLS_SHA256_HEX_BYTES (MGTLS_SHA256_BYTES * 2U)
#define MGTLS_HOSTNAME_MAX_BYTES 253U

typedef struct mgtls_deadline {
    u64 end_ms;
    bool immediate;
} mgtls_deadline_t;

struct mgtls_context {
    WOLFSSL_CTX *wolf;
    unsigned int sessions;
};

struct mgtls_session {
    WOLFSSL *wolf;
    mgtls_context_t *context;
    mg_handle_t stream;
    mg_result_t io_error;
    mgtls_deadline_t deadline;
    bool owns_stream;
    bool handshake_complete;
    bool shutdown_complete;
    bool poisoned;
};

static usize bounded_string_length(const char *text, usize bound)
{
    usize length = 0;
    if (!text) return bound + 1U;
    while (length < bound && text[length] != '\0') ++length;
    return length;
}

static mg_result_t deadline_start(u32 timeout_ms, mgtls_deadline_t *out)
{
    mg_monotonic_time_t now;
    mg_result_t result;

    if (!out) return MG_ERR_BAD_ARGUMENT;
    if (timeout_ms == MG_NET_TIMEOUT_DEFAULT ||
        timeout_ms == (u32)~0U)
        timeout_ms = MGTLS_TIMEOUT_DEFAULT;
    result = mg_clock_monotonic(&now);
    if (result != MG_OK) return result;
    out->immediate = timeout_ms == 0U;
    if (now.milliseconds > (u64)~(u64)0 - timeout_ms)
        return MG_ERR_BAD_ARGUMENT;
    out->end_ms = now.milliseconds + timeout_ms;
    return MG_OK;
}

static mg_result_t deadline_remaining(const mgtls_deadline_t *deadline,
                                      u32 *out_ms)
{
    mg_monotonic_time_t now;
    mg_result_t result;

    if (!deadline || !out_ms) return MG_ERR_BAD_ARGUMENT;
    if (deadline->immediate) {
        *out_ms = MG_NET_TIMEOUT_IMMEDIATE;
        return MG_OK;
    }
    result = mg_clock_monotonic(&now);
    if (result != MG_OK) return result;
    if (now.milliseconds >= deadline->end_ms) return MG_ERR_TIMEOUT;
    {
        u64 remaining = deadline->end_ms - now.milliseconds;
        *out_ms = remaining > (u64)~(u32)0 ? (u32)~0U : (u32)remaining;
    }
    return MG_OK;
}

static mg_result_t current_deadline_timeout(mgtls_session_t *session,
                                           u32 *out_ms)
{
    mg_result_t result = deadline_remaining(&session->deadline, out_ms);
    if (result != MG_OK) session->io_error = result;
    return result;
}

static int callback_error_code(mg_result_t error, bool writing)
{
    switch (error) {
    case MG_ERR_TIMEOUT:
    case MG_ERR_WOULD_BLOCK:
        return writing ? WOLFSSL_CBIO_ERR_WANT_WRITE :
                         WOLFSSL_CBIO_ERR_WANT_READ;
    case MG_ERR_INTERRUPTED:
    case MG_ERR_SHUTDOWN:
        return WOLFSSL_CBIO_ERR_ISR;
    case MG_ERR_CONNECTION_RESET:
        return WOLFSSL_CBIO_ERR_CONN_RST;
    case MG_ERR_CONNECTION_CLOSED:
        return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    default:
        return WOLFSSL_CBIO_ERR_GENERAL;
    }
}

static int mgtls_io_receive(WOLFSSL *wolf, char *buffer, int capacity,
                            void *opaque)
{
    mgtls_session_t *session = (mgtls_session_t *)opaque;
    mg_result_t result;
    u32 timeout_ms;

    (void)wolf;
    if (!session || !buffer || capacity <= 0 || !session->owns_stream)
        return WOLFSSL_CBIO_ERR_GENERAL;
    session->io_error = MG_OK;
    result = current_deadline_timeout(session, &timeout_ms);
    if (result == MG_OK)
        result = mg_stream_receive(session->stream, buffer,
                                   (usize)capacity, timeout_ms);
    if (result > 0) return (int)result;
    if (result == 0) result = MG_ERR_CONNECTION_CLOSED;
    session->io_error = result;
    return callback_error_code(result, false);
}

static int mgtls_io_send(WOLFSSL *wolf, char *buffer, int length,
                         void *opaque)
{
    mgtls_session_t *session = (mgtls_session_t *)opaque;
    mg_result_t result;
    u32 timeout_ms;

    (void)wolf;
    if (!session || !buffer || length <= 0 || !session->owns_stream)
        return WOLFSSL_CBIO_ERR_GENERAL;
    session->io_error = MG_OK;
    result = current_deadline_timeout(session, &timeout_ms);
    if (result == MG_OK)
        result = mg_stream_send(session->stream, buffer,
                                (usize)length, timeout_ms);
    if (result > 0) return (int)result;
    if (result == 0) result = MG_ERR_CONNECTION_CLOSED;
    session->io_error = result;
    return callback_error_code(result, true);
}

static int mgtls_verify_callback(int preverified,
                                 WOLFSSL_X509_STORE_CTX *store)
{
    if (!preverified) {
        int verify_error = wolfSSL_X509_STORE_CTX_get_error(store);
        mg_result_t reason = verify_error == ASN_SIG_CONFIRM_E
            ? MG_ERR_CERTIFICATE_SIGNATURE : MG_ERR_CERTIFICATE_INVALID;
        mgtls_platform_note_certificate_failure(reason);
        return 0;
    }
    if (mgtls_platform_check_trusted_time() != MG_OK) return 0;
    return 1;
}

static mg_result_t read_file_all(const char *path, unsigned char **out_data,
                                 usize *out_length)
{
    unsigned char chunk[MGTLS_CA_READ_CHUNK];
    unsigned char *data = NULL;
    usize length = 0;
    usize capacity = 0;
    mg_result_t open_result;
    mg_result_t result = MG_OK;
    mg_handle_t handle;

    if (!path || !out_data || !out_length) return MG_ERR_BAD_ARGUMENT;
    *out_data = NULL;
    *out_length = 0;
    open_result = file_open(path, MG_OPEN_READ);
    if (open_result < 0) return open_result;
    handle = (mg_handle_t)open_result;

    for (;;) {
        mg_result_t read_result = object_read(handle, chunk, sizeof(chunk));
        usize received;
        usize required;
        usize new_capacity;
        unsigned char *replacement;

        if (read_result < 0) {
            result = read_result;
            break;
        }
        if (read_result == 0) break;
        if ((u64)read_result > sizeof(chunk)) {
            result = MG_ERR_PROTOCOL;
            break;
        }
        received = (usize)read_result;
        if (length > (usize)UINT32_MAX - received) {
            result = MG_ERR_TRUST_STORE_INVALID;
            break;
        }
        required = length + received;
        if (required > capacity) {
            new_capacity = capacity ? capacity : MGTLS_CA_READ_CHUNK;
            while (new_capacity < required) {
                if (new_capacity > (usize)UINT32_MAX / 2U) {
                    new_capacity = required;
                    break;
                }
                new_capacity *= 2U;
            }
            if (new_capacity > (usize)UINT32_MAX) {
                result = MG_ERR_TRUST_STORE_INVALID;
                break;
            }
            replacement = (unsigned char *)realloc(data,
                                                    (size_t)new_capacity);
            if (!replacement) {
                result = MG_ERR_NO_MEMORY;
                break;
            }
            data = replacement;
            capacity = new_capacity;
        }
        memcpy(data + length, chunk, (size_t)received);
        length = required;
    }

    {
        mg_result_t close_result = handle_close(handle);
        if (result == MG_OK && close_result != MG_OK)
            result = close_result;
    }
    if (result != MG_OK) {
        free(data);
        return result;
    }
    if (length == 0U) {
        free(data);
        return MG_ERR_TRUST_STORE_INVALID;
    }
    *out_data = data;
    *out_length = length;
    return MG_OK;
}

static int hex_value(unsigned char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool json_string_field(const unsigned char *json, usize length,
                              const char *field, unsigned char *out,
                              usize capacity, usize *out_length)
{
    usize field_length = (usize)strlen(field);
    usize found = 0;
    usize at = 0;
    usize value_start;
    usize value_length = 0;
    usize index;

    if (!json || !field || !out || !out_length) return false;
    while (at + field_length + 2U <= length) {
        if (json[at] == '"' &&
            memcmp(json + at + 1U, field, (size_t)field_length) == 0 &&
            json[at + field_length + 1U] == '"') {
            ++found;
            value_start = at + field_length + 2U;
            while (value_start < length &&
                   (json[value_start] == ' ' || json[value_start] == '\t' ||
                    json[value_start] == '\r' || json[value_start] == '\n'))
                ++value_start;
            if (value_start == length || json[value_start++] != ':')
                return false;
            while (value_start < length &&
                   (json[value_start] == ' ' || json[value_start] == '\t' ||
                    json[value_start] == '\r' || json[value_start] == '\n'))
                ++value_start;
            if (value_start == length || json[value_start++] != '"')
                return false;
            index = value_start;
            while (index < length && json[index] != '"') ++index;
            if (index == length) return false;
            value_length = index - value_start;
            if (value_length == 0U || value_length > capacity) return false;
            memcpy(out, json + value_start, (size_t)value_length);
            *out_length = value_length;
        }
        ++at;
    }
    return found == 1U;
}

static bool json_u32_field(const unsigned char *json, usize length,
                           const char *field, u32 *out_value)
{
    usize field_length = (usize)strlen(field);
    usize found = 0;
    usize at = 0;
    u32 value = 0;

    if (!json || !field || !out_value) return false;
    while (at + field_length + 2U <= length) {
        if (json[at] == '"' &&
            memcmp(json + at + 1U, field, (size_t)field_length) == 0 &&
            json[at + field_length + 1U] == '"') {
            usize index = at + field_length + 2U;
            ++found;
            while (index < length &&
                   (json[index] == ' ' || json[index] == '\t' ||
                    json[index] == '\r' || json[index] == '\n')) ++index;
            if (index == length || json[index++] != ':') return false;
            while (index < length &&
                   (json[index] == ' ' || json[index] == '\t' ||
                    json[index] == '\r' || json[index] == '\n')) ++index;
            if (index == length || json[index] < '0' || json[index] > '9')
                return false;
            value = 0;
            do {
                u32 digit = (u32)(json[index] - '0');
                if (value > (UINT32_MAX - digit) / 10U) return false;
                value = value * 10U + digit;
                ++index;
            } while (index < length && json[index] >= '0' &&
                     json[index] <= '9');
            if (index < length && json[index] == '.') return false;
        }
        ++at;
    }
    if (found != 1U || value == 0U) return false;
    *out_value = value;
    return true;
}

static bool trust_manifest_digest(const unsigned char *manifest,
                                  usize manifest_length,
                                  const unsigned char *bundle,
                                  usize bundle_length,
                                  u32 *out_certificate_count)
{
    unsigned char expected_hex[MGTLS_SHA256_HEX_BYTES];
    unsigned char expected[MGTLS_SHA256_BYTES];
    unsigned char actual[MGTLS_SHA256_BYTES];
    usize hex_length = 0;
    wc_Sha256 hash;

    if (!manifest || !bundle || !out_certificate_count ||
        manifest_length > (usize)UINT32_MAX ||
        bundle_length > (usize)UINT32_MAX ||
        !json_string_field(manifest, manifest_length, "bundle_sha256",
                           expected_hex, sizeof(expected_hex), &hex_length) ||
        hex_length != sizeof(expected_hex) ||
        !json_u32_field(manifest, manifest_length, "certificate_count",
                        out_certificate_count))
        return false;
    for (usize i = 0; i < MGTLS_SHA256_BYTES; ++i) {
        int high = hex_value(expected_hex[i * 2U]);
        int low = hex_value(expected_hex[i * 2U + 1U]);
        if (high < 0 || low < 0) return false;
        expected[i] = (unsigned char)((high << 4) | low);
    }
    if (wc_InitSha256(&hash) != 0) return false;
    if (wc_Sha256Update(&hash, bundle, (word32)bundle_length) != 0 ||
        wc_Sha256Final(&hash, actual) != 0)
        return false;
    return memcmp(expected, actual, sizeof(expected)) == 0;
}

static bool exact_line(const unsigned char *data, usize start, usize end,
                       const char *expected)
{
    usize length = end - start;
    usize expected_length = (usize)strlen(expected);
    if (length && data[start + length - 1U] == '\r') --length;
    return length == expected_length &&
           memcmp(data + start, expected, (size_t)length) == 0;
}

static mg_result_t load_verified_roots(WOLFSSL_CTX *wolf)
{
    unsigned char *bundle = NULL;
    unsigned char *manifest = NULL;
    usize bundle_length = 0;
    usize manifest_length = 0;
    u32 expected_count = 0;
    u32 loaded_count = 0;
    usize line_start = 0;
    usize cert_start = 0;
    bool in_certificate = false;
    mg_result_t result;

    result = read_file_all(MGTLS_CA_BUNDLE_PATH, &bundle, &bundle_length);
    if (result != MG_OK) return result;
    result = read_file_all(MGTLS_CA_MANIFEST_PATH, &manifest,
                           &manifest_length);
    if (result != MG_OK) {
        free(bundle);
        return result;
    }
    if (memchr(bundle, '\0', (size_t)bundle_length) != NULL ||
        memchr(manifest, '\0', (size_t)manifest_length) != NULL ||
        !trust_manifest_digest(manifest, manifest_length, bundle,
                               bundle_length, &expected_count)) {
        result = MG_ERR_TRUST_STORE_INVALID;
        goto done;
    }

    for (usize at = 0; at <= bundle_length; ++at) {
        if (at != bundle_length && bundle[at] != '\n') continue;
        usize line_end = at;
        if (!in_certificate && exact_line(bundle, line_start, line_end,
                                           "-----BEGIN CERTIFICATE-----")) {
            cert_start = line_start;
            in_certificate = true;
        }
        else if (in_certificate &&
                 exact_line(bundle, line_start, line_end,
                            "-----END CERTIFICATE-----")) {
            usize cert_end = at < bundle_length ? at + 1U : at;
            usize cert_length = cert_end - cert_start;
            int loaded;
            if (cert_length > (usize)LONG_MAX) {
                result = MG_ERR_TRUST_STORE_INVALID;
                goto done;
            }
            loaded = wolfSSL_CTX_load_verify_buffer(wolf,
                bundle + cert_start, (long)cert_length,
                WOLFSSL_FILETYPE_PEM);
            if (loaded != WOLFSSL_SUCCESS) {
                result = loaded == MEMORY_E ? MG_ERR_NO_MEMORY :
                                             MG_ERR_TRUST_STORE_INVALID;
                goto done;
            }
            if (loaded_count == UINT32_MAX) {
                result = MG_ERR_TRUST_STORE_INVALID;
                goto done;
            }
            ++loaded_count;
            in_certificate = false;
        }
        else if (in_certificate &&
                 exact_line(bundle, line_start, line_end,
                            "-----BEGIN CERTIFICATE-----")) {
            result = MG_ERR_TRUST_STORE_INVALID;
            goto done;
        }
        else if (!in_certificate &&
                 (exact_line(bundle, line_start, line_end,
                             "-----END CERTIFICATE-----") ||
                  (line_end - line_start >= 11U &&
                   memcmp(bundle + line_start, "-----BEGIN ", 11U) == 0) ||
                  (line_end - line_start >= 9U &&
                   memcmp(bundle + line_start, "-----END ", 9U) == 0))) {
            result = MG_ERR_TRUST_STORE_INVALID;
            goto done;
        }
        line_start = at + 1U;
    }
    if (in_certificate || loaded_count == 0U || loaded_count != expected_count)
        result = MG_ERR_TRUST_STORE_INVALID;
    else
        result = MG_OK;

done:
    free(bundle);
    free(manifest);
    return result;
}

mg_result_t mgtls_context_create(mgtls_context_t **out_context)
{
    mgtls_context_t *context;
    mg_result_t result;

    if (!out_context) return MG_ERR_BAD_ARGUMENT;
    *out_context = NULL;
    if (!mgtls_platform_is_initialized()) return MG_ERR_SERVICE_UNAVAILABLE;
    /* wolfSSL's trust-anchor loader applies certificate validity checks too,
     * so fail with the clock's native status before translating it as a
     * malformed trust-store error. */
    result = mgtls_platform_check_trusted_time();
    if (result != MG_OK) return result;
    context = (mgtls_context_t *)calloc(1U, sizeof(*context));
    if (!context) return MG_ERR_NO_MEMORY;
    context->wolf = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    if (!context->wolf) {
        free(context);
        return MG_ERR_NO_MEMORY;
    }
    wolfSSL_CTX_set_verify(context->wolf, WOLFSSL_VERIFY_PEER,
                           mgtls_verify_callback);
    wolfSSL_CTX_SetIORecv(context->wolf, mgtls_io_receive);
    wolfSSL_CTX_SetIOSend(context->wolf, mgtls_io_send);
    result = load_verified_roots(context->wolf);
    if (result != MG_OK) {
        wolfSSL_CTX_free(context->wolf);
        free(context);
        return result;
    }
    result = mgtls_platform_context_acquire();
    if (result != MG_OK) {
        wolfSSL_CTX_free(context->wolf);
        free(context);
        return result;
    }
    *out_context = context;
    return MG_OK;
}

mg_result_t mgtls_context_destroy(mgtls_context_t *context)
{
    if (!context) return MG_ERR_BAD_ARGUMENT;
    if (context->sessions != 0U) return MG_ERR_BUSY;
    wolfSSL_CTX_free(context->wolf);
    mgtls_platform_context_release();
    free(context);
    return MG_OK;
}

static mg_result_t resolve_ipv4(const char *name, usize name_length,
                                u32 timeout_ms, mg_ipv4_addr_t *out_address,
                                bool *out_is_ip)
{
    mg_ipv4_addr_t address;

    if (!name || !out_address || !out_is_ip || name_length == 0U ||
        name_length > MGTLS_HOSTNAME_MAX_BYTES)
        return MG_ERR_BAD_ARGUMENT;
    if (mg_ipv4_parse(name, &address)) {
        *out_address = address;
        *out_is_ip = true;
        return MG_OK;
    }
    *out_is_ip = false;
    return mg_net_resolve_a(name, out_address, timeout_ms);
}

static mg_result_t mgtls_ssl_failure(mgtls_session_t *session, int ret)
{
    if (session->io_error != MG_OK) return session->io_error;
    mg_result_t callback_error = mgtls_platform_callback_error();
    mg_result_t clock_error = mgtls_platform_clock_error();
    int error;

    if (callback_error != MG_OK) return callback_error;
    if (clock_error != MG_OK) return clock_error;
    error = wolfSSL_get_error(session->wolf, ret);
    if (error == WOLFSSL_ERROR_ZERO_RETURN)
        return MG_ERR_CONNECTION_CLOSED;
    if (error == WOLFSSL_ERROR_WANT_READ ||
        error == WOLFSSL_ERROR_WANT_WRITE)
        return MG_ERR_WOULD_BLOCK;
    if (wolfSSL_get_verify_result(session->wolf) != WOLFSSL_X509_V_OK)
        return MG_ERR_CERTIFICATE_INVALID;
    return MG_ERR_TLS_PROTOCOL;
}

static mg_result_t close_failed_connection(mgtls_session_t *session,
                                           mg_result_t failure)
{
    if (session->wolf) wolfSSL_free(session->wolf);
    if (session->owns_stream) {
        (void)mg_stream_close(session->stream);
        session->owns_stream = false;
    }
    free(session);
    return failure;
}

static mg_result_t mgtls_session_handshake(mgtls_session_t *session,
                                           mgtls_context_t *context,
                                           const char *server_name,
                                           usize name_length, bool is_ip,
                                           mg_handle_t stream,
                                           const mgtls_deadline_t *deadline,
                                           mgtls_session_t **out_session)
{
    mg_result_t result;
    u32 remaining;
    int ret;

    if (!session || !context || !server_name || !stream || !deadline ||
        !out_session) return MG_ERR_BAD_ARGUMENT;
    session->context = context;
    session->stream = stream;
    session->owns_stream = true;
    session->deadline = *deadline;

    result = deadline_remaining(&session->deadline, &remaining);
    if (result != MG_OK) return close_failed_connection(session, result);
    session->wolf = wolfSSL_new(context->wolf);
    if (!session->wolf)
        return close_failed_connection(session, MG_ERR_NO_MEMORY);
    wolfSSL_SetIOReadCtx(session->wolf, session);
    wolfSSL_SetIOWriteCtx(session->wolf, session);

    if (is_ip) {
        if (wolfSSL_check_ip_address(session->wolf, server_name) !=
                WOLFSSL_SUCCESS)
            return close_failed_connection(session, MG_ERR_BAD_ARGUMENT);
    } else {
        if (wolfSSL_check_domain_name(session->wolf, server_name) !=
                WOLFSSL_SUCCESS ||
            wolfSSL_UseSNI(session->wolf, WOLFSSL_SNI_HOST_NAME,
                           server_name, (unsigned short)name_length) !=
                WOLFSSL_SUCCESS)
            return close_failed_connection(session, MG_ERR_BAD_ARGUMENT);
    }

    result = mgtls_platform_check_trusted_time();
    if (result != MG_OK) return close_failed_connection(session, result);
    mgtls_platform_begin_tls_operation();
    session->io_error = MG_OK;
    ret = wolfSSL_connect(session->wolf);
    if (ret != WOLFSSL_SUCCESS)
        return close_failed_connection(session,
                                       mgtls_ssl_failure(session, ret));
    result = mgtls_platform_check_trusted_time();
    if (result != MG_OK)
        return close_failed_connection(session, result);
    if (wolfSSL_get_verify_result(session->wolf) != WOLFSSL_X509_V_OK)
        return close_failed_connection(session,
                                       MG_ERR_CERTIFICATE_INVALID);

    session->handshake_complete = true;
    if (context->sessions == (unsigned int)~0U)
        return close_failed_connection(session, MG_ERR_NO_MEMORY);
    ++context->sessions;
    *out_session = session;
    return MG_OK;
}

mg_result_t mgtls_session_connect(mgtls_context_t *context,
                                  const char *server_name, u16 port,
                                  u32 timeout_ms,
                                  mgtls_session_t **out_session)
{
    usize name_length;
    mg_ipv4_addr_t address;
    mg_net_endpoint_t endpoint;
    bool is_ip;
    mg_result_t result;
    u32 remaining;
    mgtls_session_t *session;
    mgtls_deadline_t deadline;

    if (!out_session) return MG_ERR_BAD_ARGUMENT;
    *out_session = NULL;
    if (!context || !server_name || port == 0U) return MG_ERR_BAD_ARGUMENT;
    name_length = bounded_string_length(server_name,
                                         MGTLS_HOSTNAME_MAX_BYTES + 1U);
    if (name_length == 0U || name_length > MGTLS_HOSTNAME_MAX_BYTES)
        return MG_ERR_BAD_ARGUMENT;
    result = mgtls_platform_check_trusted_time();
    if (result != MG_OK) return result;
    session = (mgtls_session_t *)calloc(1U, sizeof(*session));
    if (!session) return MG_ERR_NO_MEMORY;
    result = deadline_start(timeout_ms, &deadline);
    if (result != MG_OK) {
        free(session);
        return result;
    }

    result = deadline_remaining(&deadline, &remaining);
    if (result != MG_OK) return close_failed_connection(session, result);
    result = resolve_ipv4(server_name, name_length, remaining, &address,
                          &is_ip);
    if (result != MG_OK) return close_failed_connection(session, result);

    endpoint.address = address;
    endpoint.port = port;
    endpoint.reserved = 0;
    result = deadline_remaining(&deadline, &remaining);
    if (result != MG_OK) return close_failed_connection(session, result);
    mg_handle_t stream = 0;
    result = mg_stream_connect(&endpoint, remaining, &stream);
    if (result != MG_OK) return close_failed_connection(session, result);
    return mgtls_session_handshake(session, context, server_name, name_length,
                                   is_ip, stream, &deadline, out_session);
}

mg_result_t mgtls_session_connect_stream(mgtls_context_t *context,
                                         const char *server_name,
                                         mg_handle_t stream,
                                         u32 timeout_ms,
                                         mgtls_session_t **out_session)
{
    mgtls_session_t *session = NULL;
    mgtls_deadline_t deadline;
    mg_ipv4_addr_t address;
    usize name_length;
    bool is_ip;
    mg_result_t result;

    if (out_session) *out_session = NULL;
    if (!stream) return MG_ERR_BAD_ARGUMENT;
    session = (mgtls_session_t *)calloc(1U, sizeof(*session));
    if (!session) {
        (void)mg_stream_close(stream);
        return MG_ERR_NO_MEMORY;
    }
    session->stream = stream;
    session->owns_stream = true;
    if (!out_session || !context || !server_name) {
        return close_failed_connection(session, MG_ERR_BAD_ARGUMENT);
    }
    name_length = bounded_string_length(server_name,
                                         MGTLS_HOSTNAME_MAX_BYTES + 1U);
    if (name_length == 0U || name_length > MGTLS_HOSTNAME_MAX_BYTES)
        return close_failed_connection(session, MG_ERR_BAD_ARGUMENT);
    result = mgtls_platform_check_trusted_time();
    if (result != MG_OK) return close_failed_connection(session, result);
    result = deadline_start(timeout_ms, &deadline);
    if (result != MG_OK) return close_failed_connection(session, result);
    is_ip = mg_ipv4_parse(server_name, &address);
    return mgtls_session_handshake(session, context, server_name, name_length,
                                   is_ip, stream, &deadline, out_session);
}

static mg_result_t begin_session_operation(mgtls_session_t *session,
                                           u32 timeout_ms)
{
    if (!session || !session->wolf || !session->handshake_complete)
        return MG_ERR_BAD_ARGUMENT;
    if (session->poisoned) return MG_ERR_TLS_PROTOCOL;
    session->io_error = MG_OK;
    return deadline_start(timeout_ms, &session->deadline);
}

mg_result_t mgtls_session_send(mgtls_session_t *session, const void *data,
                               usize length, u32 timeout_ms,
                               usize *out_transferred)
{
    mg_result_t result;
    int ret;
    int error;
    int request;

    if (!out_transferred) return MG_ERR_BAD_ARGUMENT;
    *out_transferred = 0;
    if (!session || (!data && length != 0U)) return MG_ERR_BAD_ARGUMENT;
    if (length == 0U) return MG_OK;
    result = begin_session_operation(session, timeout_ms);
    if (result != MG_OK) return result;
    request = length > (usize)INT_MAX ? INT_MAX : (int)length;
    mgtls_platform_begin_tls_operation();
    ret = wolfSSL_write(session->wolf, data, request);
    if (ret > 0) {
        *out_transferred = (usize)ret;
        return MG_OK;
    }
    result = mgtls_ssl_failure(session, ret);
    error = wolfSSL_get_error(session->wolf, ret);
    if (error != WOLFSSL_ERROR_WANT_READ &&
        error != WOLFSSL_ERROR_WANT_WRITE)
        session->poisoned = true;
    else
        /* wolfSSL requires retrying a pending write with the exact same
         * application buffer. This API cannot retain the caller's pointer. */
        session->poisoned = true;
    return result;
}

mg_result_t mgtls_session_receive(mgtls_session_t *session, void *data,
                                  usize capacity, u32 timeout_ms,
                                  usize *out_transferred)
{
    mg_result_t result;
    int request;
    int ret;

    if (!out_transferred) return MG_ERR_BAD_ARGUMENT;
    *out_transferred = 0;
    if (!session || (!data && capacity != 0U)) return MG_ERR_BAD_ARGUMENT;
    if (capacity == 0U) return MG_OK;
    result = begin_session_operation(session, timeout_ms);
    if (result != MG_OK) return result;
    request = capacity > (usize)INT_MAX ? INT_MAX : (int)capacity;
    mgtls_platform_begin_tls_operation();
    ret = wolfSSL_read(session->wolf, data, request);
    if (ret > 0) {
        *out_transferred = (usize)ret;
        return MG_OK;
    }
    result = mgtls_ssl_failure(session, ret);
    if (result != MG_ERR_TIMEOUT && result != MG_ERR_INTERRUPTED &&
        result != MG_ERR_WOULD_BLOCK)
        session->poisoned = true;
    return result;
}

mg_result_t mgtls_session_shutdown(mgtls_session_t *session,
                                   u32 timeout_ms)
{
    mg_result_t result;
    int ret;

    if (!session || !session->wolf || !session->handshake_complete)
        return MG_ERR_BAD_ARGUMENT;
    if (session->shutdown_complete) return MG_OK;
    if (session->poisoned) return MG_ERR_TLS_PROTOCOL;
    result = deadline_start(timeout_ms, &session->deadline);
    if (result != MG_OK) return result;
    mgtls_platform_begin_tls_operation();
    ret = wolfSSL_shutdown(session->wolf);
    if (ret >= 0) {
        session->shutdown_complete = true;
        return MG_OK;
    }
    result = mgtls_ssl_failure(session, ret);
    if (wolfSSL_get_shutdown(session->wolf) & WOLFSSL_SENT_SHUTDOWN) {
        session->shutdown_complete = true;
        return MG_OK;
    }
    return result;
}

mg_result_t mgtls_session_close(mgtls_session_t *session)
{
    mg_result_t shutdown_result = MG_OK;
    mg_result_t close_result = MG_OK;

    if (!session) return MG_ERR_BAD_ARGUMENT;
    if (session->handshake_complete && !session->shutdown_complete &&
        !session->poisoned)
        shutdown_result = mgtls_session_shutdown(session,
                                                  MGTLS_TIMEOUT_DEFAULT);
    if (session->owns_stream) {
        close_result = mg_stream_close(session->stream);
        if (close_result == MG_ERR_BUSY) return close_result;
        session->owns_stream = false;
    }
    if (session->wolf) wolfSSL_free(session->wolf);
    if (session->context && session->context->sessions != 0U)
        --session->context->sessions;
    free(session);
    return shutdown_result != MG_OK ? shutdown_result : close_result;
}
