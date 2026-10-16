/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct format_sink {
    bool (*put)(struct format_sink *sink, char character);
    size_t count;
    mg_result_t error;
    void *context;
} format_sink_t;

typedef struct console_context {
    char buffer[128];
    size_t length;
} console_context_t;

typedef struct string_context {
    char *buffer;
    size_t size;
    size_t position;
} string_context_t;

typedef enum format_length {
    LENGTH_DEFAULT, LENGTH_HH, LENGTH_H, LENGTH_L, LENGTH_LL,
    LENGTH_J, LENGTH_Z, LENGTH_T
} format_length_t;

static bool sink_put(format_sink_t *sink, char character)
{
    if (sink->error < 0) return false;
    if (sink->count == (size_t)INT_MAX) {
        sink->error = MG_ERR_BUFFER_TOO_SMALL;
        return false;
    }
    if (!sink->put(sink, character)) return false;
    sink->count++;
    return true;
}

static bool console_flush(format_sink_t *sink)
{
    console_context_t *context = (console_context_t *)sink->context;
    mg_result_t result;
    if (context->length == 0 || sink->error < 0) return sink->error >= 0;
    result = object_write(MG_STDOUT_HANDLE, context->buffer, context->length);
    if (result < 0 || (size_t)result != context->length) {
        sink->error = result < 0 ? result : MG_ERR_IO;
        return false;
    }
    context->length = 0;
    return true;
}

static bool console_put(format_sink_t *sink, char character)
{
    console_context_t *context = (console_context_t *)sink->context;
    if (context->length == sizeof(context->buffer) && !console_flush(sink))
        return false;
    context->buffer[context->length++] = character;
    return true;
}

static bool string_put(format_sink_t *sink, char character)
{
    string_context_t *context = (string_context_t *)sink->context;
    if (context->size && context->position < context->size - 1)
        context->buffer[context->position] = character;
    context->position++;
    return true;
}

static bool repeat(format_sink_t *sink, char value, size_t count)
{
    while (count--) if (!sink_put(sink, value)) return false;
    return true;
}

static bool emit_field(format_sink_t *sink, const char *text, size_t length,
                       int width, bool left)
{
    size_t padding = width > 0 && (size_t)width > length ?
                     (size_t)width - length : 0;
    if (!left && !repeat(sink, ' ', padding)) return false;
    for (size_t i = 0; i < length; i++)
        if (!sink_put(sink, text[i])) return false;
    return left ? repeat(sink, ' ', padding) : true;
}

static bool emit_number(format_sink_t *sink, uint64_t value, unsigned int base,
                        bool uppercase, bool negative, bool plus, bool space,
                        bool alternate, bool zero_pad, bool left, int width,
                        int precision, bool pointer)
{
    char digits[64];
    char prefix[3];
    size_t digits_count = 0, prefix_count = 0, zeroes, padding, body;
    bool precision_set = precision >= 0;
    char fill = '0';

    if (value == 0 && (pointer || !(precision_set && precision == 0))) {
        digits[digits_count++] = '0';
    } else {
        while (value) {
            unsigned int digit = (unsigned int)(value % base);
            digits[digits_count++] = digit < 10 ? (char)('0' + digit) :
                (char)((uppercase ? 'A' : 'a') + digit - 10);
            value /= base;
        }
    }
    if (negative) prefix[prefix_count++] = '-';
    else if (plus) prefix[prefix_count++] = '+';
    else if (space) prefix[prefix_count++] = ' ';
    if ((pointer || (alternate && base == 16 && digits_count &&
                     !(digits_count == 1 && digits[0] == '0')))) {
        prefix[prefix_count++] = '0';
        prefix[prefix_count++] = uppercase ? 'X' : 'x';
    }
    zeroes = precision_set && precision > (int)digits_count ?
             (size_t)precision - digits_count : 0;
    if (alternate && base == 8 &&
        (digits_count == 0 || (!zeroes && digits[digits_count - 1] != '0')))
        zeroes++;
    body = prefix_count + zeroes + digits_count;
    padding = width > 0 && (size_t)width > body ? (size_t)width - body : 0;
    if (!left && !(zero_pad && !precision_set) && !repeat(sink, ' ', padding))
        return false;
    for (size_t i = 0; i < prefix_count; i++)
        if (!sink_put(sink, prefix[i])) return false;
    if (!left && zero_pad && !precision_set) fill = '0';
    if (!repeat(sink, fill, zeroes +
                ((!left && zero_pad && !precision_set) ? padding : 0)))
        return false;
    for (size_t i = digits_count; i != 0; i--)
        if (!sink_put(sink, digits[i - 1])) return false;
    return left ? repeat(sink, ' ', padding) : true;
}

