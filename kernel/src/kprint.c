#include <kprint.h>
#include <terminal.h>
#include <stdarg.h>

static void print_unsigned(u64 value, u32 base);
static void print_signed(i64 value);
static void print_hex(u64 value);

static void print_unsigned(u64 value, u32 base)
{
    char buffer[32];
    int i = 0;

    if (value == 0) {
        terminal_putc('0');
        return;
    }

    while (value > 0) {
        u32 digit = value % base;

        if (digit < 10)
            buffer[i++] = '0' + digit;
        else
            buffer[i++] = 'a' + (digit - 10);

        value /= base;
    }

    while (i > 0) {
        terminal_putc(buffer[--i]);
    }
}

static void print_signed(i64 value)
{
    if (value < 0) {
        terminal_putc('-');
        value = -value;
    }

    print_unsigned((u64)value, 10);
}

static void print_hex(u64 value)
{
    print_unsigned(value, 16);
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

        switch (*fmt) {

        case '%':
            terminal_putc('%');
            break;

        case 'c':
            terminal_putc((char)va_arg(args, int));
            break;

        case 's': {
            const char *str = va_arg(args, const char *);
            terminal_write(str ? str : "(null)");
            break;
        }

        case 'd':
            print_signed(va_arg(args, int));
            break;

        case 'u':
            print_unsigned(va_arg(args, unsigned int), 10);
            break;

        case 'x':
            print_hex(va_arg(args, unsigned int));
            break;

        case 'p':
            terminal_write("0x");
            print_hex((u64)va_arg(args, void *));
            break;

        default:
            terminal_putc('%');
            terminal_putc(*fmt);
            break;
        }

        fmt++;
    }

    va_end(args);
}
