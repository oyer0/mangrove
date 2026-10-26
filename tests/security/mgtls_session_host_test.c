/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <mg/tls.h>
#include <mg/net.h>
#include <mg/object.h>
#include <mg/time.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* Keep this host fixture on the host clock ABI; libc/include/time.h is the
 * Mangrove target header and cannot coexist with glibc's struct timespec. */
extern int clock_gettime(int clock_id, struct timespec *sample);
extern time_t time(time_t *timer);

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

static bool test_clock_trusted = true;
static bool interrupt_receive;

mg_result_t mg_random_status(void) { return MG_OK; }

mg_result_t mg_random_bytes(void *buffer, usize length)
{
    int fd;
    usize copied = 0;
    if (!buffer && length) return MG_ERR_BAD_ARGUMENT;
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return MG_ERR_ENTROPY_UNAVAILABLE;
    while (copied < length) {
        ssize_t amount = read(fd, (unsigned char *)buffer + copied,
                              (size_t)(length - copied));
        if (amount <= 0) {
            close(fd);
            return MG_ERR_ENTROPY_UNAVAILABLE;
        }
        copied += (usize)amount;
    }
    close(fd);
    return MG_OK;
}

mg_result_t mg_clock_monotonic(mg_monotonic_time_t *out)
{
    struct timespec sample;
    if (!out || clock_gettime(CLOCK_MONOTONIC, &sample) != 0)
        return MG_ERR_IO;
    out->milliseconds = (u64)sample.tv_sec * 1000U +
                        (u64)sample.tv_nsec / 1000000U;
    return MG_OK;
}

mg_result_t mg_clock_realtime_info(mg_realtime_info_t *out)
{
    time_t now;
    if (!out) return MG_ERR_BAD_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!test_clock_trusted) {
        out->state = MG_REALTIME_UNTRUSTED;
        return MG_OK;
    }
    now = time(NULL);
    if (now == (time_t)-1) {
        out->state = MG_REALTIME_UNAVAILABLE;
        return MG_OK;
    }
    out->state = MG_REALTIME_TRUSTED;
    out->time.seconds = (i64)now - 946684800LL;
    out->resolution_ns = 1000000000U;
    return MG_OK;
}

mg_result_t mangrove_time_to_unix(const mg_mangrove_time_t *value,
                                  mg_unix_time_t *out)
{
    if (!value || !out || value->nanoseconds >= 1000000000U)
        return MG_ERR_BAD_ARGUMENT;
    if (value->seconds > INT64_MAX - 946684800LL)
        return MG_ERR_BAD_ARGUMENT;
    out->seconds = value->seconds + 946684800LL;
    out->nanoseconds = value->nanoseconds;
    out->reserved = 0;
    return MG_OK;
}

static const char *test_path(const char *path)
{
    if (strcmp(path, MGTLS_SYSTEM_CA_BUNDLE) == 0) {
        const char *override = getenv("MGTLS_TEST_CA_BUNDLE");
        return override ? override : path;
    }
    if (strcmp(path, MGTLS_SYSTEM_CA_MANIFEST) == 0) {
        const char *override = getenv("MGTLS_TEST_CA_MANIFEST");
        return override ? override : path;
    }
    return path;
}

mg_result_t file_open(const char *path, u32 flags)
{
    int fd;
    if (!path || flags != MG_OPEN_READ) return MG_ERR_BAD_ARGUMENT;
    fd = open(test_path(path), O_RDONLY);
    return fd < 0 ? MG_ERR_NOT_FOUND : (mg_result_t)fd;
}

mg_result_t object_read(mg_handle_t handle, void *buffer, usize length)
{
    ssize_t result;
    if (!buffer && length) return MG_ERR_BAD_ARGUMENT;
    result = read((int)handle, buffer, (size_t)length);
    return result < 0 ? MG_ERR_IO : (mg_result_t)result;
}

mg_result_t handle_close(mg_handle_t handle)
{
    return close((int)handle) == 0 ? MG_OK : MG_ERR_IO;
}

bool mg_ipv4_parse(const char *text, mg_ipv4_addr_t *out)
{
    struct in_addr address;
    if (!text || !out || inet_pton(AF_INET, text, &address) != 1)
        return false;
    memcpy(out->octet, &address.s_addr, sizeof(out->octet));
    return true;
}

mg_result_t mg_net_resolve_a(const char *hostname, mg_ipv4_addr_t *out,
                             u32 timeout_ms)
{
    (void)timeout_ms;
    if (!hostname || !out) return MG_ERR_BAD_ARGUMENT;
    /* Every fixture hostname resolves to the local fixture listener. */
    out->octet[0] = 127U;
    out->octet[1] = 0U;
    out->octet[2] = 0U;
    out->octet[3] = 1U;
    return MG_OK;
}

mg_result_t mg_stream_connect(const mg_net_endpoint_t *remote,
                              u32 timeout_ms, mg_handle_t *out_handle)
{
    struct sockaddr_in address;
    int fd;
    int result;
    (void)timeout_ms;
    if (!remote || !out_handle || remote->port == 0U)
        return MG_ERR_BAD_ARGUMENT;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return MG_ERR_NETWORK_UNAVAILABLE;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(remote->port);
    memcpy(&address.sin_addr.s_addr, remote->address.octet, 4U);
    result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (result != 0) {
        close(fd);
        return MG_ERR_NETWORK_UNAVAILABLE;
    }
    *out_handle = (mg_handle_t)fd;
    return MG_OK;
}

