#include <bootinfo.h>
#include <gdt.h>
#include <idt.h>
#include <pmm.h>
#include <memory_types.h>
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
#include <console.h>
#include <kmon/core.h>
#include <pci.h>
#include <acpi.h>
#include <lapic.h>
#include <ioapic.h>
#include <ahci.h>

extern char __stack_top[];
extern char __stack_bottom[];

void kmain(BOOT_INFO *BootInfo) {
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
    kprint("[OK] PIT initialized at %u Hz\n", TIMER_FREQUENCY);

    timer_init();
    kprint("[OK] Timer initialized\n");

    keyboard_init();
    kprint("[OK] Keyboard initialized\n");

    pmm_init(BootInfo);

    acpi_init(BootInfo);

    if (acpi_present()) {
        kprint("[OK] ACPI RSDP found\n");
    } else {
        kprint("[FAIL] ACPI RSDP not found\n");
    }

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
            desc->Type != 7 &&
            desc->Type != 9 &&
            desc->Type != 10)
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

    acpi_madt_t *madt = acpi_madt();
    
    if (!madt)
    {
        kprint("[FAIL] ACPI MADT not found\n");
        return;
    }
    
    u64 lapic_base = madt->local_apic_address;
    
    vmm_map(
        k_pml4,
        (void *)lapic_base,
        (void *)lapic_base,
        PTE_PRESENT |
        PTE_READWRITE |
        PTE_WRITETHROUGH |
        PTE_CACHEDISABLE
    );

    for (u32 i = 0; i < acpi_io_apic_count(); i++)
    {
        const acpi_io_apic_t *apic = acpi_io_apic(i);
    
        if (!apic)
        {
            continue;
        }
    
        vmm_map(
            k_pml4,
            (void *)(u64)apic->address,
            (void *)(u64)apic->address,
            PTE_PRESENT |
            PTE_READWRITE |
            PTE_WRITETHROUGH |
            PTE_CACHEDISABLE
        );
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

    lapic_init();
    
    if (lapic_present())
    {
        lapic_enable();

        kprint("[OK] Local APIC enabled\n");
    }
    else
    {
        kprint("[FAIL] Local APIC not found\n");
    }
    
    ioapic_init();
    
    if (ioapic_present())
    {
        u8 apic_id = lapic_read(LAPIC_ID) >> 24;

        ioapic_route_irq(acpi_irq_to_gsi(0), 0x20, apic_id);
        ioapic_route_irq(acpi_irq_to_gsi(1), 0x21, apic_id);

        kprint("[OK] I/O APIC initialized\n");
    }
    else
    {
        kprint("[FAIL] I/O APIC not found\n");
    }

    pci_init();
    kprint("[OK] PCI initialized\n");

    ahci_init();
    kprint("[OK] AHCI initialized\n");

    __asm__ volatile("sti");

    kprint("[OK] Booting complete.\n\n");

    console_init();
    kmon_init();

    for (;;)
    {
        asm volatile("hlt");
    }

}
