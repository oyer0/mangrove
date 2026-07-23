#include <panic.h>
#include <kprint.h>
#include <terminal.h>

void panic(const char *message, struct cpu_registers *regs){
    terminal_cursor_hide();

    terminal_set_background(0x8B0000);
    terminal_set_color(0xFFFFFF);
    terminal_clear();
    terminal_cursor_hide();

    kprint("=========== KERNEL PANIC ===========\n\n");
    kprint("%s\n\n", message);

    if (regs != 0)
    {
        u64 cr2, cr3;

        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        asm volatile("mov %%cr3, %0" : "=r"(cr3));

        kprint("Vector:      %u\n", (u32)regs->vec_no);
        kprint("Error Code:  %x\n", regs->err_code);
        kprint("RIP:         %p\n", regs->rip);
        kprint("RSP:         %p\n", regs->rsp);
        kprint("RFLAGS:      %p\n", regs->rflags);
        kprint("CR2:         %p\n", cr2);
        kprint("CR3:         %p\n", cr3);
    }

    kprint("\nSystem halted.\n");

    for (;;)
    {
        asm volatile("hlt");
    }
}
