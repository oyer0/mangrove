#include <bootinfo.h>

__attribute__((noreturn))
void handoff(void *Entry, BOOT_INFO *BootInfo)
{
    __asm__ volatile (
        "mov $0x110000, %%rsp\n\t"
        "mov %0, %%rdi\n\t"
        "jmp *%1\n\t"
        :
        : "r"(BootInfo), "r"(Entry)
        : "rsp", "rdi"
    );

    __builtin_unreachable();
}
