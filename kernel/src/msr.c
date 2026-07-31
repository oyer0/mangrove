#include <msr.h>

u64 rdmsr(u32 msr)
{
    u32 low;
    u32 high;

    __asm__ volatile(
        "rdmsr"
        : "=a"(low), "=d"(high)
        : "c"(msr)
    );

    return ((u64)high << 32) | low;
}

void wrmsr(u32 msr, u64 value)
{
    u32 low = (u32)value;
    u32 high = (u32)(value >> 32);

    __asm__ volatile(
        "wrmsr"
        :
        : "c"(msr), "a"(low), "d"(high)
    );
}
