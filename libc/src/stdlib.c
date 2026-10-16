/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct parsed_integer {
    uintmax_t magnitude;
    const char *end;
    bool negative;
    bool any;
    bool overflow;
} parsed_integer_t;

static int digit_value(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

static bool digit_for_base(unsigned char c, int base)
{
    int digit = digit_value(c);
    return digit >= 0 && digit < base;
}

static parsed_integer_t parse_integer(const char *text, int base,
                                      uintmax_t limit)
{
    parsed_integer_t parsed = { 0, text, false, false, false };
    const char *cursor = text;

    if (!text || (base != 0 && (base < 2 || base > 36))) {
        errno = EINVAL;
        return parsed;
    }
    while (isspace((unsigned char)*cursor)) cursor++;
    if (*cursor == '+' || *cursor == '-') {
        parsed.negative = *cursor == '-';
        cursor++;
    }
    if ((base == 0 || base == 16) && cursor[0] == '0' &&
        (cursor[1] == 'x' || cursor[1] == 'X') &&
        digit_for_base((unsigned char)cursor[2], 16)) {
        base = 16;
        cursor += 2;
    } else if (base == 0) {
        base = cursor[0] == '0' ? 8 : 10;
    }

    while (digit_for_base((unsigned char)*cursor, base)) {
        uintmax_t digit = (uintmax_t)digit_value((unsigned char)*cursor);
        parsed.any = true;
        if (parsed.magnitude > (limit - digit) / (uintmax_t)base) {
            parsed.overflow = true;
        } else if (!parsed.overflow) {
            parsed.magnitude = parsed.magnitude * (uintmax_t)base + digit;
        }
        cursor++;
    }
    if (!parsed.any) parsed.end = text;
    else parsed.end = cursor;
    return parsed;
}

static void update_end(char **end, const char *value)
{
    if (end) *end = (char *)value;
}

static intmax_t signed_value(parsed_integer_t parsed, intmax_t minimum,
                             intmax_t maximum, char **end)
{
    uintmax_t limit = parsed.negative ? (uintmax_t)maximum + 1U :
                                        (uintmax_t)maximum;
    intmax_t result;
    update_end(end, parsed.end);
    if (!parsed.any) return 0;
    if (parsed.overflow || parsed.magnitude > limit) {
        errno = ERANGE;
        return parsed.negative ? minimum : maximum;
    }
    if (parsed.negative) {
        if (parsed.magnitude == (uintmax_t)maximum + 1U) return minimum;
        result = -(intmax_t)parsed.magnitude;
    } else {
        result = (intmax_t)parsed.magnitude;
    }
    return result;
}

static uintmax_t unsigned_value(parsed_integer_t parsed, uintmax_t maximum,
                               char **end)
{
    update_end(end, parsed.end);
    if (!parsed.any) return 0;
    if (parsed.overflow || parsed.magnitude > maximum) {
        errno = ERANGE;
        return maximum;
    }
    return parsed.negative ? (uintmax_t)0 - parsed.magnitude :
                             parsed.magnitude;
}

long strtol(const char *text, char **end, int base)
{
    parsed_integer_t parsed = parse_integer(text, base, (uintmax_t)LONG_MAX + 1U);
    return (long)signed_value(parsed, LONG_MIN, LONG_MAX, end);
}

unsigned long strtoul(const char *text, char **end, int base)
{
    parsed_integer_t parsed = parse_integer(text, base, ULONG_MAX);
    return (unsigned long)unsigned_value(parsed, ULONG_MAX, end);
}

long long strtoll(const char *text, char **end, int base)
{
    parsed_integer_t parsed = parse_integer(text, base,
                                             (uintmax_t)LLONG_MAX + 1U);
    return (long long)signed_value(parsed, LLONG_MIN, LLONG_MAX, end);
}

unsigned long long strtoull(const char *text, char **end, int base)
{
    parsed_integer_t parsed = parse_integer(text, base, ULLONG_MAX);
    return (unsigned long long)unsigned_value(parsed, ULLONG_MAX, end);
}

intmax_t strtoimax(const char *text, char **end, int base)
{
    parsed_integer_t parsed = parse_integer(text, base,
                                             (uintmax_t)INTMAX_MAX + 1U);
    return signed_value(parsed, INTMAX_MIN, INTMAX_MAX, end);
}

intmax_t imaxabs(intmax_t value) { return value < 0 ? -value : value; }

imaxdiv_t imaxdiv(intmax_t numerator, intmax_t denominator)
{
    imaxdiv_t result = { numerator / denominator, numerator % denominator };
    return result;
}

uintmax_t strtoumax(const char *text, char **end, int base)
{
    parsed_integer_t parsed = parse_integer(text, base, UINTMAX_MAX);
    return unsigned_value(parsed, UINTMAX_MAX, end);
}

int atoi(const char *text) { return (int)strtol(text, NULL, 10); }
long atol(const char *text) { return strtol(text, NULL, 10); }
long long atoll(const char *text) { return strtoll(text, NULL, 10); }

int abs(int value) { return value < 0 ? -value : value; }
long labs(long value) { return value < 0 ? -value : value; }
long long llabs(long long value) { return value < 0 ? -value : value; }

div_t div(int numerator, int denominator)
{
    div_t result = { numerator / denominator, numerator % denominator };
    return result;
}

ldiv_t ldiv(long numerator, long denominator)
{
    ldiv_t result = { numerator / denominator, numerator % denominator };
    return result;
}

lldiv_t lldiv(long long numerator, long long denominator)
{
    lldiv_t result = { numerator / denominator, numerator % denominator };
    return result;
}

static unsigned int random_state = 1U;

int rand(void)
{
    random_state = random_state * 1103515245U + 12345U;
    return (int)((random_state >> 16) & RAND_MAX);
}

void srand(unsigned int seed) { random_state = seed; }

char *getenv(const char *name)
{
    (void)name;
    return NULL;
}

int system(const char *command)
{
    if (!command) return 0; /* This runtime has no command processor. */
    errno = ENOSYS;
    return -1;
}

void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *))
{
    const unsigned char *bytes = (const unsigned char *)base;
    size_t low = 0, high = count;
    if (!compare || (count && (!base || !size || count > SIZE_MAX / size)))
        return NULL;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = compare(key, bytes + middle * size);
        if (order == 0) return (void *)(bytes + middle * size);
        if (order < 0) high = middle;
        else low = middle + 1;
    }
    return NULL;
}

static void swap_bytes(unsigned char *left, unsigned char *right, size_t size)
{
    while (size--) {
        unsigned char temporary = *left;
        *left++ = *right;
        *right++ = temporary;
    }
}

static void sift_down(unsigned char *base, size_t root, size_t count,
                      size_t size, int (*compare)(const void *, const void *))
{
    while (count > 1 && root <= (count - 2) / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count &&
            compare(base + child * size, base + (child + 1) * size) < 0)
            child++;
        if (compare(base + root * size, base + child * size) >= 0) return;
        swap_bytes(base + root * size, base + child * size, size);
        root = child;
    }
}

void qsort(void *array, size_t count, size_t size,
           int (*compare)(const void *, const void *))
{
    unsigned char *base = (unsigned char *)array;
    if (count < 2 || size == 0 || !base || !compare ||
        count > SIZE_MAX / size) return;
    for (size_t root = count / 2; root != 0; )
        sift_down(base, --root, count, size, compare);
    for (size_t end = count; end > 1; ) {
        swap_bytes(base, base + (--end) * size, size);
        sift_down(base, 0, end, size, compare);
    }
}