static bool parse_decimal(const char **format, int *value)
{
    unsigned int result = 0;
    const char *cursor = *format;
    while (*cursor >= '0' && *cursor <= '9') {
        unsigned int digit = (unsigned int)(*cursor - '0');
        if (result > ((unsigned int)INT_MAX - digit) / 10U) return false;
        result = result * 10U + digit;
        cursor++;
    }
    *format = cursor;
    *value = (int)result;
    return true;
}

static int64_t signed_argument(va_list *arguments, format_length_t length)
{
    switch (length) {
    case LENGTH_HH: return (signed char)va_arg(*arguments, int);
    case LENGTH_H: return (short)va_arg(*arguments, int);
    case LENGTH_L: return va_arg(*arguments, long);
    case LENGTH_LL: return va_arg(*arguments, long long);
    case LENGTH_J: return va_arg(*arguments, intmax_t);
    case LENGTH_Z: return va_arg(*arguments, ptrdiff_t);
    case LENGTH_T: return va_arg(*arguments, ptrdiff_t);
    default: return va_arg(*arguments, int);
    }
}

static uint64_t unsigned_argument(va_list *arguments, format_length_t length)
{
    switch (length) {
    case LENGTH_HH: return (unsigned char)va_arg(*arguments, int);
    case LENGTH_H: return (unsigned short)va_arg(*arguments, int);
    case LENGTH_L: return va_arg(*arguments, unsigned long);
    case LENGTH_LL: return va_arg(*arguments, unsigned long long);
    case LENGTH_J: return va_arg(*arguments, uintmax_t);
    case LENGTH_Z: return va_arg(*arguments, size_t);
    case LENGTH_T: return va_arg(*arguments, size_t);
    default: return va_arg(*arguments, unsigned int);
    }
}

static void store_count(va_list *arguments, format_length_t length, size_t count)
{
    switch (length) {
    case LENGTH_HH: *va_arg(*arguments, signed char *) = (signed char)count; break;
    case LENGTH_H: *va_arg(*arguments, short *) = (short)count; break;
    case LENGTH_L: *va_arg(*arguments, long *) = (long)count; break;
    case LENGTH_LL: *va_arg(*arguments, long long *) = (long long)count; break;
    case LENGTH_J: *va_arg(*arguments, intmax_t *) = (intmax_t)count; break;
    case LENGTH_Z: *va_arg(*arguments, ptrdiff_t *) = (ptrdiff_t)count; break;
    case LENGTH_T: *va_arg(*arguments, ptrdiff_t *) = (ptrdiff_t)count; break;
    default: *va_arg(*arguments, int *) = (int)count; break;
    }
}

