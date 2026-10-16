/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "floatconv.h"

typedef struct scan_input {
    FILE *stream;
    const char *string;
    size_t offset;
    size_t consumed;
    bool io_error;
    bool reached_eof;
} scan_input_t;

typedef enum scan_length {
    SCAN_DEFAULT, SCAN_HH, SCAN_H, SCAN_L, SCAN_LL,
    SCAN_J, SCAN_Z, SCAN_T, SCAN_CAP_L
} scan_length_t;

#define SCAN_FLOAT_TOKEN_MAX 65536U

static int input_get(scan_input_t *input)
{
    int value;
    if (input->stream) {
        value = fgetc(input->stream);
        if (value == EOF && ferror(input->stream)) input->io_error = true;
    } else {
        unsigned char byte = (unsigned char)input->string[input->offset];
        value = byte ? byte : EOF;
        if (value != EOF) input->offset++;
    }
    if (value != EOF) input->consumed++;
    else input->reached_eof = true;
    return value;
}

static void input_unget(scan_input_t *input, int value)
{
    if (value == EOF) return;
    if (input->stream) {
        if (ungetc(value, input->stream) == EOF) input->io_error = true;
    } else if (input->offset) {
        input->offset--;
    }
    if (input->consumed) input->consumed--;
    input->reached_eof = false;
}

static int input_failure(scan_input_t *input)
{
    return input->io_error || input->reached_eof ||
           (input->stream && ferror(input->stream));
}

static bool format_decimal(const char **format, int *value)
{
    unsigned int number = 0;
    const char *cursor = *format;
    while (*cursor >= '0' && *cursor <= '9') {
        unsigned int digit = (unsigned int)(*cursor - '0');
        if (number > ((unsigned int)INT_MAX - digit) / 10U) return false;
        number = number * 10U + digit;
        cursor++;
    }
    *format = cursor;
    *value = (int)number;
    return true;
}

static int digit_value(int value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'z') return value - 'a' + 10;
    if (value >= 'A' && value <= 'Z') return value - 'A' + 10;
    return -1;
}

static uintmax_t signed_limit(scan_length_t length, bool negative)
{
    switch (length) {
    case SCAN_HH: return negative ? (uintmax_t)SCHAR_MAX + 1U : SCHAR_MAX;
    case SCAN_H: return negative ? (uintmax_t)SHRT_MAX + 1U : SHRT_MAX;
    case SCAN_L: return negative ? (uintmax_t)LONG_MAX + 1U : LONG_MAX;
    case SCAN_LL: return negative ? (uintmax_t)LLONG_MAX + 1U : LLONG_MAX;
    case SCAN_J: return negative ? (uintmax_t)INTMAX_MAX + 1U : INTMAX_MAX;
    case SCAN_Z: case SCAN_T:
        return negative ? (uintmax_t)PTRDIFF_MAX + 1U : PTRDIFF_MAX;
    default: return negative ? (uintmax_t)INT_MAX + 1U : INT_MAX;
    }
}

static uintmax_t unsigned_limit(scan_length_t length)
{
    switch (length) {
    case SCAN_HH: return UCHAR_MAX;
    case SCAN_H: return USHRT_MAX;
    case SCAN_L: return ULONG_MAX;
    case SCAN_LL: return ULLONG_MAX;
    case SCAN_J: return UINTMAX_MAX;
    case SCAN_Z: case SCAN_T: return SIZE_MAX;
    default: return UINT_MAX;
    }
}

