#include <mangrove.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct format_sink {
    bool (*put)(struct format_sink *sink, char character);
    usize count;
    mg_result_t error;
    void *context;
} format_sink_t;

typedef struct console_context {
    char buffer[128];
    usize length;
} console_context_t;

typedef struct string_context {
    char *buffer;
    usize size;
    usize position;
} string_context_t;

static bool sink_put(format_sink_t *sink, char character)
{
    if (sink->error < 0) return false;
    if (!sink->put(sink, character)) return false;
    sink->count++;
    return true;
}

static bool console_flush(format_sink_t *sink)
{
    console_context_t *context = (console_context_t *)sink->context;
    mg_result_t result;
    if (context->length == 0) return true;
    result = object_write(MG_CONSOLE_HANDLE, context->buffer, context->length);
    if (result < 0 || (usize)result != context->length) {
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
    if (context->size != 0 && context->position + 1 < context->size)
        context->buffer[context->position] = character;
    context->position++;
    return true;
}

static bool emit_text(format_sink_t *sink, const char *text)
{
    while (*text && sink_put(sink, *text++)) {}
    return sink->error >= 0;
}

static bool emit_unsigned(format_sink_t *sink, u64 value, u32 base,
                          bool uppercase, int width, char padding)
{
    char digits[64];
    int length = 0;
    int index;
    if (value == 0) digits[length++] = '0';
    while (value != 0) {
        u32 digit = (u32)(value % base);
        digits[length++] = digit < 10 ? (char)('0' + digit) :
            (char)((uppercase ? 'A' : 'a') + digit - 10);
        value /= base;
    }
    while (width > length) {
        if (!sink_put(sink, padding)) return false;
        width--;
    }
    for (index = length - 1; index >= 0; index--)
        if (!sink_put(sink, digits[index])) return false;
    return true;
}

static bool emit_signed(format_sink_t *sink, i64 value, int width, char padding)
{
    u64 magnitude;
    char digits[64];
    int length = 0;
    int index;
    bool negative = value < 0;
    if (negative) magnitude = (u64)(-(value + 1)) + 1;
    else magnitude = (u64)value;
    if (magnitude == 0) digits[length++] = '0';
    while (magnitude != 0) {
        digits[length++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    }
    if (negative) width--;
    if (padding == ' ') {
        while (width > length) {
            if (!sink_put(sink, ' ')) return false;
            width--;
        }
    }
    if (negative && !sink_put(sink, '-')) return false;
    if (padding == '0') {
        while (width > length) {
            if (!sink_put(sink, '0')) return false;
            width--;
        }
    }
    for (index = length - 1; index >= 0; index--)
        if (!sink_put(sink, digits[index])) return false;
    return true;
}

static bool format_run(format_sink_t *sink, const char *format, va_list args)
{
    while (*format) {
        int width = 0;
        char padding = ' ';
        int length = 0;
        char specifier;
        if (*format != '%') {
            if (!sink_put(sink, *format++)) return false;
            continue;
        }
        format++;
        if (*format == '%') {
            if (!sink_put(sink, '%')) return false;
            format++;
            continue;
        }
        if (*format == '0') {
            padding = '0';
            format++;
        }
        while (*format >= '0' && *format <= '9') {
            if (width <= 1000000) width = width * 10 + (*format - '0');
            format++;
        }
        while (*format == 'l') {
            length++;
            format++;
        }
        specifier = *format++;
        switch (specifier) {
        case 's':
        {
            const char *string = va_arg(args, const char *);
            if (!emit_text(sink, string ? string : "(null)")) return false;
            break;
        }
        case 'c':
            if (!sink_put(sink, (char)va_arg(args, int))) return false;
            break;
        case 'd':
        case 'i':
            if (!emit_signed(sink, length >= 2 ? va_arg(args, i64) :
                             (i64)va_arg(args, int), width, padding)) return false;
            break;
        case 'u':
        case 'x':
        case 'X': {
            u64 value = length >= 2 ? va_arg(args, u64) :
                (u64)va_arg(args, unsigned int);
            u32 base = specifier == 'u' ? 10 : 16;
            if (!emit_unsigned(sink, value, base, specifier == 'X', width,
                               padding)) return false;
            break;
        }
        case 'p':
            if (!emit_text(sink, "0x") ||
                !emit_unsigned(sink, (u64)(uintptr_t)va_arg(args, void *),
                               16, false, width ? width : 16, '0')) return false;
            break;
        default:
            if (!sink_put(sink, '%') || (specifier && !sink_put(sink, specifier)))
                return false;
            break;
        }
    }
    return sink->error >= 0;
}

static int console_format(const char *format, va_list args)
{
    console_context_t context = { { 0 }, 0 };
    format_sink_t sink = { console_put, 0, MG_OK, &context };
    format_run(&sink, format, args);
    console_flush(&sink);
    if (sink.error < 0) return (int)sink.error;
    return (int)sink.count;
}

int putchar(int character)
{
    char value = (char)character;
    return object_write(MG_CONSOLE_HANDLE, &value, 1) == 1 ?
        (unsigned char)value : (int)MG_ERR_IO;
}

int puts(const char *string)
{
    int result = printf("%s\n", string ? string : "(null)");
    return result < 0 ? result : 0;
}

int printf(const char *format, ...)
{
    va_list args;
    int result;
    if (!format) return (int)MG_ERR_BAD_ARGUMENT;
    va_start(args, format);
    result = console_format(format, args);
    va_end(args);
    return result;
}

int snprintf(char *buffer, usize size, const char *format, ...)
{
    string_context_t context = { buffer, size, 0 };
    format_sink_t sink = { string_put, 0, MG_OK, &context };
    va_list args;
    int result;
    if (!format || (size != 0 && !buffer)) return (int)MG_ERR_BAD_ARGUMENT;
    va_start(args, format);
    format_run(&sink, format, args);
    va_end(args);
    if (size != 0) {
        usize end = context.position < size - 1 ? context.position : size - 1;
        buffer[end] = '\0';
    }
    result = (int)context.position;
    return sink.error < 0 ? (int)sink.error : result;
}