static bool format_run(format_sink_t *sink, const char *format, va_list *arguments)
{
    while (*format) {
        bool left = false, plus = false, space = false;
        bool alternate = false, zero = false;
        int width = 0, precision = -1;
        format_length_t length = LENGTH_DEFAULT;
        char specifier;
        if (*format != '%') {
            if (!sink_put(sink, *format++)) return false;
            continue;
        }
        format++;
        if (!*format) { sink->error = MG_ERR_BAD_ARGUMENT; return false; }
        for (;;) {
            if (*format == '-') left = true;
            else if (*format == '+') plus = true;
            else if (*format == ' ') space = true;
            else if (*format == '#') alternate = true;
            else if (*format == '0') zero = true;
            else break;
            format++;
        }
        if (*format == '*') {
            int supplied = va_arg(*arguments, int);
            format++;
            if (supplied < 0) {
                left = true;
                if (supplied == INT_MIN) { sink->error = MG_ERR_BAD_ARGUMENT; return false; }
                width = -supplied;
            } else width = supplied;
        } else if (*format >= '0' && *format <= '9') {
            if (!parse_decimal(&format, &width)) {
                sink->error = MG_ERR_BAD_ARGUMENT; return false;
            }
        }
        if (*format == '.') {
            precision = 0;
            format++;
            if (*format == '*') {
                int supplied = va_arg(*arguments, int);
                format++;
                precision = supplied < 0 ? -1 : supplied;
            } else if (*format >= '0' && *format <= '9') {
                if (!parse_decimal(&format, &precision)) {
                    sink->error = MG_ERR_BAD_ARGUMENT; return false;
                }
            }
        }
        if (format[0] == 'h' && format[1] == 'h') {
            length = LENGTH_HH; format += 2;
        } else if (*format == 'h') { length = LENGTH_H; format++; }
        else if (format[0] == 'l' && format[1] == 'l') {
            length = LENGTH_LL; format += 2;
        } else if (*format == 'l') { length = LENGTH_L; format++; }
        else if (*format == 'j') { length = LENGTH_J; format++; }
        else if (*format == 'z') { length = LENGTH_Z; format++; }
        else if (*format == 't') { length = LENGTH_T; format++; }
        specifier = *format;
        if (!specifier) { sink->error = MG_ERR_BAD_ARGUMENT; return false; }
        format++;
        switch (specifier) {
        case 'd': case 'i': {
            int64_t value = signed_argument(arguments, length);
            uint64_t magnitude = value < 0 ? (uint64_t)(-(value + 1)) + 1U :
                                             (uint64_t)value;
            if (!emit_number(sink, magnitude, 10, false, value < 0, plus,
                             space, false, zero, left, width, precision, false))
                return false;
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            unsigned int base = specifier == 'o' ? 8U :
                ((specifier == 'x' || specifier == 'X') ? 16U : 10U);
            if (!emit_number(sink, unsigned_argument(arguments, length), base,
                             specifier == 'X', false, false, false, alternate,
                             zero, left, width, precision, false)) return false;
            break;
        }
        case 'p': {
            void *pointer;
            if (length != LENGTH_DEFAULT) { sink->error = MG_ERR_BAD_ARGUMENT; return false; }
            pointer = va_arg(*arguments, void *);
            if (!emit_number(sink, (uint64_t)(uintptr_t)pointer, 16, false,
                             false, false, false, true, zero, left, width,
                             precision, true)) return false;
            break;
        }
        case 's': {
            const char *value;
            size_t length_bytes = 0;
            if (length != LENGTH_DEFAULT) { sink->error = MG_ERR_BAD_ARGUMENT; return false; }
            value = va_arg(*arguments, const char *);
            if (!value) value = "(null)"; /* Mangrove diagnostic extension. */
            while (value[length_bytes] &&
                   (precision < 0 || length_bytes < (size_t)precision))
                length_bytes++;
            if (!emit_field(sink, value, length_bytes, width, left)) return false;
            break;
        }
        case 'c': {
            char value;
            if (length != LENGTH_DEFAULT) { sink->error = MG_ERR_BAD_ARGUMENT; return false; }
            value = (char)va_arg(*arguments, int);
            if (!emit_field(sink, &value, 1, width, left)) return false;
            break;
        }
        case 'n':
            store_count(arguments, length, sink->count);
            break;
        case '%':
            if (length != LENGTH_DEFAULT) { sink->error = MG_ERR_BAD_ARGUMENT; return false; }
            if (!emit_field(sink, "%", 1, width, left)) return false;
            break;
        default:
            /* Floating conversions and unknown spellings are not implemented. */
            sink->error = (specifier == 'a' || specifier == 'A' ||
                           specifier == 'e' || specifier == 'E' ||
                           specifier == 'f' || specifier == 'F' ||
                           specifier == 'g' || specifier == 'G') ?
                          MG_ERR_UNSUPPORTED : MG_ERR_BAD_ARGUMENT;
            return false;
        }
    }
    return true;
}

static int format_error(mg_result_t result)
{
    errno = mg_error_to_errno(result);
    return -1;
}

static int console_format(const char *format, va_list arguments)
{
    console_context_t context = { { 0 }, 0 };
    format_sink_t sink = { console_put, 0, MG_OK, &context };
    va_list copy;
    va_copy(copy, arguments);
    (void)format_run(&sink, format, &copy);
    va_end(copy);
    if (sink.error >= 0) (void)console_flush(&sink);
    if (sink.error < 0) return format_error(sink.error);
    return (int)sink.count;
}

int putchar(int character)
{
    char value = (char)(unsigned char)character;
    mg_result_t result = object_write(MG_STDOUT_HANDLE, &value, 1);
    if (result == 1) return (unsigned char)value;
    return format_error(result < 0 ? result : MG_ERR_IO);
}

int puts(const char *string)
{
    int result = printf("%s\n", string ? string : "(null)");
    return result < 0 ? EOF : result;
}

int printf(const char *format, ...)
{
    va_list arguments;
    int result;
    if (!format) return format_error(MG_ERR_BAD_ARGUMENT);
    va_start(arguments, format);
    result = console_format(format, arguments);
    va_end(arguments);
    return result;
}

int snprintf(char *buffer, size_t size, const char *format, ...)
{
    string_context_t context = { buffer, size, 0 };
    format_sink_t sink = { string_put, 0, MG_OK, &context };
    va_list arguments;
    if (!format || (size && !buffer)) return format_error(MG_ERR_BAD_ARGUMENT);
    va_start(arguments, format);
    (void)format_run(&sink, format, &arguments);
    va_end(arguments);
    if (size) {
        size_t end = context.position < size - 1 ? context.position : size - 1;
        buffer[end] = '\0';
    }
    if (sink.error < 0) return format_error(sink.error);
    return (int)sink.count;
}
