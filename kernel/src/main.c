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
#include <scheduler.h>
#include <initramfs.h>
#include <storage/fat32.h>
#include <storage/mgfs.h>
#include <string.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

#include <kmon/pci.h>
#include <kmon/ahci.h>

extern char __stack_top[];
extern char __stack_bottom[];

static void scheduler_probe_entry(void *argument)
{
    (void)argument;
}

static kernel_thread_t *scheduler_priority_high;
static kernel_thread_t *scheduler_priority_n1;
static kernel_thread_t *scheduler_priority_n2;
static kernel_thread_t *scheduler_priority_background;
static bool scheduler_test_failed;
static char scheduler_priority_log[8];
static u32 scheduler_priority_log_length;

static void scheduler_priority_record(char marker)
{
    if (scheduler_priority_log_length < sizeof(scheduler_priority_log)) {
        scheduler_priority_log[scheduler_priority_log_length++] = marker;
    } else {
        scheduler_test_failed = true;
    }
}

static void scheduler_priority_high_entry(void *argument)
{
    (void)argument;
    if (thread_current() != scheduler_priority_high) {
        scheduler_test_failed = true;
    }
    scheduler_priority_record('H');
}

static void scheduler_priority_n1_entry(void *argument)
{
    (void)argument;
    if (thread_current() != scheduler_priority_n1) {
        scheduler_test_failed = true;
    }
    scheduler_priority_record('1');
    if (!scheduler_yield()) {
        scheduler_test_failed = true;
    }
    if (thread_current() != scheduler_priority_n1) {
        scheduler_test_failed = true;
    }
    scheduler_priority_record('1');
}

static void scheduler_priority_n2_entry(void *argument)
{
    (void)argument;
    if (thread_current() != scheduler_priority_n2) {
        scheduler_test_failed = true;
    }
    scheduler_priority_record('2');
    if (!scheduler_yield()) {
        scheduler_test_failed = true;
    }
    if (thread_current() != scheduler_priority_n2) {
        scheduler_test_failed = true;
    }
    scheduler_priority_record('2');
}

static void scheduler_priority_background_entry(void *argument)
{
    (void)argument;
    if (thread_current() != scheduler_priority_background) {
        scheduler_test_failed = true;
    }
    scheduler_priority_record('B');
}

static void scheduler_priority_test(void)
{
    bool passed;

    scheduler_test_failed = false;
    scheduler_priority_log_length = 0;
    scheduler_priority_high = thread_create_with_priority(
        "scheduler-high", scheduler_priority_high_entry, NULL,
        THREAD_PRIORITY_HIGH);
    scheduler_priority_n1 = thread_create_with_priority(
        "scheduler-normal-1", scheduler_priority_n1_entry, NULL,
        THREAD_PRIORITY_NORMAL);
    scheduler_priority_n2 = thread_create_with_priority(
        "scheduler-normal-2", scheduler_priority_n2_entry, NULL,
        THREAD_PRIORITY_NORMAL);
    scheduler_priority_background = thread_create_with_priority(
        "scheduler-background", scheduler_priority_background_entry, NULL,
        THREAD_PRIORITY_BACKGROUND);

    if (!scheduler_priority_high || !scheduler_priority_n1 ||
        !scheduler_priority_n2 || !scheduler_priority_background ||
        scheduler_priority_high->priority != THREAD_PRIORITY_HIGH ||
        scheduler_priority_n1->priority != THREAD_PRIORITY_NORMAL ||
        scheduler_priority_n2->priority != THREAD_PRIORITY_NORMAL ||
        scheduler_priority_background->priority != THREAD_PRIORITY_BACKGROUND ||
        !scheduler_reschedule()) {
        scheduler_test_failed = true;
    }

    passed = !scheduler_test_failed && scheduler_priority_log_length == 6 &&
        scheduler_priority_log[0] == 'H' &&
        scheduler_priority_log[1] == '1' &&
        scheduler_priority_log[2] == '2' &&
        scheduler_priority_log[3] == '1' &&
        scheduler_priority_log[4] == '2' &&
        scheduler_priority_log[5] == 'B' &&
        thread_current() && thread_current()->id == 1 &&
        scheduler_priority_high->state == THREAD_STATE_TERMINATED &&
        scheduler_priority_n1->state == THREAD_STATE_TERMINATED &&
        scheduler_priority_n2->state == THREAD_STATE_TERMINATED &&
        scheduler_priority_background->state == THREAD_STATE_TERMINATED;

    if (passed) {
        kprint("Scheduler: priority queues and round-robin test passed\n");
    } else {
        kprint("Scheduler: priority queues and round-robin test failed\n");
    }

    if (scheduler_priority_high) thread_destroy(scheduler_priority_high);
    if (scheduler_priority_n1) thread_destroy(scheduler_priority_n1);
    if (scheduler_priority_n2) thread_destroy(scheduler_priority_n2);
    if (scheduler_priority_background) thread_destroy(scheduler_priority_background);
    scheduler_priority_high = NULL;
    scheduler_priority_n1 = NULL;
    scheduler_priority_n2 = NULL;
    scheduler_priority_background = NULL;
}