static mg_result_t wait_socket(mg_handle_t handle, short events,
                               u32 timeout_ms)
{
    struct pollfd descriptor;
    int result;
    descriptor.fd = (int)handle;
    descriptor.events = events;
    descriptor.revents = 0;
    result = poll(&descriptor, 1U,
                  timeout_ms > (u32)INT32_MAX ? INT32_MAX : (int)timeout_ms);
    if (result == 0) return MG_ERR_TIMEOUT;
    if (result < 0) return MG_ERR_INTERRUPTED;
    if (descriptor.revents & (POLLERR | POLLNVAL))
        return MG_ERR_CONNECTION_RESET;
    if (!(descriptor.revents & events) && !(descriptor.revents & POLLHUP))
        return MG_ERR_IO;
    return MG_OK;
}

mg_result_t mg_stream_send(mg_handle_t handle, const void *data, usize length,
                           u32 timeout_ms)
{
    mg_result_t result;
    ssize_t sent;
    if (!data && length) return MG_ERR_BAD_ARGUMENT;
    result = wait_socket(handle, POLLOUT, timeout_ms);
    if (result != MG_OK) return result;
    sent = send((int)handle, data, (size_t)length, 0);
    if (sent > 0) return (mg_result_t)sent;
    return MG_ERR_CONNECTION_RESET;
}

mg_result_t mg_stream_receive(mg_handle_t handle, void *data, usize capacity,
                              u32 timeout_ms)
{
    mg_result_t result;
    ssize_t received;
    if (!data && capacity) return MG_ERR_BAD_ARGUMENT;
    if (interrupt_receive) {
        interrupt_receive = false;
        return MG_ERR_INTERRUPTED;
    }
    result = wait_socket(handle, POLLIN, timeout_ms);
    if (result != MG_OK) return result;
    received = recv((int)handle, data, (size_t)capacity, 0);
    if (received > 0) return (mg_result_t)received;
    if (received == 0) return MG_ERR_CONNECTION_CLOSED;
    return MG_ERR_CONNECTION_RESET;
}

mg_result_t mg_stream_close(mg_handle_t handle)
{
    return close((int)handle) == 0 ? MG_OK : MG_ERR_IO;
}

static int expected_result(const char *text, mg_result_t *out)
{
    char *end = NULL;
    long long value;
    if (!text || !out) return 0;
    value = strtoll(text, &end, 10);
    if (!end || *end) return 0;
    *out = (mg_result_t)value;
    return 1;
}

static int exchange(mgtls_session_t *session, const char *mode)
{
    static const unsigned char request[] = {'P','I','N','G'};
    static const unsigned char response[] = {'P','O','N','G'};
    unsigned char received[sizeof(response)];
    usize sent_total = 0;
    usize received_total = 0;

    while (sent_total < sizeof(request)) {
        usize sent = 0;
        mg_result_t result = mgtls_session_send(session, request + sent_total,
            sizeof(request) - sent_total, 2000U, &sent);
        if (result != MG_OK || sent == 0U) return 0;
        sent_total += sent;
    }
    if (strcmp(mode, "reset") == 0) {
        usize amount = 0;
        mg_result_t result = mgtls_session_receive(session, received,
            sizeof(received), 2000U, &amount);
        return result == MG_ERR_CONNECTION_RESET ||
               result == MG_ERR_CONNECTION_CLOSED;
    }
    while (received_total < sizeof(response)) {
        usize amount = 0;
        mg_result_t result = mgtls_session_receive(session,
            received + received_total, sizeof(received) - received_total,
            2000U, &amount);
        if (result != MG_OK || amount == 0U) return 0;
        received_total += amount;
    }
    (void)mgtls_session_shutdown(session, 1000U);
    return memcmp(received, response, sizeof(response)) == 0;
}

int main(int argc, char **argv)
{
    const char *mode;
    const char *host;
    u16 port;
    u32 timeout_ms = 2000U;
    mg_result_t expected;
    mg_result_t result;
    mgtls_context_t *context = NULL;
    mgtls_session_t *session = NULL;

    if (argc < 5 || !expected_result(argv[4], &expected)) return 2;
    mode = argv[1];
    host = argv[2];
    port = (u16)strtoul(argv[3], NULL, 10);
    if (argc > 5) timeout_ms = (u32)strtoul(argv[5], NULL, 10);
    if (strcmp(mode, "untrusted-time") == 0) test_clock_trusted = false;
    if (strcmp(mode, "interrupted") == 0) interrupt_receive = true;

    result = mgtls_init();
    if (result != MG_OK) return 3;
    result = mgtls_context_create(&context);
    if (strcmp(mode, "context") == 0 || result != MG_OK) {
        if (result == expected) {
            (void)mgtls_cleanup();
            return 0;
        }
        if (context) (void)mgtls_context_destroy(context);
        (void)mgtls_cleanup();
        fprintf(stderr, "context result %lld, expected %lld\n",
                (long long)result, (long long)expected);
        return 4;
    }
    result = mgtls_session_connect(context, host, port, timeout_ms, &session);
    if (result != expected &&
        !(strcmp(mode, "reset") == 0 && result == MG_OK &&
          expected == MG_ERR_CONNECTION_RESET)) {
        fprintf(stderr, "connect result %lld, expected %lld\n",
                (long long)result, (long long)expected);
        if (session) (void)mgtls_session_close(session);
        (void)mgtls_context_destroy(context);
        (void)mgtls_cleanup();
        return 5;
    }
    if (result == MG_OK) {
        if (!session || !exchange(session, mode)) {
            if (session) (void)mgtls_session_close(session);
            (void)mgtls_context_destroy(context);
            (void)mgtls_cleanup();
            return 6;
        }
        result = mgtls_session_close(session);
        session = NULL;
        if (result != MG_OK) return 7;
    }
    if (mgtls_context_destroy(context) != MG_OK ||
        mgtls_cleanup() != MG_OK)
        return 8;
    return 0;
}
