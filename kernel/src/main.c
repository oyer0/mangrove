#include <bootinfo.h>
#include <font.h>
#include <gdt.h>
#include <idt.h>
#include <pmm.h>
#include <vmm.h>
#include <version.h>
#include <heap.h>
#include <pic.h>
#include <pit.h>

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

    kprint("=======================================================\n");
    kprint_centered("MANGROVE OPERATING SYSTEM v" MANGROVE_VERSION);
    kprint("\n");
    kprint("=======================================================\n\n");

    kprint("[OK] Graphics engine initialized successfully.\n");
    kprint("[INFO] Screen resolution: %d x %d pixels\n", BootInfo->FramebufferWidth, BootInfo->FramebufferHeight);
    kprint("[INFO] Framebuffer VRAM:  %x\n", (u64)BootInfo->FramebufferBase);

    kprint("[INFO] Loading custom Kernel GDT segments...\n");
    gdt_init();
    kprint("[OK] GDT & TSS loaded successfully. Emergency Stack (IST1) ready.\n");

    idt_init();
    kprint("[OK] IDT loaded. Exception handlers online.\n");

    pic_init();
    kprint("[OK] Legacy PIC initialized.\n");

    pit_init(TIMER_FREQUENCY);
    kprint("[OK] PIT initialized at %d Hz.\n", TIMER_FREQUENCY);

    pmm_init(BootInfo);
    kprint("[OK] Physical Memory Manager initialized.\n");
    
    u64 free_bytes = pmm_get_free_memory();
    u64 used_bytes = pmm_get_used_memory();
    u64 total_bytes = free_bytes + used_bytes;

    int free_mb  = (int)((free_bytes + (512 * 1024)) / 1024 / 1024);
    int total_mb = (int)((total_bytes + (512 * 1024)) / 1024 / 1024);
    kprint("[INFO] Free RAM:  %d MB / %d MB total physical\n", free_mb, total_mb);

    kprint("[VMM] Allocating root PML4 directory...\n");
    page_table_t *k_pml4 = (page_table_t *)pmm_alloc_frame();
    for (int i = 0; i < 512; i++) {
        k_pml4->entries[i] = 0;
    }

    vmm_set_kernel_pml4(k_pml4);

    MANGROVE_MEMORY_DESCRIPTOR *mmap =
        (MANGROVE_MEMORY_DESCRIPTOR *)BootInfo->MemoryMap;

    u64 mmap_entries = BootInfo->MemoryMapSize / BootInfo->DescriptorSize;

    kprint("[VMM] Identity mapping usable RAM...\n");

    for (u64 i = 0; i < mmap_entries; i++) {
        MANGROVE_MEMORY_DESCRIPTOR *desc =
            (MANGROVE_MEMORY_DESCRIPTOR *)((u64)mmap +
                    i * BootInfo->DescriptorSize);

        if (desc->Type != 2 &&
            desc->Type != 3 &&
            desc->Type != 4 &&
            desc->Type != 7)
            continue;

        u64 addr = desc->PhysicalStart;

        for (u64 page = 0; page < desc->NumberOfPages; page++) {
            vmm_map(
                k_pml4,
                (void *)addr,
                (void *)addr,
                PTE_PRESENT | PTE_READWRITE
            );

            addr += PAGE_SIZE;
        }
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

    kprint("[OK] Virtual Memory Manager active. Custom 4-level paging online.\n");

    heap_init();
    kprint("[OK] Kernel heap initialized.\n");

    for (;;)
    { __asm__ volatile ("hlt"); }
}
