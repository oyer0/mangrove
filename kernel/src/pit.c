#include <pit.h>
#include <io.h>

#define PIT_CHANNEL0    0x40
#define PIT_COMMAND     0x43

#define PIT_BASE_FREQUENCY  1193182
#define PIT_MODE_RATE_GENERATOR 0x36

static u8 pit_command = PIT_MODE_RATE_GENERATOR;
static u16 pit_divisor;

static void pit_program(u8 command, u16 divisor)
{
    outb(PIT_COMMAND, command);
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, divisor >> 8);

    pit_command = command;
    pit_divisor = divisor;
}

void pit_init(u32 frequency) {
    u32 divisor;

    if (frequency == 0) {
        return;
    }

    divisor = PIT_BASE_FREQUENCY / frequency;
    /* The PIT has a 16-bit reload value; zero encodes 65536. */
    if (divisor > 0x10000U) {
        divisor = 0x10000U;
    }
    pit_program(PIT_MODE_RATE_GENERATOR,
                divisor == 0x10000U ? 0 : (u16)divisor);
}
