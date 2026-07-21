#include <pit.h>
#include <io.h>

#define PIT_CHANNEL0    0x40
#define PIT_COMMAND     0x43

#define PIT_BASE_FREQUENCY  1193182

void pit_init(u32 frequency) {
    u16 divisor = PIT_BASE_FREQUENCY / frequency;

    outb(PIT_COMMAND, 0x36);

    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, divisor >> 8);
}
