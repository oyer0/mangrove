#include <bootinfo.h>
#include <font.h>
#include <gdt.h>
#include <idt.h>

extern char __stack_top[];
extern char __stack_bottom[];

void kmain(BOOT_INFO *BootInfo)
{
    u32 *fb = (u32 *)BootInfo->FramebufferBase;
    usize total_pixels = BootInfo->FramebufferSize / sizeof(u32);
    for (usize i = 0; i < total_pixels; i++)
    {
        fb[i] = 0xFFFFFFFF;
    }

    kprint_init(BootInfo);

    kprint("=====================================================\n");
    kprint("              MANGROVE OPERATING SYSTEM              \n", "0.1.0");
    kprint("=====================================================\n\n");

    kprint("[OK] Graphics engine initialized successfully.\n");
    kprint("[INFO] Screen resolution: %d x %d pixels\n", BootInfo->FramebufferWidth, BootInfo->FramebufferHeight);

    kprint("[INFO] Framebuffer VRAM:  %x\n", (u64)BootInfo->FramebufferBase);

    kprint("[INFO] Loading custom Kernel GDT segments...\n");

    gdt_init();
    kprint("[OK] GDT & TSS loaded successfully. Emergency Stack (IST1) ready.\n");

    idt_init();
    kprint("[ OK ] IDT loaded. Exception handlers online.\n\n");

    kprint("[TEST] Triggering ud2 exception...\n");
    __asm__ volatile ("ud2");

    for (;;)
    { __asm__ volatile ("hlt"); }
}
