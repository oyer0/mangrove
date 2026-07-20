#include <bootinfo.h>
#include <font.h>
#include <gdt.h>
#include <idt.h>
#include <pmm.h>

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
    kprint("              MANGROVE OPERATING SYSTEM v%s           \n", "0.1.0"); // Added v%s
    kprint("=====================================================\n\n");

    kprint("[OK] Graphics engine initialized successfully.\n");
    kprint("[INFO] Screen resolution: %d x %d pixels\n", BootInfo->FramebufferWidth, BootInfo->FramebufferHeight);

    kprint("[INFO] Framebuffer VRAM:  %x\n", (u64)BootInfo->FramebufferBase);

    kprint("[INFO] Loading custom Kernel GDT segments...\n");

    gdt_init();
    kprint("[OK] GDT & TSS loaded successfully. Emergency Stack (IST1) ready.\n");

    idt_init();
    kprint("[OK] IDT loaded. Exception handlers online.\n\n");

    pmm_init(BootInfo);
    kprint("[ OK ] Physical Memory Manager initialized.\n");
    
    u64 free_bytes = pmm_get_free_memory();
    u64 used_bytes = pmm_get_used_memory();
    u64 total_bytes = free_bytes + used_bytes;

    // Use signed int types to natively match the %d format specifiers
    int free_mb  = (int)((free_bytes + (512 * 1024)) / 1024 / 1024);
    int total_mb = (int)((total_bytes + (512 * 1024)) / 1024 / 1024);

    kprint("[INFO] Free RAM:  %d MB / %d MB total physical\n", free_mb, total_mb);

    void *test_frame = pmm_alloc_frame();
    kprint("[TEST] Allocated 4KB page frame at physical address: %x\n", (u64)test_frame);

    for (;;)
    { __asm__ volatile ("hlt"); }
}
