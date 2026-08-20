#include <kprint.h>
#include <terminal.h>
#include <stdarg.h>
#ifdef NETWORK_BOOT_DIAG
#include <io.h>
#endif

#ifdef NETWORK_BOOT_DIAG
#define DIAG_SERIAL_BASE 0x3f8U

static u8 diag_serial_ready;

static void diag_serial_init(void)
{
    outb(DIAG_SERIAL_BASE + 1, 0x00); /* disable UART interrupts */
    outb(DIAG_SERIAL_BASE + 3, 0x80); /* divisor latch access */
    outb(DIAG_SERIAL_BASE + 0, 0x01); /* 115200 baud */
    outb(DIAG_SERIAL_BASE + 1, 0x00);
    outb(DIAG_SERIAL_BASE + 3, 0x03); /* 8 data bits, no parity, one stop */
    outb(DIAG_SERIAL_BASE + 2, 0xc7); /* enable and clear FIFOs */
    outb(DIAG_SERIAL_BASE + 4, 0x0b); /* IRQs enabled, RTS/DTR asserted */
    diag_serial_ready = 1;
}

static void diag_serial_putc(char value)
{
    if (!diag_serial_ready) {
        diag_serial_init();
    }
    if (value == '\n') {
        while (!(inb(DIAG_SERIAL_BASE + 5) & 0x20)) {
            __asm__ volatile("pause");
        }
        outb(DIAG_SERIAL_BASE, '\r');
    }
    while (!(inb(DIAG_SERIAL_BASE + 5) & 0x20)) {
        __asm__ volatile("pause");
    }
    outb(DIAG_SERIAL_BASE, (u8)value);
}

static void diag_serial_write(const char *value)
{
    if (!value) {
        value = "(null)";
    }
    while (*value) {
        diag_serial_putc(*value++);
    }
}
#endif

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
#ifdef NETWORK_BOOT_DIAG
        diag_serial_putc(pad_char);
#endif
        width--;
    }

    while (i > 0) {
        char digit = buffer[--i];
        terminal_putc(digit);
#ifdef NETWORK_BOOT_DIAG
        diag_serial_putc(digit);
#endif
    }
}

static void print_signed(i64 value, int width, char pad_char)
{
    u64 uval;

    if (value < 0) {
        terminal_putc('-');
#ifdef NETWORK_BOOT_DIAG
        diag_serial_putc('-');
#endif
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
#ifdef NETWORK_BOOT_DIAG
    if (!diag_serial_ready) {
        diag_serial_init();
    }
#endif
    terminal_begin_batch();

    while (*fmt) {
        if (*fmt != '%') {
            terminal_putc(*fmt++);
#ifdef NETWORK_BOOT_DIAG
            diag_serial_putc(fmt[-1]);
#endif
            continue;
        }

        fmt++;

        if (*fmt == '%') {
            terminal_putc('%');
#ifdef NETWORK_BOOT_DIAG
            diag_serial_putc('%');
#endif
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
            {
                char value = (char)va_arg(args, int);
                terminal_putc(value);
#ifdef NETWORK_BOOT_DIAG
                diag_serial_putc(value);
#endif
            }
            break;
        case 's': {
            const char *str = va_arg(args, const char *);
            terminal_write(str ? str : "(null)");
#ifdef NETWORK_BOOT_DIAG
            diag_serial_write(str ? str : "(null)");
#endif
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
#ifdef NETWORK_BOOT_DIAG
            diag_serial_write("0x");
#endif
            print_unsigned((u64)va_arg(args, void *), 16, width > 0 ? width : 16, '0');
            break;
        default:
            terminal_putc('%');
#ifdef NETWORK_BOOT_DIAG
            diag_serial_putc('%');
#endif
            if (*fmt) terminal_putc(*fmt);
#ifdef NETWORK_BOOT_DIAG
            if (*fmt) diag_serial_putc(*fmt);
#endif
            else { va_end(args); return; }
            break;
        }

        if (*fmt) fmt++;
    }

    terminal_end_batch();
    va_end(args);
}