static volatile char scheduler_timer_log[32];
static volatile u32 scheduler_timer_log_length;
static bool scheduler_timer_test_failed;
static kernel_thread_t *scheduler_timer_h1;
static kernel_thread_t *scheduler_timer_h2;
static kernel_thread_t *scheduler_timer_n1;
static kernel_thread_t *scheduler_timer_b1;

static void scheduler_timer_record(char marker)
{
    if (scheduler_timer_log_length < sizeof(scheduler_timer_log)) {
        scheduler_timer_log[scheduler_timer_log_length++] = marker;
    } else {
        scheduler_timer_test_failed = true;
    }
}

static void scheduler_timer_wait(void)
{
    u64 start = timer_ticks();
    while (timer_ticks() - start < 8) {
        __asm__ volatile("pause");
    }
}

static void scheduler_timer_h1_entry(void *argument)
{
    (void)argument;
    if (thread_current() != scheduler_timer_h1) scheduler_timer_test_failed = true;
    scheduler_timer_record('1');
    scheduler_timer_wait();
    scheduler_timer_record('1');
}

static void scheduler_timer_h2_entry(void *argument)
{
    (void)argument;
    if (thread_current() != scheduler_timer_h2) scheduler_timer_test_failed = true;
    scheduler_timer_record('2');
    scheduler_timer_wait();
    scheduler_timer_record('2');
}

static void scheduler_timer_n1_entry(void *argument)
{
    (void)argument;
    if (thread_current() != scheduler_timer_n1) scheduler_timer_test_failed = true;
    scheduler_timer_record('N');
    scheduler_timer_wait();
    scheduler_timer_record('N');
}

static void scheduler_timer_b1_entry(void *argument)
{
    (void)argument;
    if (thread_current() != scheduler_timer_b1) scheduler_timer_test_failed = true;
    scheduler_timer_record('B');
    scheduler_timer_wait();
    scheduler_timer_record('B');
}

