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
    if (!diag_serial_ready)
        diag_serial_init();
    if (value == '\n') {
        while (!(inb(DIAG_SERIAL_BASE + 5) & 0x20))
            __asm__ volatile("pause");
        outb(DIAG_SERIAL_BASE, '\r');
    }
    while (!(inb(DIAG_SERIAL_BASE + 5) & 0x20))
        __asm__ volatile("pause");
    outb(DIAG_SERIAL_BASE, (u8)value);
}
#endif

typedef struct {
    bool terminal;
    bool serial;
    bool boot_log;
} kprint_sink_t;

static char boot_log[KERNEL_BOOT_LOG_CAPACITY];
static usize boot_log_start;
static usize boot_log_length;
static bool boot_log_active = true;

static u64 boot_log_irq_save(void)
{
    u64 flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void boot_log_irq_restore(u64 flags)
{
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
}

static void boot_log_append(char value)
{
    u64 flags = boot_log_irq_save();

    if (boot_log_active) {
        usize index;
        if (boot_log_length < KERNEL_BOOT_LOG_CAPACITY) {
            index = (boot_log_start + boot_log_length) %
                    KERNEL_BOOT_LOG_CAPACITY;
            boot_log[index] = value;
            boot_log_length++;
        } else {
            boot_log[boot_log_start] = value;
            boot_log_start = (boot_log_start + 1U) %
                              KERNEL_BOOT_LOG_CAPACITY;
        }
    }

    boot_log_irq_restore(flags);
}

static void sink_putc(kprint_sink_t *sink, char value)
{
    if (sink->boot_log)
        boot_log_append(value);
    if (sink->terminal)
        terminal_putc(value);
#ifdef NETWORK_BOOT_DIAG
    if (sink->serial)
        diag_serial_putc(value);
#else
    (void)sink;
#endif
}

static void sink_write(kprint_sink_t *sink, const char *value)
{
    if (!value)
        value = "(null)";
    while (*value)
        sink_putc(sink, *value++);
}

static void print_unsigned(kprint_sink_t *sink, u64 value, u32 base,
                           int width, char pad_char)
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
        sink_putc(sink, pad_char);
        width--;
    }

    while (i > 0)
        sink_putc(sink, buffer[--i]);
}

static void print_signed(kprint_sink_t *sink, i64 value, int width,
                         char pad_char)
{
    u64 uval;

    if (value < 0) {
        sink_putc(sink, '-');
        uval = -(u64)value;
        width--;
    } else {
        uval = (u64)value;
    }

    print_unsigned(sink, uval, 10, width, pad_char);
}

static void kprint_vformat(const char *fmt, va_list args,
                           bool terminal_output, bool serial_output)
{
    kprint_sink_t sink = {
        .terminal = terminal_output,
        .serial = serial_output,
        .boot_log = true,
    };

    while (*fmt) {
        if (*fmt != '%') {
            sink_putc(&sink, *fmt++);
            continue;
        }

        fmt++;
        if (*fmt == '%') {
            sink_putc(&sink, '%');
            fmt++;
            continue;
        }

        char pad_char = ' ';
        int width = 0;
        int length = 0;

        if (*fmt == '0') {
            pad_char = '0';
            fmt++;
        }

        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        while (*fmt == 'l') {
            length++;
            fmt++;
        }

        switch (*fmt) {
        case 'c':
            sink_putc(&sink, (char)va_arg(args, int));
            break;
        case 's':
            sink_write(&sink, va_arg(args, const char *));
            break;
        case 'd':
        case 'i': {
            i64 value = (length >= 2) ? va_arg(args, i64) :
                                        va_arg(args, int);
            print_signed(&sink, value, width, pad_char);
            break;
        }
        case 'u': {
            u64 value = (length >= 2) ? va_arg(args, u64) :
                                        va_arg(args, unsigned int);
            print_unsigned(&sink, value, 10, width, pad_char);
            break;
        }
        case 'x': {
            u64 value = (length >= 2) ? va_arg(args, u64) :
                                        va_arg(args, unsigned int);
            print_unsigned(&sink, value, 16, width, pad_char);
            break;
        }
        case 'p':
            sink_write(&sink, "0x");
            print_unsigned(&sink, (u64)va_arg(args, void *), 16,
                           width > 0 ? width : 16, '0');
            break;
        default:
            sink_putc(&sink, '%');
            if (*fmt)
                sink_putc(&sink, *fmt);
            break;
        }

        if (*fmt)
            fmt++;
    }
}

void kprint(const char *fmt, ...)
{
    va_list args;

#ifdef NETWORK_BOOT_DIAG
    if (!diag_serial_ready)
        diag_serial_init();
#endif

    va_start(args, fmt);
    terminal_begin_batch();
    kprint_vformat(fmt, args, true,
#ifdef NETWORK_BOOT_DIAG
                   true
#else
                   false
#endif
    );
    va_end(args);
    terminal_end_batch();
}

void kprint_debug(const char *fmt, ...)
{
    va_list args;
    bool terminal_output = KERNEL_BOOT_DEBUG != 0;

#ifdef NETWORK_BOOT_DIAG
    bool serial_output = terminal_output;
    if (serial_output && !diag_serial_ready)
        diag_serial_init();
#else
    bool serial_output = false;
#endif

    va_start(args, fmt);
    if (terminal_output)
        terminal_begin_batch();
    kprint_vformat(fmt, args, terminal_output, serial_output);
    if (terminal_output)
        terminal_end_batch();
    va_end(args);
}

void kprint_debug_screen(const char *fmt, ...)
{
    va_list args;

#ifdef NETWORK_BOOT_DIAG
    if (!diag_serial_ready)
        diag_serial_init();
#endif

    va_start(args, fmt);
    terminal_begin_batch();
    kprint_vformat(fmt, args, true,
#ifdef NETWORK_BOOT_DIAG
                   true
#else
                   false
#endif
    );
    va_end(args);
    terminal_end_batch();
}

usize kprint_boot_log_size(void)
{
    u64 flags = boot_log_irq_save();
    usize length = boot_log_length;
    boot_log_irq_restore(flags);
    return length;
}

usize kprint_boot_log_read(usize offset, char *buffer, usize capacity)
{
    u64 flags;
    usize available;
    usize count;

    if (!buffer || capacity == 0)
        return 0;

    flags = boot_log_irq_save();
    if (offset >= boot_log_length) {
        boot_log_irq_restore(flags);
        return 0;
    }

    available = boot_log_length - offset;
    count = available < capacity ? available : capacity;
    for (usize i = 0; i < count; i++) {
        usize index = (boot_log_start + offset + i) %
                      KERNEL_BOOT_LOG_CAPACITY;
        buffer[i] = boot_log[index];
    }
    boot_log_irq_restore(flags);
    return count;
}

void kprint_boot_log_stop(void)
{
    u64 flags = boot_log_irq_save();
    boot_log_active = false;
    boot_log_irq_restore(flags);
}
