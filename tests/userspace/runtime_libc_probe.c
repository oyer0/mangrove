/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern int *probe_errno_location(void);
extern long probe_strtol(const char *, char **, int);
extern unsigned long probe_strtoul(const char *, char **, int);
extern long long probe_strtoll(const char *, char **, int);
extern unsigned long long probe_strtoull(const char *, char **, int);
extern intmax_t probe_strtoimax(const char *, char **, int);
extern uintmax_t probe_strtoumax(const char *, char **, int);
extern void probe_qsort(void *, size_t, size_t, int (*)(const void *, const void *));
extern void *probe_bsearch(const void *, const void *, size_t, size_t,
                           int (*)(const void *, const void *));
extern int probe_rand(void);
extern void probe_srand(unsigned int);
extern int probe_system(const char *);
extern char *probe_getenv(const char *);

extern int probe_isalnum(int), probe_isalpha(int), probe_isblank(int);
extern int probe_iscntrl(int), probe_isdigit(int), probe_isgraph(int);
extern int probe_islower(int), probe_isprint(int), probe_ispunct(int);
extern int probe_isspace(int), probe_isupper(int), probe_isxdigit(int);
extern int probe_tolower(int), probe_toupper(int);

static int compare_int(const void *left, const void *right)
{
    int a = *(const int *)left, b = *(const int *)right;
    return (a > b) - (a < b);
}

static void compare_signed(const char *text, int base)
{
    char *host_end, *target_end;
    long host_value, target_value;
    errno = 0;
    host_value = strtol(text, &host_end, base);
    int host_error = errno;
    *probe_errno_location() = 0;
    target_value = probe_strtol(text, &target_end, base);
    assert(host_value == target_value && host_end - text == target_end - text);
    assert(host_error == *probe_errno_location());
}

static void compare_unsigned(const char *text, int base)
{
    char *host_end, *target_end;
    unsigned long host_value, target_value;
    errno = 0;
    host_value = strtoul(text, &host_end, base);
    int host_error = errno;
    *probe_errno_location() = 0;
    target_value = probe_strtoul(text, &target_end, base);
    assert(host_value == target_value && host_end - text == target_end - text);
    assert(host_error == *probe_errno_location());
}

int main(void)
{
    static const char *const values[] = {
        "0", "00", "09", "123xyz", "  -0x7f!", "+077tail",
        "0XfFz", "no digits", "9223372036854775808",
        "-9223372036854775809", "18446744073709551616",
        "-18446744073709551615"
    };
    static const int bases[] = { 0, 2, 8, 10, 16, 36 };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
        for (size_t j = 0; j < sizeof(bases) / sizeof(bases[0]); j++) {
            compare_signed(values[i], bases[j]);
            compare_unsigned(values[i], bases[j]);
        }

    *probe_errno_location() = 0;
    assert(probe_strtol("17", NULL, 1) == 0 &&
           *probe_errno_location() == EINVAL);
    errno = 0;
    assert(probe_strtoll("9223372036854775808", NULL, 10) == LLONG_MAX &&
           *probe_errno_location() == ERANGE);
    assert(probe_strtoull("18446744073709551616", NULL, 10) == ULLONG_MAX &&
           *probe_errno_location() == ERANGE);
    assert(probe_strtoimax("-9223372036854775808", NULL, 10) == INTMAX_MIN);
    assert(probe_strtoumax("18446744073709551615", NULL, 10) == UINTMAX_MAX);

    for (int c = -1; c <= 255; c++) {
        assert(!!probe_isalnum(c) == !!isalnum(c));
        assert(!!probe_isalpha(c) == !!isalpha(c));
        assert(!!probe_isblank(c) == !!isblank(c));
        assert(!!probe_iscntrl(c) == !!iscntrl(c));
        assert(!!probe_isdigit(c) == !!isdigit(c));
        assert(!!probe_isgraph(c) == !!isgraph(c));
        assert(!!probe_islower(c) == !!islower(c));
        assert(!!probe_isprint(c) == !!isprint(c));
        assert(!!probe_ispunct(c) == !!ispunct(c));
        assert(!!probe_isspace(c) == !!isspace(c));
        assert(!!probe_isupper(c) == !!isupper(c));
        assert(!!probe_isxdigit(c) == !!isxdigit(c));
        assert(probe_tolower(c) == tolower(c));
        assert(probe_toupper(c) == toupper(c));
    }

    int values_to_sort[] = { 7, -3, 7, INT_MIN, 0, INT_MAX, -3 };
    probe_qsort(values_to_sort,
                sizeof(values_to_sort) / sizeof(values_to_sort[0]),
                sizeof(values_to_sort[0]), compare_int);
    for (size_t i = 1; i < sizeof(values_to_sort) / sizeof(values_to_sort[0]); i++)
        assert(values_to_sort[i - 1] <= values_to_sort[i]);
    int key = 7;
    assert(probe_bsearch(&key, values_to_sort,
        sizeof(values_to_sort) / sizeof(values_to_sort[0]),
        sizeof(values_to_sort[0]), compare_int) != NULL);

    probe_srand(1);
    assert(probe_rand() == 16838 && probe_rand() == 5758 &&
           probe_rand() == 10113);
    assert(probe_system(NULL) == 0);
    *probe_errno_location() = 0;
    assert(probe_system("true") == -1 && *probe_errno_location() == ENOSYS);
    assert(probe_getenv("PATH") == NULL);
    assert(probe_getenv(NULL) == NULL);
    return 0;
}
