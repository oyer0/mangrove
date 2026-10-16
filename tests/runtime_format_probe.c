/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef long long mg_result_t;
typedef unsigned int mg_handle_t;
typedef unsigned long long mg_size_t;
extern int probe_snprintf(char *, size_t, const char *, ...);
extern int probe_printf(const char *, ...);
extern int probe_putchar(int);
extern int probe_puts(const char *);
extern int *probe_errno_location(void);
extern int mg_error_to_errno(mg_result_t);
extern mg_result_t object_write(mg_handle_t, const void *, mg_size_t);

static char captured[4096];
static size_t captured_size;
static int write_mode = 1;

mg_result_t object_write(mg_handle_t handle, const void *data, mg_size_t size)
{
    (void)handle;
    if (write_mode < 0) return -12;
    if (write_mode == 0) return 0;
    assert(size <= sizeof(captured) - captured_size);
    memcpy(captured + captured_size, data, (size_t)size);
    captured_size += (size_t)size;
    return (mg_result_t)size;
}

#define CHECK_FORMAT(format, ...) do { \
    char expected[512], actual[512]; \
    const char *format_text = (format); \
    int n = snprintf(expected, sizeof(expected), format_text, __VA_ARGS__); \
    int m = probe_snprintf(actual, sizeof(actual), format_text, __VA_ARGS__); \
    assert(n == m && !memcmp(expected, actual, (size_t)n + 1)); \
} while (0)

int main(void)
{
    char expected[512], actual[512];
    int n, m, count = -1;
    const void *pointer = (const void *)&count;

    CHECK_FORMAT("%hhd|%hd|%d|%ld|%lld|%jd|%zd|%td",
        (int)-128, (int)-32768, INT_MIN, LONG_MIN, LLONG_MIN,
        INTMAX_MIN, (ptrdiff_t)-4294967297L, (ptrdiff_t)-17);
    CHECK_FORMAT("%hhu|%hu|%u|%lu|%llu|%ju|%zu|%tu",
        (int)255, (int)65535, UINT_MAX, ULONG_MAX, ULLONG_MAX,
        UINTMAX_MAX, SIZE_MAX, (size_t)4294967297UL);
    CHECK_FORMAT("%#o|%#x|%#X|%+08d|% 8d|%-9.5s|%c",
        0U, 42U, 42U, -42, 7, "alphabet", 'Z');
    CHECK_FORMAT("%#.0o|%#.0x|%08.5u|%.0u", 0U, 0U, 42U, 0U);
    CHECK_FORMAT("%p", pointer);
    assert(probe_snprintf(actual, sizeof(actual), "%p", (void *)0) == 3);
    assert(!strcmp(actual, "0x0"));

    n = snprintf(expected, sizeof(expected), "prefix:%ld", 4294967297L);
    m = probe_snprintf(actual, sizeof(actual), "prefix:%ld", 4294967297L);
    assert(n == m && !strcmp(expected, actual));

    for (size_t capacity = 0; capacity < 24; capacity++) {
        memset(expected, 0xa5, sizeof(expected));
        memset(actual, 0xa5, sizeof(actual));
        n = snprintf(expected, capacity, "%#08x:%s", 42U, "bounded");
        m = probe_snprintf(actual, capacity, "%#08x:%s", 42U, "bounded");
        assert(n == m && !memcmp(expected, actual, sizeof(expected)));
    }
    memset(expected, 0xa5, sizeof(expected));
    memset(actual, 0xa5, sizeof(actual));
    n = snprintf(expected, 0, "%ld", 19L);
    m = probe_snprintf(actual, 0, "%ld", 19L);
    assert(n == m && expected[0] == (char)0xa5 && actual[0] == (char)0xa5);

    n = snprintf(expected, sizeof(expected), "ab%ncd", &count);
    m = probe_snprintf(actual, sizeof(actual), "ab%ncd", &count);
    assert(n == 4 && m == 4 && count == 2 && !strcmp(expected, actual));

    *probe_errno_location() = 0;
    assert(probe_snprintf(actual, sizeof(actual), "%q", 1) == -1);
    assert(*probe_errno_location() == EINVAL);
    assert(probe_snprintf(actual, sizeof(actual), "%") == -1);
    assert(*probe_errno_location() == EINVAL);
    assert(probe_snprintf(actual, sizeof(actual), "%f", 1.0) == -1);
    assert(*probe_errno_location() == ENOSYS);
    assert(errno == 0); /* The C library has its own process errno cell. */
    assert(mg_error_to_errno(-33) == EIO && mg_error_to_errno(-31) == ENODEV);

    captured_size = 0; write_mode = 1;
    assert(probe_printf("value=%ld", 4294967297L) == 16);
    assert(captured_size == 16 && !memcmp(captured, "value=4294967297", 16));
    write_mode = -1;
    assert(probe_printf("failure") == EOF && *probe_errno_location() == EIO);
    write_mode = 0;
    assert(probe_putchar('x') == EOF && *probe_errno_location() == EIO);
    (void)probe_puts;
    puts("production integer formatter and native error boundary PASS");
    return 0;
}
