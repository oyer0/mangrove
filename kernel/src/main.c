#include <bootinfo.h>
#include <gdt.h>
#include <idt.h>
#include <pmm.h>
#include <vmm.h>
#include <version.h>
#include <heap.h>
#include <pic.h>
#include <pit.h>
#include <timer.h>
#include <keyboard.h>
#include <font.h>
#include <terminal.h>
#include <framebuffer.h>
#include <kprint.h>

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

    framebuffer_init(BootInfo);
    font_init(BootInfo);
    terminal_init(BootInfo);

    kprint("Mangrove OS %s\n\n", MANGROVE_VERSION);

    gdt_init();
    kprint("[OK] GDT initialized\n");

    idt_init();
    kprint("[OK] IDT initialized\n");

    pic_init();
    kprint("[OK] PIC initialized\n");

    pit_init(TIMER_FREQUENCY);
    kprint("[OK] PIT initialized (%u Hz)\n", TIMER_FREQUENCY);

    timer_init();
    kprint("[OK] Timer initialized\n");

    keyboard_init();
    kprint("[OK] Keyboard initialized\n");

    pmm_init(BootInfo);

    u64 free_bytes = pmm_get_free_memory();
    u64 used_bytes = pmm_get_used_memory();
    u64 total_bytes = free_bytes + used_bytes;

    int free_mb  = (int)((free_bytes + (512 * 1024)) / 1024 / 1024);
    int total_mb = (int)((total_bytes + (512 * 1024)) / 1024 / 1024);

    kprint("[OK] Physical memory manager initialized\n");
    kprint("     Total memory : %u MB\n", total_mb);
    kprint("     Free memory  : %u MB\n", free_mb);
    
    page_table_t *k_pml4 = (page_table_t *)pmm_alloc_frame();
    for (int i = 0; i < 512; i++) {
        k_pml4->entries[i] = 0;
    }

    vmm_set_kernel_pml4(k_pml4);
    kprint("[OK] Virtual memory manager initialized\n");

    MANGROVE_MEMORY_DESCRIPTOR *mmap =
        (MANGROVE_MEMORY_DESCRIPTOR *)BootInfo->MemoryMap;

    u64 mmap_entries = BootInfo->MemoryMapSize / BootInfo->DescriptorSize;


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

    u64 fb_base = (u64)BootInfo->FramebufferBase;
    u64 fb_size = BootInfo->FramebufferSize;
    
    u64 fb_pages = ((fb_size + PAGE_SIZE - 1) / PAGE_SIZE) + 1;

    for (u64 i = 0; i < fb_pages; i++) {
        u64 addr = fb_base + (i * PAGE_SIZE);
        vmm_map(k_pml4, (void *)addr, (void *)addr, PTE_PRESENT | PTE_READWRITE | PTE_WRITETHROUGH | PTE_CACHEDISABLE);
    }

    __asm__ volatile(
        "mov %0, %%cr3\n\t"
        "jmp 1f\n\t"
        "1:\n\t"
        :: "r"(k_pml4) : "memory"
    );
    kprint("[OK] Paging enabled\n");

    heap_init();
    kprint("[OK] Kernel heap initialized\n");

    __asm__ volatile("sti");

    kprint("\n");
    kprint("Mangrove OS boot complete.\n");

    for (;;) {
            asm volatile ("hlt");
    }

}
