#include <io.h>

void outb(u16 port, u8 value)
{
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

u8 inb(u16 port)
{
    u8 value;

    __asm__ volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

void outl(u16 port, u32 value)
{
    __asm__ volatile (
        "outl %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

u32 inl(u16 port)
{
    u32 value;

    __asm__ volatile (
        "inl %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

void io_wait(void)
{
    __asm__ volatile (
        "outb %%al, $0x80"
        :
        : "a"(0)
    );
}