static void store_signed(va_list *arguments, scan_length_t length, bool negative,
                         uintmax_t magnitude)
{
    intmax_t value;
    if (negative) {
        uintmax_t min_magnitude = signed_limit(length, true);
        value = magnitude >= min_magnitude
            ? (length == SCAN_HH ? SCHAR_MIN : length == SCAN_H ? SHRT_MIN :
               length == SCAN_L ? LONG_MIN : length == SCAN_LL ? LLONG_MIN :
               length == SCAN_J ? INTMAX_MIN : length == SCAN_Z || length == SCAN_T
                   ? PTRDIFF_MIN : INT_MIN)
            : -(intmax_t)magnitude;
    } else value = (intmax_t)magnitude;
    switch (length) {
    case SCAN_HH: *va_arg(*arguments, signed char *) = (signed char)value; break;
    case SCAN_H: *va_arg(*arguments, short *) = (short)value; break;
    case SCAN_L: *va_arg(*arguments, long *) = (long)value; break;
    case SCAN_LL: *va_arg(*arguments, long long *) = (long long)value; break;
    case SCAN_J: *va_arg(*arguments, intmax_t *) = (intmax_t)value; break;
    case SCAN_Z: case SCAN_T: *va_arg(*arguments, ptrdiff_t *) = (ptrdiff_t)value; break;
    default: *va_arg(*arguments, int *) = (int)value; break;
    }
}

static void store_unsigned(va_list *arguments, scan_length_t length,
                           uintmax_t value)
{
    switch (length) {
    case SCAN_HH: *va_arg(*arguments, unsigned char *) = (unsigned char)value; break;
    case SCAN_H: *va_arg(*arguments, unsigned short *) = (unsigned short)value; break;
    case SCAN_L: *va_arg(*arguments, unsigned long *) = (unsigned long)value; break;
    case SCAN_LL: *va_arg(*arguments, unsigned long long *) = (unsigned long long)value; break;
    case SCAN_J: *va_arg(*arguments, uintmax_t *) = (uintmax_t)value; break;
    case SCAN_Z: case SCAN_T: *va_arg(*arguments, size_t *) = (size_t)value; break;
    default: *va_arg(*arguments, unsigned int *) = (unsigned int)value; break;
    }
}

static bool scan_integer(scan_input_t *input, va_list *arguments, char conversion,
                         scan_length_t length, int width, bool suppress)
{
    bool is_signed = conversion == 'd' || conversion == 'i';
    bool pointer = conversion == 'p';
    bool negative = false, any = false, overflow = false;
    int base = conversion == 'o' ? 8 : conversion == 'x' || conversion == 'X' || pointer ? 16 :
               conversion == 'i' ? 0 : 10;
    uintmax_t limit;
    uintmax_t magnitude = 0;
    int used = 0, c;
    do { c = input_get(input); } while (c != EOF && isspace((unsigned char)c));
    if (c == EOF) return false;
    if (c == '+' || c == '-') {
        negative = c == '-';
        used++;
        if (width > 0 && used >= width) return false;
        c = input_get(input);
    }
    if (base == 0) base = c == '0' ? 8 : 10;
    if (c == '0' && (base == 16 || conversion == 'i')) {
        used++;
        any = true;
        magnitude = 0;
        int next = input_get(input);
        if (next == 'x' || next == 'X') {
            if (width > 0 && used + 1 >= width) {
                input_unget(input, next);
                c = EOF;
            } else {
                used++;
                c = input_get(input);
                base = 16;
                any = false;
            }
        } else {
            if (width > 0 && used >= width) {
                input_unget(input, next);
                c = EOF;
            } else {
            c = next;
            }
        }
    }
    limit = is_signed ? signed_limit(length, negative) :
            (pointer ? UINTPTR_MAX : unsigned_limit(length));
    while (c != EOF && (width <= 0 || used < width)) {
        int digit = digit_value(c);
        if (digit < 0 || digit >= base) {
            input_unget(input, c);
            c = EOF;
            break;
        }
        any = true;
        if (magnitude > (limit - (uintmax_t)digit) / (uintmax_t)base)
            overflow = true;
        else if (!overflow)
            magnitude = magnitude * (uintmax_t)base + (uintmax_t)digit;
        used++;
        if (width > 0 && used >= width) break;
        c = input_get(input);
    }
    if (c != EOF && (width <= 0 || used < width)) input_unget(input, c);
    if (!any) return false;
    if (overflow) {
        errno = ERANGE;
        magnitude = limit;
    }
    if (!suppress) {
        if (pointer) *va_arg(*arguments, void **) = (void *)(uintptr_t)magnitude;
        else if (is_signed) store_signed(arguments, length, negative, magnitude);
        else {
            if (negative) magnitude = (uintmax_t)0 - magnitude;
            store_unsigned(arguments, length, magnitude);
        }
    }
    return true;
}

