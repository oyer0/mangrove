#include <kprint.h>
#include <terminal.h>
#include <stdarg.h>

static void print_unsigned(u64 value, u32 base, int width, char pad_char);
static void print_signed(i64 value, int width, char pad_char);

static void print_unsigned(u64 value, u32 base, int width, char pad_char)
{
    char buffer[64];
    int i = 0;

    if (value == 0) {
        buffer[i++] = '0';
    } else {
        while (value > 0) {
            u32 digit = value % base;
            if (digit < 10)
                buffer[i++] = '0' + digit;
            else
                buffer[i++] = 'a' + (digit - 10);
            value /= base;
        }
    }

    while (width > i) {
        terminal_putc(pad_char);
        width--;
    }

    while (i > 0) {
        terminal_putc(buffer[--i]);
    }
}

static void print_signed(i64 value, int width, char pad_char)
{
    u64 uval;

    if (value < 0) {
        terminal_putc('-');
        uval = -(u64)value;
        width--;
    } else {
        uval = (u64)value;
    }

    print_unsigned(uval, 10, width, pad_char);
}

void kprint(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            terminal_putc(*fmt++);
            continue;
        }

        fmt++;

        if (*fmt == '%') {
            terminal_putc('%');
            fmt++;
            continue;
        }

        char pad_char = ' ';
        int width = 0;
        int length = 0;

        // Parse zero padding flag
        if (*fmt == '0') {
            pad_char = '0';
            fmt++;
        }

        // Parse width
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        // Parse length modifier (e.g. 'll' for 64-bit)
        while (*fmt == 'l') {
            length++;
            fmt++;
        }

        switch (*fmt) {
        case 'c':
            terminal_putc((char)va_arg(args, int));
            break;
        case 's': {
            const char *str = va_arg(args, const char *);
            terminal_write(str ? str : "(null)");
            break;
        }
        case 'd':
        case 'i': {
            i64 val = (length >= 2) ? va_arg(args, i64) : va_arg(args, int);
            print_signed(val, width, pad_char);
            break;
        }
        case 'u': {
            u64 val = (length >= 2) ? va_arg(args, u64) : va_arg(args, unsigned int);
            print_unsigned(val, 10, width, pad_char);
            break;
        }
        case 'x': {
            u64 val = (length >= 2) ? va_arg(args, u64) : va_arg(args, unsigned int);
            print_unsigned(val, 16, width, pad_char);
            break;
        }
        case 'p':
            terminal_write("0x");
            print_unsigned((u64)va_arg(args, void *), 16, width > 0 ? width : 16, '0');
            break;
        default:
            terminal_putc('%');
            if (*fmt) terminal_putc(*fmt);
            else { va_end(args); return; }
            break;
        }

        if (*fmt) fmt++;
    }

    va_end(args);
}