static void scheduler_timer_test(void)
{
    bool passed;
    u32 first_normal = 0;
    u32 first_background = 0;
    u32 i;

    __asm__ volatile("cli");
    scheduler_timer_test_failed = false;
    scheduler_timer_log_length = 0;
    scheduler_timer_h1 = thread_create_with_priority(
        "timer-high-1", scheduler_timer_h1_entry, NULL, THREAD_PRIORITY_HIGH);
    scheduler_timer_h2 = thread_create_with_priority(
        "timer-high-2", scheduler_timer_h2_entry, NULL, THREAD_PRIORITY_HIGH);
    scheduler_timer_n1 = thread_create_with_priority(
        "timer-normal", scheduler_timer_n1_entry, NULL, THREAD_PRIORITY_NORMAL);
    scheduler_timer_b1 = thread_create_with_priority(
        "timer-background", scheduler_timer_b1_entry, NULL,
        THREAD_PRIORITY_BACKGROUND);
    __asm__ volatile("sti");

    if (!scheduler_timer_h1 || !scheduler_timer_h2 || !scheduler_timer_n1 ||
        !scheduler_timer_b1 || !scheduler_reschedule()) {
        scheduler_timer_test_failed = true;
    }

    for (i = 0; i < scheduler_timer_log_length; i++) {
        if (!first_normal && scheduler_timer_log[i] == 'N') first_normal = i + 1;
        if (!first_background && scheduler_timer_log[i] == 'B') first_background = i + 1;
    }
    passed = !scheduler_timer_test_failed &&
        timer_preemptions() > 0 && scheduler_timer_log_length == 8 &&
        scheduler_timer_log[0] == '1' &&
        scheduler_timer_log[1] == '2' &&
        scheduler_timer_log[2] == '1' &&
        scheduler_timer_log[3] == '2' &&
        first_normal >= 5 && first_background >= 7 &&
        scheduler_timer_h1->state == THREAD_STATE_TERMINATED &&
        scheduler_timer_h2->state == THREAD_STATE_TERMINATED &&
        scheduler_timer_n1->state == THREAD_STATE_TERMINATED &&
        scheduler_timer_b1->state == THREAD_STATE_TERMINATED &&
        thread_current() && thread_current()->id == 1;

    if (passed) {
        kprint("Scheduler: timer preemption test passed\n");
    } else {
        kprint("Scheduler: timer preemption test failed\n");
    }

    if (scheduler_timer_h1) thread_destroy(scheduler_timer_h1);
    if (scheduler_timer_h2) thread_destroy(scheduler_timer_h2);
    if (scheduler_timer_n1) thread_destroy(scheduler_timer_n1);
    if (scheduler_timer_b1) thread_destroy(scheduler_timer_b1);
    scheduler_timer_h1 = NULL;
    scheduler_timer_h2 = NULL;
    scheduler_timer_n1 = NULL;
    scheduler_timer_b1 = NULL;
}

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

    if (scheduler_init() && thread_current() &&
        thread_current()->state == THREAD_STATE_RUNNING) {
        kprint("[OK] Scheduler initialized: thread %u (%s) is running\n",
               (u32)thread_current()->id,
               thread_current()->name);

        kernel_thread_t *probe = thread_create("scheduler-probe",
                                                scheduler_probe_entry, NULL);
        if (probe && probe->id != thread_current()->id &&
            probe->state == THREAD_STATE_READY &&
            probe->kernel_stack_base != thread_current()->kernel_stack_base &&
            probe->saved_stack_pointer >= probe->kernel_stack_base &&
            probe->saved_stack_pointer <
                probe->kernel_stack_base + probe->kernel_stack_size &&
            (probe->saved_stack_pointer & 0x0f) == 0) {
            kprint("[OK] Scheduler prepared thread %u with a dedicated stack\n",
                   (u32)probe->id);
            if (!thread_destroy(probe)) {
                kprint("[FAIL] Scheduler probe cleanup failed\n");
            }
        } else {
            kprint("[FAIL] Scheduler thread preparation verification failed\n");
            if (probe) {
                thread_destroy(probe);
            }
        }

        scheduler_priority_test();
    } else {
        kprint("[FAIL] Scheduler bootstrap thread initialization failed\n");
    }

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
    mgfs_init();

    bool root_mounted = false;
    bool fat32_mounted = false;
    if (block_device_count() > 1) {
        block_device_t *bdev = block_get_device(1);
        if (bdev) {
            vfs_fs_type_t *mgfs_driver = vfs_find_fs("mgfs");
            if (mgfs_driver && mgfs_driver->probe && mgfs_driver->probe(bdev)) {
                int mount_result = vfs_mount_root("mgfs", bdev);
                if (mount_result == VFS_OK) {
                    root_mounted = true;
                    kprint("[OK] Mounted MGFS test disk as VFS root filesystem ('/')\n");
                } else {
                    kprint("[FAIL] MGFS mount rejected: %s (error: %d)\n",
                           mgfs_last_error(), mount_result);
                }
            } else {
                vfs_fs_type_t *fat32_driver = vfs_find_fs("fat32");
                if (fat32_driver && fat32_driver->probe && fat32_driver->probe(bdev)) {
                    if (vfs_mount_root("fat32", bdev) == VFS_OK) {
                        root_mounted = true;
                        kprint("[OK] Mounted FAT32 test disk as VFS root filesystem ('/')\n");
                        fat32_mounted = true;
                    }
                }
            }
        }
    }

    if (!root_mounted) {
        if (vfs_mount_root("initramfs", NULL) == VFS_OK) {
            root_mounted = true;
            kprint("[OK] Mounted Initramfs RAM filesystem as VFS root filesystem ('/')\n");
        }
    }

    vfs_node_t *root_node = vfs_get_root_node();
    if (root_node && root_node->super) {
        vfs_super_t *sb = root_node->super;
        vfs_dirent_t ent;
        u32 idx = 0;
        char first_file_path[256] = { '/', '\0' };
        bool first_file_seen = false;
        kprint("[OK] VFS root is a %s; enumerating through VFS:\n",
               root_node->type == VFS_TYPE_DIRECTORY ? "directory" : "non-directory");
        while (vfs_readdir(root_node, idx, &ent)) {
            kprint("  - %s (%s, inode: %u)\n",
                   ent.name,
                   (ent.type == VFS_TYPE_DIRECTORY) ? "DIR" : "FILE",
                   (u32)ent.inode);
            if (!first_file_seen && ent.type == VFS_TYPE_FILE && strlen(ent.name) < sizeof(first_file_path) - 1) {
                strcpy(first_file_path + 1, ent.name);
                first_file_seen = true;
            }
            idx++;
        }

        vfs_file_handle_t *verification_handle = NULL;
        if (vfs_open("/", VFS_OPEN_READ, &verification_handle) == VFS_OK &&
            verification_handle && verification_handle->node == root_node &&
            verification_handle->node->type == VFS_TYPE_DIRECTORY &&
            verification_handle->offset == 0) {
            kprint("[OK] VFS open('/') returned a directory handle at offset 0\n");
            vfs_close(verification_handle);
        } else {
            kprint("[FAIL] VFS open('/') verification failed\n");
        }

        if (first_file_seen) {
            verification_handle = NULL;
            if (vfs_open(first_file_path, VFS_OPEN_READ, &verification_handle) == VFS_OK && verification_handle) {
                char verification_byte[1];
                u64 before = verification_handle->offset;
                u64 first_read = vfs_file_read(verification_handle, sizeof(verification_byte), verification_byte);
                u64 reset_offset = 0;
                int seek_result = vfs_seek(verification_handle, 0, VFS_SEEK_SET, &reset_offset);
                u64 second_read = vfs_file_read(verification_handle, sizeof(verification_byte), verification_byte);
                kprint("[OK] VFS handle '%s': open offset=%u, read=%u, seek=%d, reread=%u\n",
                       first_file_path, (u32)before, (u32)first_read, seek_result, (u32)second_read);
                vfs_close(verification_handle);
            } else {
                kprint("[FAIL] VFS open('%s') verification failed\n", first_file_path);
            }
        }

        verification_handle = NULL;
        if (vfs_open("/__vfs_missing__", VFS_OPEN_READ, &verification_handle) == VFS_ERR_NOT_FOUND &&
            verification_handle == NULL) {
            kprint("[OK] VFS open() rejects a nonexistent path\n");
        } else {
            kprint("[FAIL] VFS open() nonexistent-path verification failed\n");
            if (verification_handle) vfs_close(verification_handle);
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

    scheduler_timer_test();

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
