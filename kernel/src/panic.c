#include <panic.h>
#include <kprint.h>
#include <terminal.h>
#include <version.h>
#include <stddef.h>

/* Bounded delay for framebuffer capture; independent of timer interrupts. */
#define PANIC_DEBUG_DELAY_LOOPS 1000000000ULL

static void panic_internal(
    const char *message,
    struct cpu_registers *regs)
{
    terminal_cursor_hide();

    terminal_set_background(0x8B0000);
    terminal_set_color(0xFFFFFF);
    /* Keep the preceding diagnostic trace visible while debugging allocator
     * faults; restore clearing once the PMM issue is resolved. */
    terminal_clear();
    terminal_cursor_disable();

    kprint("%s %s\n\n", RHIZOME_NAME, RHIZOME_VERSION);
    kprint("=============== KERNEL PANIC ===============\n\n");

    kprint("Reason: ");
    kprint("%s\n", message);

    if (regs != NULL)
    {
        u64 cr2, cr3;

        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        asm volatile("mov %%cr3, %0" : "=r"(cr3));

        kprint("\nDiagnostics\n");
        kprint("-----------\n");

        kprint("Vector:      %u\n", (u32)regs->vec_no);
        kprint("Error Code:  %x\n", regs->err_code);
        kprint("RIP:         %p\n", regs->rip);
        if (cpu_registers_has_privilege_stack(regs)) {
            kprint("RSP:         %p\n", cpu_registers_interrupted_rsp(regs));
            kprint("SS:          %p\n", cpu_registers_interrupted_ss(regs));
        } else {
            kprint("RSP:         %p (derived, same-ring)\n",
                   cpu_registers_interrupted_rsp(regs));
        }
        kprint("RFLAGS:      %p\n", regs->rflags);
        kprint("CR2:         %p\n", cr2);
        kprint("CR3:         %p\n", cr3);
        kprint("Stack dump (%p):\n", cpu_registers_interrupted_rsp(regs));
        u64 *sp = (u64 *)cpu_registers_interrupted_rsp(regs);
        for (int i = 0; i < 24; i++) {
            kprint("  +%02x [%p] = %p\n", i * 8, &sp[i], sp[i]);
        }
    }

    kprint("\nSystem halted.\n");
    kprint("Halting in 3 seconds...\n");

    volatile u64 delay = PANIC_DEBUG_DELAY_LOOPS;
    while (delay != 0) {
        asm volatile("pause");
        delay--;
    }

    for (;;)
    {
        asm volatile("cli; hlt");
    }
}

void panic(const char *message)
{
    panic_internal(message, NULL);
}

void panic_exception(
    const char *message,
    struct cpu_registers *regs)
{
    panic_internal(message, regs);
}