static bool prefix_of(const char *text, size_t length, const char *word)
{
    size_t word_length = strlen(word);
    if (length > word_length) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char a = (unsigned char)text[i], b = (unsigned char)word[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

static bool float_prefix_state(const char *text, size_t length, bool *complete)
{
    size_t i = 0;
    *complete = false;
    if (i < length && (text[i] == '+' || text[i] == '-')) i++;
    if (i == length) return true;
    const char *word = text + i;
    size_t n = length - i;
    if (prefix_of(word, n, "infinity") || prefix_of(word, n, "inf")) {
        *complete = n >= 3U;
        return true;
    }
    if (prefix_of(word, n < 3U ? n : 3U, "nan")) {
        if (n < 3U) return true;
        if (n == 3U) { *complete = true; return true; }
        if (word[3] != '(') return false;
        size_t j = 4U;
        while (j < n && ((word[j] >= 'a' && word[j] <= 'z') ||
                         (word[j] >= 'A' && word[j] <= 'Z') ||
                         (word[j] >= '0' && word[j] <= '9') || word[j] == '_')) j++;
        if (j == n) return true;
        if (j + 1U == n && word[j] == ')') { *complete = true; return true; }
        return false;
    }

    bool hex = n >= 2U && word[0] == '0' && (word[1] == 'x' || word[1] == 'X');
    size_t p = hex ? 2U : 0U;
    bool digit = false, dot = false;
    while (p < n) {
        int d = digit_value((unsigned char)word[p]);
        if (d >= 0 && d < (hex ? 16 : 10)) { digit = true; p++; continue; }
        if (word[p] == '.' && !dot) { dot = true; p++; continue; }
        break;
    }
    if (p == n) {
        *complete = digit;
        return digit || (hex && (n <= 2U || (n == 3U && word[2] == '.'))) ||
               (!hex && (dot || n == 0));
    }
    if (!digit) return false;
    char exp = hex ? 'p' : 'e';
    if (word[p] == exp || word[p] == (char)(exp - ('a' - 'A'))) {
        p++;
        if (p < n && (word[p] == '+' || word[p] == '-')) p++;
        size_t before_digits = p;
        while (p < n && word[p] >= '0' && word[p] <= '9') p++;
        if (p == n) {
            *complete = p > before_digits;
            return true;
        }
        return false;
    }
    return false;
}

static bool append_char(char **buffer, size_t *length, size_t *capacity, char c)
{
    if (*length + 1U >= *capacity) {
        size_t next = *capacity > SIZE_MAX / 2U ? 0U : *capacity * 2U;
        if (!next) { errno = EOVERFLOW; return false; }
        char *replacement = realloc(*buffer, next);
        if (!replacement) { errno = ENOMEM; return false; }
        *buffer = replacement;
        *capacity = next;
    }
    (*buffer)[(*length)++] = c;
    (*buffer)[*length] = '\0';
    return true;
}

static bool scan_float(scan_input_t *input, va_list *arguments, char conversion,
                       scan_length_t length, int width, bool suppress)
{
    int c;
    do { c = input_get(input); } while (c != EOF && isspace((unsigned char)c));
    if (c == EOF) return false;
    size_t capacity = 64U, used = 0;
    char *token = malloc(capacity);
    if (!token) { errno = ENOMEM; input_unget(input, c); return false; }
    token[0] = '\0';
    bool complete = false;
    int consumed = 0;
    do {
        if (used == SCAN_FLOAT_TOKEN_MAX) {
            free(token);
            input_unget(input, c);
            errno = EOVERFLOW;
            return false;
        }
        if (!append_char(&token, &used, &capacity, (char)c)) {
            free(token); input_unget(input, c); return false;
        }
        consumed++;
        if (!float_prefix_state(token, used, &complete)) {
            used--;
            token[used] = '\0';
            input_unget(input, c);
            (void)float_prefix_state(token, used, &complete);
            break;
        }
        if (width > 0 && consumed >= width) break;
        c = input_get(input);
    } while (c != EOF);
    if (!complete || !used) { free(token); return false; }
    char *end;
    int kind = length == SCAN_CAP_L ? 2 : length == SCAN_L ? 1 : 0;
    if (kind == 2) {
        long double value = 0.0L;
        if (__mg_strto_float(token, &end, kind, &value) < 0) {
            free(token); return false;
        }
        if ((size_t)(end - token) != used) { free(token); return false; }
        if (!suppress) *va_arg(*arguments, long double *) = value;
    } else if (kind == 1) {
        double value = 0.0;
        if (__mg_strto_float(token, &end, kind, &value) < 0) {
            free(token); return false;
        }
        if ((size_t)(end - token) != used) { free(token); return false; }
        if (!suppress) *va_arg(*arguments, double *) = value;
    } else {
        float value = 0.0F;
        if (__mg_strto_float(token, &end, kind, &value) < 0) {
            free(token); return false;
        }
        if ((size_t)(end - token) != used) { free(token); return false; }
        if (!suppress) *va_arg(*arguments, float *) = value;
    }
    free(token);
    (void)conversion;
    return true;
}

static bool scan_set(const char **format, bool set[256])
{
    const char *p = *format;
    bool invert = false, any = false;
    memset(set, 0, 256U * sizeof(bool));
    if (*p == '^') { invert = true; p++; }
    if (*p == ']') { set[(unsigned char)*p++] = true; any = true; }
    while (*p && *p != ']') {
        unsigned char first = (unsigned char)*p++;
        if (*p == '-' && p[1] && p[1] != ']') {
            unsigned char last = (unsigned char)p[1];
            if (last < first) return false;
            for (unsigned int c = first; c <= last; c++) set[c] = true;
            p += 2;
        } else set[first] = true;
        any = true;
    }
    if (*p != ']' || !any) return false;
    p++;
    if (invert) for (size_t i = 0; i < 256U; i++) set[i] = !set[i];
    *format = p;
    return true;
}

static int scan_run(scan_input_t *input, const char *format, va_list *arguments)
{
    int assignments = 0;
    while (*format) {
        if (isspace((unsigned char)*format)) {
            while (isspace((unsigned char)*format)) format++;
            int c;
            do { c = input_get(input); } while (c != EOF && isspace((unsigned char)c));
            input_unget(input, c);
            continue;
        }
        if (*format != '%') {
            int c = input_get(input);
            if (c == EOF) return assignments ? assignments : EOF;
            if (c != (unsigned char)*format) {
                input_unget(input, c);
                return assignments;
            }
            format++;
            continue;
        }
        format++;
        if (*format == '%') {
            int c = input_get(input);
            if (c == EOF) return assignments ? assignments : EOF;
            if (c != '%') { input_unget(input, c); return assignments; }
            format++;
            continue;
        }
        bool suppress = false;
        if (*format == '*') { suppress = true; format++; }
        int width = 0;
        if (*format >= '0' && *format <= '9') {
            if (!format_decimal(&format, &width) || width == 0) { errno = EINVAL; return assignments; }
        }
        scan_length_t length = SCAN_DEFAULT;
        if (format[0] == 'h' && format[1] == 'h') { length = SCAN_HH; format += 2; }
        else if (*format == 'h') { length = SCAN_H; format++; }
        else if (format[0] == 'l' && format[1] == 'l') { length = SCAN_LL; format += 2; }
        else if (*format == 'l') { length = SCAN_L; format++; }
        else if (*format == 'j') { length = SCAN_J; format++; }
        else if (*format == 'z') { length = SCAN_Z; format++; }
        else if (*format == 't') { length = SCAN_T; format++; }
        else if (*format == 'L') { length = SCAN_CAP_L; format++; }
        char conversion = *format++;
        if (!conversion) { errno = EINVAL; return assignments; }
        if (conversion != 'c' && conversion != '[' && conversion != 'n') {
            int c;
            do { c = input_get(input); } while (c != EOF && isspace((unsigned char)c));
            input_unget(input, c);
        }
        if (conversion == 'n') {
            if (suppress || length == SCAN_CAP_L) { errno = EINVAL; return assignments; }
            switch (length) {
            case SCAN_HH: *va_arg(*arguments, signed char *) = (signed char)input->consumed; break;
            case SCAN_H: *va_arg(*arguments, short *) = (short)input->consumed; break;
            case SCAN_L: *va_arg(*arguments, long *) = (long)input->consumed; break;
            case SCAN_LL: *va_arg(*arguments, long long *) = (long long)input->consumed; break;
            case SCAN_J: *va_arg(*arguments, intmax_t *) = (intmax_t)input->consumed; break;
            case SCAN_Z: case SCAN_T: *va_arg(*arguments, ptrdiff_t *) = (ptrdiff_t)input->consumed; break;
            default: *va_arg(*arguments, int *) = (int)input->consumed; break;
            }
            continue;
        }
        bool success = false;
        if (conversion == 'd' || conversion == 'i' || conversion == 'u' ||
            conversion == 'o' || conversion == 'x' || conversion == 'X' || conversion == 'p') {
            if (length == SCAN_CAP_L || (conversion == 'p' && length != SCAN_DEFAULT)) {
                errno = EINVAL; return assignments;
            }
            success = scan_integer(input, arguments, conversion, length, width, suppress);
        } else if (strchr("aAeEfFgG", conversion)) {
            if (length == SCAN_HH || length == SCAN_H || length == SCAN_J ||
                length == SCAN_Z || length == SCAN_T) { errno = EINVAL; return assignments; }
            success = scan_float(input, arguments, conversion, length, width, suppress);
        } else if (conversion == 'c' || conversion == 's' || conversion == '[') {
            if (length != SCAN_DEFAULT) { errno = ENOTSUP; return assignments; }
            bool set[256];
            if (conversion == '[' && !scan_set(&format, set)) { errno = EINVAL; return assignments; }
            char *destination = suppress ? NULL : va_arg(*arguments, char *);
            int limit = width > 0 ? width : conversion == 'c' ? 1 : INT_MAX;
            int copied = 0;
            while (copied < limit) {
                int c = input_get(input);
                bool accepted = c != EOF && (conversion == 'c' ||
                    (conversion == 's' ? !isspace((unsigned char)c) : set[(unsigned char)c]));
                if (!accepted) { input_unget(input, c); break; }
                if (destination) destination[copied] = (char)c;
                copied++;
            }
            success = conversion == 'c' ? copied == limit : copied != 0;
            if (success && destination && conversion != 'c') destination[copied] = '\0';
        } else {
            errno = EINVAL;
            return assignments;
        }
        if (!success) {
            if (input_failure(input) && assignments == 0) return EOF;
            return assignments;
        }
        if (!suppress) assignments++;
    }
    return assignments;
}

int vfscanf(FILE *restrict stream, const char *restrict format, va_list arguments)
{
    if (!stream || !format) { errno = EINVAL; return EOF; }
    scan_input_t input = { stream, NULL, 0, 0, false, false };
    va_list copy;
    va_copy(copy, arguments);
    int result = scan_run(&input, format, &copy);
    va_end(copy);
    return result;
}

int fscanf(FILE *restrict stream, const char *restrict format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vfscanf(stream, format, arguments);
    va_end(arguments);
    return result;
}

int scanf(const char *restrict format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vfscanf(stdin, format, arguments);
    va_end(arguments);
    return result;
}

int vscanf(const char *restrict format, va_list arguments)
{
    return vfscanf(stdin, format, arguments);
}

int vsscanf(const char *restrict string, const char *restrict format,
            va_list arguments)
{
    if (!string || !format) { errno = EINVAL; return EOF; }
    scan_input_t input = { NULL, string, 0, 0, false, false };
    va_list copy;
    va_copy(copy, arguments);
    int result = scan_run(&input, format, &copy);
    va_end(copy);
    return result;
}

int sscanf(const char *restrict string, const char *restrict format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vsscanf(string, format, arguments);
    va_end(arguments);
    return result;
}
