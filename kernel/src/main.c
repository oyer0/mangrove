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
#include <xhci.h>
#include <irq.h> // Ensure we can register the xHCI interrupt
#include <vfs.h>
#include <initramfs.h>
#include <storage/fat32.h>
#include <string.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

#include <kmon/pci.h>
#include <kmon/ahci.h>

extern char __stack_top[];
extern char __stack_bottom[];

/* Global pointer so the IRQ stub can pass it to the driver */
xhci_controller_t *g_xhc = 0;
extern void usb_keyboard_handler(u8 modifier_mask, const u8 *key_codes, u8 count);

static void main_xhci_irq_handler(struct cpu_registers *regs)
{
    (void)regs;
    if (g_xhc) {
        xhci_interrupt_handler(g_xhc);
    }
}

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
    vfs_init();

    kprint("Mangrove OS %s\n\n", MANGROVE_VERSION);

    gdt_init();
    idt_init();
    pic_init();
    pit_init(TIMER_FREQUENCY);
    timer_init();
    keyboard_init();
    kprint("[OK] Core architecture & interrupts initialized\n");

    pmm_init(BootInfo);
    acpi_init(BootInfo);

    if (!acpi_present()) {
        kprint("[FAIL] ACPI RSDP not found\n");
    }

    page_table_t *k_pml4 = (page_table_t *)pmm_alloc_frame();
    for (int i = 0; i < 512; i++) {
        k_pml4->entries[i] = 0;
    }

    vmm_set_kernel_pml4(k_pml4);

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
    kprint("[OK] Virtual memory & paging enabled\n");

    heap_init();
    kprint("[OK] Kernel heap initialized\n");

    lapic_init();
    
    if (lapic_present())
    {
        lapic_enable();
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

        kprint("[OK] APIC interrupt routing enabled\n");
    }
    else
    {
        kprint("[FAIL] I/O APIC not found\n");
    }

    pci_init();
    ahci_init();
    kprint("[OK] PCI bus & AHCI storage initialized\n");

    /* Register filesystem drivers */
    initramfs_init();
    fat32_init();

    bool fat32_mounted = false;
    if (block_device_count() > 1) {
        block_device_t *bdev = block_get_device(1);
        if (bdev) {
            vfs_fs_type_t *fat32_driver = vfs_find_fs("fat32");
            if (fat32_driver && fat32_driver->probe && fat32_driver->probe(bdev)) {
                if (vfs_mount_root("fat32", bdev) == VFS_OK) {
                    kprint("[OK] Mounted FAT32 test disk as VFS root filesystem ('/')\n");
                    fat32_mounted = true;
                }
            }
        }
    }

    if (!fat32_mounted) {
        if (vfs_mount_root("initramfs", NULL) == VFS_OK) {
            kprint("[OK] Mounted Initramfs RAM filesystem as VFS root filesystem ('/')\n");
        }
    }

    vfs_node_t *root_node = vfs_get_root_node();
    if (root_node && root_node->super) {
        vfs_super_t *sb = root_node->super;
        vfs_dirent_t ent;
        u32 idx = 0;
        kprint("Root Directory Listing:\n");
        while (vfs_readdir(root_node, idx, &ent)) {
            kprint("  - %s (%s, inode: %u)\n",
                   ent.name,
                   (ent.type == VFS_TYPE_DIRECTORY) ? "DIR" : "FILE",
                   (u32)ent.inode);
            idx++;
        }

        if (fat32_mounted) {
            u32 alloc1 = fat32_alloc_cluster(sb);
            u32 alloc2 = fat32_extend_chain(sb, alloc1);
            u32 link1 = fat32_get_cluster_link(sb, alloc1);
            kprint("[OK] FAT Allocation Primitives Test: alloc1=%u, alloc2=%u, link1=%u\n",
                   alloc1, alloc2, link1);

            fat32_free_chain(sb, alloc1);
            u32 freed_link = fat32_get_cluster_link(sb, alloc1);
            kprint("[OK] FAT Free Chain Test: freed_link=%u\n", freed_link);

            /* Test End-to-End Object Lifecycle: Create -> Write -> Read -> Delete */
            vfs_node_t *created_file = NULL;
            if (vfs_create(root_node, "NEWFILE.TXT", &created_file) == VFS_OK && created_file) {
                const char *msg = "Hello from Mangrove OS FAT32 file creation!";
                u64 w_bytes = vfs_write(created_file, 0, strlen(msg), msg);

                char read_back[128];
                u64 r_bytes = vfs_read(created_file, 0, sizeof(read_back) - 1, read_back);
                read_back[r_bytes] = '\0';

                kprint("[OK] VFS File Lifecycle Test: '%s' (%u written, %u read)\n",
                       read_back, (u32)w_bytes, (u32)r_bytes);
            }

            vfs_node_t *new_dir = NULL;
            if (vfs_mkdir(root_node, "DOCS", &new_dir) == VFS_OK && new_dir) {
                kprint("[OK] VFS Directory Creation Test: Created '/DOCS' (inode: %u)\n", (u32)new_dir->inode);

                vfs_node_t *sub_file = NULL;
                if (vfs_create(new_dir, "NOTES.TXT", &sub_file) == VFS_OK && sub_file) {
                    const char *sub_msg = "Nested file inside FAT32 /DOCS directory!";
                    vfs_write(sub_file, 0, strlen(sub_msg), sub_msg);

                    char sub_read[128];
                    u64 sr_bytes = vfs_read(sub_file, 0, sizeof(sub_read) - 1, sub_read);
                    sub_read[sr_bytes] = '\0';
                    kprint("[OK] Nested File Lifecycle Test: '%s' (%u bytes read)\n", sub_read, (u32)sr_bytes);

                    if (vfs_unlink(new_dir, "NOTES.TXT") == VFS_OK) {
                        kprint("[OK] VFS Unlink Test: Deleted '/DOCS/NOTES.TXT'\n");
                    }
                }

                if (vfs_rmdir(root_node, "DOCS") == VFS_OK) {
                    kprint("[OK] VFS Rmdir Test: Removed empty directory '/DOCS'\n");
                }
            }

            /* Clean up temporary test file */
            vfs_unlink(root_node, "NEWFILE.TXT");
        }
    }

    __asm__ volatile("sti");

    /* ==============================================================================
     * xHCI Subsystem Initialization
     * ============================================================================== */

    bool xhci_found = false;
    uintptr_t xhci_mmio_base = 0;
    usize xhci_mmio_size = 0x4000;
    u8 xhci_irq = 0;

    u32 dev_count = pci_get_device_count();
    for (u32 i = 0; i < dev_count; i++) {
        const pci_device_t *pdev = pci_get_device(i);
        if (!pdev) continue;

        if (pdev->class_code == 0x0C && pdev->subclass == 0x03 && pdev->prog_if == 0x30) {
            xhci_found = true;

            pci_bar_t bar0 = pci_get_bar(pdev, 0);
            xhci_mmio_base = (uintptr_t)bar0.address;

            xhci_irq = 11;
            break;
        }
    }

    if (xhci_found) {
        u64 mmio_pages = ((xhci_mmio_size + PAGE_SIZE - 1) / PAGE_SIZE);
        
        for (u64 i = 0; i < mmio_pages; i++) {
            u64 addr = xhci_mmio_base + (i * PAGE_SIZE);
            vmm_map(
                k_pml4,
                (void *)addr,
                (void *)addr,
                PTE_PRESENT | PTE_READWRITE | PTE_WRITETHROUGH | PTE_CACHEDISABLE
            );
        }

        if (ioapic_present()) {
            u8 apic_id = lapic_read(LAPIC_ID) >> 24;
            ioapic_route_irq(acpi_irq_to_gsi(xhci_irq), 0x22, apic_id);
            
            /* Register the interrupt handler for IRQ index 2 (vector 0x22) */
            irq_register_handler(2, main_xhci_irq_handler);
        }

        g_xhc = xhci_init(xhci_mmio_base, xhci_irq);
        if (g_xhc != 0) {
            if (xhci_start(g_xhc) == XHCI_SUCCESS) {
                xhci_probe_ports(g_xhc);
                
                /* Link the callback to our newly created HID translator */
                xhci_register_keyboard_callback(g_xhc, usb_keyboard_handler);
                kprint("[OK] xHCI USB controller & keyboard active\n");
            }
        } else {
            kprint("[FAIL] xHCI controller failed to initialize\n");
        }
    } else {
        kprint("No xHCI controller found.\n");
    }

    kprint("[OK] Booting complete.\n\n");

    console_init();
    kmon_init();
    
    for (;;)
    {
        asm volatile("hlt");
    }
}
