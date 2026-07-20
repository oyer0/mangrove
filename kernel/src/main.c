#include <bootinfo.h>
#include <font.h>
#include <gdt.h>
#include <idt.h>
#include <pmm.h>
#include <vmm.h>

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
    kprint("              MANGROVE OPERATING SYSTEM v%s           \n", "0.1.0");
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

    int free_mb  = (int)((free_bytes + (512 * 1024)) / 1024 / 1024);
    int total_mb = (int)((total_bytes + (512 * 1024)) / 1024 / 1024);
    kprint("[INFO] Free RAM:  %d MB / %d MB total physical\n", free_mb, total_mb);

    kprint("\n[VMM] Allocating root PML4 directory...\n");
    page_table_t *k_pml4 = (page_table_t *)pmm_alloc_frame();
    for (int i = 0; i < 512; i++) {
        k_pml4->entries[i] = 0;
    }

    u64 total_span_frames = pmm_get_total_frames();
    kprint("[VMM] Identity mapping %d physical frames...\n", (int)total_span_frames);

    for (u64 i = 0; i < total_span_frames; i++) {
        u64 addr = i * PAGE_SIZE;
        vmm_map(k_pml4, (void *)addr, (void *)addr, PTE_PRESENT | PTE_READWRITE);
    }

    kprint("[VMM] Identity mapping Framebuffer...\n");
    u64 fb_base = (u64)BootInfo->FramebufferBase;
    u64 fb_size = BootInfo->FramebufferSize;
    u64 fb_pages = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (u64 i = 0; i < fb_pages; i++) {
        u64 addr = fb_base + (i * PAGE_SIZE);
        vmm_map(k_pml4, (void *)addr, (void *)addr, PTE_PRESENT | PTE_READWRITE | PTE_WRITETHROUGH | PTE_CACHEDISABLE);
    }

    kprint("[VMM] Memory mapping complete. Activating CR3...\n");

    __asm__ volatile(
        "mov %0, %%cr3\n\t"
        "jmp 1f\n\t"
        "1:\n\t"
        :: "r"(k_pml4) : "memory"
    );

    kprint("[ OK ] Virtual Memory Manager active. Custom 4-level paging online.\n");

    for (;;)
    { __asm__ volatile ("hlt"); }
}
