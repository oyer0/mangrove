#include <init.h>

#include <acpi.h>
#include <ahci.h>
#include <block.h>
#include <net/dhcp.h>
#include <net/e1000.h>
#include <net/rtl8168.h>
#include <net/ethernet.h>
#include <net/arp.h>
#include <storage/fat32.h>
#include <storage/gpt.h>
#include <net/icmp.h>
#include <initramfs.h>
#include <ioapic.h>
#include <irq.h>
#include <kprint.h>
#include <lapic.h>
#include <storage/mgfs.h>
#include <net/net.h>
#include <net/config.h>
#include <pci.h>
#include <pmm.h>
#include <platform_power.h>
#include <process.h>
#include <scheduler.h>
#include <timer.h>
#include <net/udp.h>
#include <vfs.h>
#include <vmm.h>
#include <xhci.h>
#include <net/dns.h>
#include <net/ipv4.h>
#include <net/tcp.h>
#include <memory_types.h>
#include <string.h>

#include <stddef.h>

extern void usb_keyboard_handler(u8 modifier_mask, const u8 *key_codes, u8 count);
#ifdef PITH_DEBUG_BOOT_TESTS
extern void kernel_debug_scheduler_tests(void);
extern void kernel_debug_runtime_tests(void);
#endif

static xhci_controller_t *g_xhc;
static volatile u32 g_xhci_irq_entries;

static void init_xhci_irq_handler(struct cpu_registers *regs)
{
    u32 entry = __atomic_add_fetch(&g_xhci_irq_entries, 1,
                                    __ATOMIC_RELAXED);
    if (entry <= 3)
        XHCI_DEBUG_LOG("[xHCI-ISR] enter n=%u v=%x\n", entry,
                       (u32)regs->vec_no);

    if (g_xhc)
        xhci_interrupt_handler(g_xhc);

    if (entry <= 3)
        XHCI_DEBUG_LOG("[xHCI-ISR] exit n=%u\n", entry);
}

static void init_acpi_sci_handler(struct cpu_registers *regs)
{
    (void)regs;
    acpi_sci_interrupt();
}

static init_result_t init_process(const char **reason)
{
    if (!process_init()) {
        *reason = "process registry initialization failed";
        return INIT_RESULT_FAILED;
    }
    return INIT_RESULT_OK;
}

static init_result_t init_scheduler(const char **reason)
{
    if (!scheduler_init() || !thread_current() ||
        thread_current()->state != THREAD_STATE_RUNNING) {
        *reason = "bootstrap thread initialization failed";
        return INIT_RESULT_FAILED;
    }
#ifdef PITH_DEBUG_BOOT_TESTS
    kernel_debug_scheduler_tests();
#endif
    return INIT_RESULT_OK;
}

static init_result_t init_irq_routing(const char **reason)
{
    lapic_init();
    if (!lapic_present()) {
        *reason = "local APIC unavailable";
        return INIT_RESULT_FAILED;
    }
    lapic_enable();
    if (!lapic_enabled()) {
        *reason = "local APIC enable failed";
        return INIT_RESULT_FAILED;
    }

    ioapic_init();
    if (!ioapic_present()) {
        *reason = "I/O APIC unavailable";
        return INIT_RESULT_FAILED;
    }

    u8 apic_id = (u8)(lapic_read(LAPIC_ID) >> 24);
    if (!ioapic_route_gsi(acpi_irq_to_gsi(0), IRQ_VECTOR_PIT, apic_id,
                          acpi_irq_flags(0)) ||
        !ioapic_route_gsi(acpi_irq_to_gsi(1), IRQ_VECTOR_PS2, apic_id,
                          acpi_irq_flags(1))) {
        *reason = "PIT/PS2 I/O APIC route failed";
        return INIT_RESULT_FAILED;
    }

    const acpi_fadt_info_t *fadt = acpi_fadt_get();
    if (fadt && fadt->sci_interrupt < 256U) {
        u16 flags = acpi_irq_flags((u8)fadt->sci_interrupt);
        if (!flags)
            flags = ACPI_IRQ_FLAGS_ACTIVE_LOW_LEVEL;
        if (!irq_register_vector(IRQ_VECTOR_ACPI_SCI, init_acpi_sci_handler)) {
            *reason = "ACPI SCI vector already owned";
            return INIT_RESULT_FAILED;
        }
        if (!ioapic_route_gsi(acpi_irq_to_gsi((u8)fadt->sci_interrupt),
                              IRQ_VECTOR_ACPI_SCI, apic_id, flags)) {
            irq_unregister_vector(IRQ_VECTOR_ACPI_SCI);
            *reason = "ACPI SCI I/O APIC route failed";
            return INIT_RESULT_FAILED;
        }
    }
    (void)acpi_events_prepare();
    KERNEL_BOOT_DEBUG_LOG(
        "[INIT] irq routing ready: LAPIC/IOAPIC, PIC masked\n");
    return INIT_RESULT_OK;
}

static init_result_t init_acpi_events(const char **reason)
{
    /* Install the callback before waking the worker.  Firmware may already
     * have a pending GPE by the time CPU interrupts are enabled. */
    aml_set_notify_handler(platform_power_acpi_notify);
    (void)platform_lid_initialize();
    if (!acpi_events_start()) {
        *reason = "no usable ACPI GPE event source";
        return INIT_RESULT_UNAVAILABLE;
    }
    if (!platform_lid_available())
        KERNEL_BOOT_DEBUG_LOG("[ACPI] lid unavailable\n");
    return INIT_RESULT_OK;
}

static init_result_t init_vfs(const char **reason)
{
    (void)reason;
    vfs_init();
    return INIT_RESULT_OK;
}

static init_result_t init_pci(const char **reason)
{
    (void)reason;
    pci_init();
    return INIT_RESULT_OK;
}

static init_result_t init_ahci(const char **reason)
{
    ahci_init();
    if (!ahci_present()) {
        *reason = "no supported AHCI controller";
        return INIT_RESULT_UNAVAILABLE;
    }
    return INIT_RESULT_OK;
}

static init_result_t init_network_core(const char **reason)
{
    (void)reason;
    net_init();
    net_config_init();
    return INIT_RESULT_OK;
}

static init_result_t init_network_device(const char **reason)
{
    rtl8168_init_result_t realtek_result;
    const char *realtek_reason = NULL;

    if (e1000_init()) {
        return INIT_RESULT_OK;
    }

    realtek_result = rtl8168_init(&realtek_reason);
    if (realtek_result == RTL8168_INIT_READY)
        return INIT_RESULT_OK;

    if (realtek_result == RTL8168_INIT_FAILED) {
        *reason = realtek_reason ? realtek_reason :
            "RTL8168h initialization failed";
        return INIT_RESULT_FAILED;
    }

    if (realtek_result == RTL8168_INIT_UNAVAILABLE) {
        *reason = realtek_reason ? realtek_reason :
            "RTL8168h unavailable";
        return INIT_RESULT_UNAVAILABLE;
    }

    *reason = "no supported Ethernet controller";
    return INIT_RESULT_UNAVAILABLE;
}

static init_result_t init_network_protocols(const char **reason)
{
    (void)reason;
    ethernet_init();
    arp_init();
    ipv4_init();
    icmp_init();
    udp_init();
    dhcp_init();
    dns_init();
    tcp_init();
    return INIT_RESULT_OK;
}

static init_result_t init_filesystems(const char **reason)
{
    if (initramfs_init() != VFS_OK ||
        fat32_init() != VFS_OK ||
        mgfs_init() != VFS_OK) {
        *reason = "filesystem registration failed";
        return INIT_RESULT_FAILED;
    }
    return INIT_RESULT_OK;
}

static init_result_t init_cpu_irqs_enabled(const char **reason)
{
    (void)reason;
    __asm__ volatile("sti" ::: "memory");
    return INIT_RESULT_OK;
}

static init_result_t init_network_config(const char **reason)
{
    net_device_t *network_device = net_primary_device();
    if (!network_device) {
        *reason = "no network device to configure";
#ifdef PITH_DEBUG_BOOT_TESTS
        kernel_debug_runtime_tests();
#endif
        return INIT_RESULT_UNAVAILABLE;
    }

    dhcp_lease_t lease;
    if (!dhcp_acquire(network_device, &lease) ||
        !net_config_apply_dhcp(&lease.address, &lease.netmask,
                               &lease.gateway, lease.has_gateway,
                               &lease.dns, lease.has_dns,
                               &lease.server, lease.lease_seconds)) {
        *reason = "DHCP configuration unavailable";
#ifdef PITH_DEBUG_BOOT_TESTS
        kernel_debug_runtime_tests();
#endif
        return INIT_RESULT_FAILED;
    }

    const net_config_t *configuration = net_config();
    kprint("[INIT] network configured: %u.%u.%u.%u\n",
           configuration->address.octet[0], configuration->address.octet[1],
           configuration->address.octet[2], configuration->address.octet[3]);
#ifdef PITH_DEBUG_BOOT_TESTS
    kernel_debug_runtime_tests();
#endif
    return INIT_RESULT_OK;
}

static init_result_t init_xhci(const char **reason)
{
    const pci_device_t *xhci_pdev = NULL;
    phys_addr_t xhci_mmio_phys = 0;
    const usize xhci_mmio_size = 0x4000;
    u8 xhci_irq = 11;

    for (u32 i = 0; i < pci_get_device_count(); i++) {
        const pci_device_t *pdev = pci_get_device(i);
        if (!pdev) continue;
        if (pdev->class_code == 0x0C && pdev->subclass == 0x03 &&
            pdev->prog_if == 0x30) {
            xhci_pdev = pdev;
            xhci_mmio_phys = (phys_addr_t)pci_get_bar(pdev, 0).address;
            break;
        }
    }

    if (!xhci_pdev) {
        *reason = "no xHCI controller";
        return INIT_RESULT_UNAVAILABLE;
    }

    uintptr_t xhci_mmio_base =
        (uintptr_t)vmm_map_mmio(xhci_mmio_phys, xhci_mmio_size);
    if (!xhci_mmio_base) {
        *reason = "xHCI MMIO mapping failed";
        return INIT_RESULT_FAILED;
    }

    if (!irq_register_vector(IRQ_VECTOR_XHCI, init_xhci_irq_handler)) {
        *reason = "xHCI interrupt vector already owned";
        return INIT_RESULT_FAILED;
    }
    g_xhc = xhci_init(xhci_mmio_base, xhci_irq);
    if (!g_xhc) {
        irq_unregister_vector(IRQ_VECTOR_XHCI);
        *reason = "xHCI controller initialization failed";
        return INIT_RESULT_FAILED;
    }

    bool msix_prepared = false;
    pci_msix_info_t msix_info = {0};
    u8 apic_id = (u8)(lapic_read(LAPIC_ID) >> 24);

    if (lapic_enabled() &&
        pci_get_msix_info(xhci_pdev, &msix_info) &&
        msix_info.table_address <= ~(u64)0 - (16 + PAGE_SIZE - 1)) {
        if (pci_map_msix_table(&msix_info)) {
            msix_prepared = pci_prepare_msix_vector(
                xhci_pdev, &msix_info, 0, apic_id, IRQ_VECTOR_XHCI);
        }
    }

    if (!msix_prepared && !ioapic_present()) {
        irq_unregister_vector(IRQ_VECTOR_XHCI);
        *reason = "no routable xHCI interrupt path";
        xhci_shutdown(g_xhc);
        g_xhc = NULL;
        return INIT_RESULT_UNAVAILABLE;
    }

    if (!msix_prepared && ioapic_present()) {
        if (!ioapic_route_gsi(acpi_irq_to_gsi(xhci_irq), IRQ_VECTOR_XHCI,
                              apic_id, acpi_irq_flags(xhci_irq))) {
            irq_unregister_vector(IRQ_VECTOR_XHCI);
            xhci_shutdown(g_xhc);
            g_xhc = NULL;
            *reason = "xHCI I/O APIC route failed";
            return INIT_RESULT_FAILED;
        }
    }

    if (xhci_start(g_xhc) != XHCI_SUCCESS) {
        if (msix_prepared)
            pci_disable_msix(xhci_pdev, &msix_info, 0);
        irq_unregister_vector(IRQ_VECTOR_XHCI);
        g_xhc = NULL;
        *reason = "xHCI start failed";
        return INIT_RESULT_FAILED;
    }

    xhci_probe_ports(g_xhc);
    xhci_acknowledge_boot_interrupts(g_xhc);
    xhci_complete_boot_enumeration(g_xhc);

    if (msix_prepared) {
        if (!pci_unmask_msix_vector(xhci_pdev, &msix_info, 0) &&
            ioapic_present()) {
            (void)ioapic_route_gsi(acpi_irq_to_gsi(xhci_irq), IRQ_VECTOR_XHCI,
                                   apic_id, acpi_irq_flags(xhci_irq));
        }
    }

    xhci_register_keyboard_callback(g_xhc, usb_keyboard_handler);
    xhci_resume_keyboard(g_xhc);
    return INIT_RESULT_OK;
}

#ifdef PITH_DEBUG_BOOT_TESTS
static void init_debug_vfs_tests(bool fat32_mounted)
{
    vfs_node_t *root_node = vfs_get_root_node();
    if (!root_node || !root_node->super)
        return;

    vfs_super_t *sb = root_node->super;
    vfs_dirent_t ent;
    u32 idx = 0;
    char first_file_path[256] = { '/', '\0' };
    bool first_file_seen = false;
    kprint("[OK] VFS root is a %s; enumerating through VFS:\n",
           root_node->type == VFS_TYPE_DIRECTORY ? "directory" : "non-directory");
    while (vfs_readdir(root_node, idx, &ent)) {
        kprint("  - %s (%s, inode: %u)\n", ent.name,
               ent.type == VFS_TYPE_DIRECTORY ? "DIR" : "FILE",
               (u32)ent.inode);
        if (!first_file_seen && ent.type == VFS_TYPE_FILE &&
            strlen(ent.name) < sizeof(first_file_path) - 1) {
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
        if (vfs_open(first_file_path, VFS_OPEN_READ, &verification_handle) == VFS_OK &&
            verification_handle) {
            char verification_byte[1];
            u64 before = verification_handle->offset;
            u64 first_read = vfs_file_read(verification_handle,
                                           sizeof(verification_byte),
                                           verification_byte);
            u64 reset_offset = 0;
            int seek_result = vfs_seek(verification_handle, 0, VFS_SEEK_SET,
                                       &reset_offset);
            u64 second_read = vfs_file_read(verification_handle,
                                            sizeof(verification_byte),
                                            verification_byte);
            kprint("[OK] VFS handle '%s': open offset=%u, read=%u, seek=%d, reread=%u\n",
                   first_file_path, (u32)before, (u32)first_read,
                   seek_result, (u32)second_read);
            vfs_close(verification_handle);
        } else {
            kprint("[FAIL] VFS open('%s') verification failed\n",
                   first_file_path);
        }
    }

    verification_handle = NULL;
    if (vfs_open("/__vfs_missing__", VFS_OPEN_READ,
                 &verification_handle) == VFS_ERR_NOT_FOUND &&
        verification_handle == NULL) {
        kprint("[OK] VFS open() rejects a nonexistent path\n");
    } else {
        kprint("[FAIL] VFS open() nonexistent-path verification failed\n");
        if (verification_handle)
            vfs_close(verification_handle);
    }

    if (!fat32_mounted)
        return;

    u32 alloc1 = fat32_alloc_cluster(sb);
    u32 alloc2 = fat32_extend_chain(sb, alloc1);
    u32 link1 = fat32_get_cluster_link(sb, alloc1);
    kprint("[OK] FAT Allocation Primitives Test: alloc1=%u, alloc2=%u, link1=%u\n",
           alloc1, alloc2, link1);
    fat32_free_chain(sb, alloc1);
    kprint("[OK] FAT Free Chain Test: freed_link=%u\n",
           fat32_get_cluster_link(sb, alloc1));

    vfs_node_t *created_file = NULL;
    if (vfs_create(root_node, "NEWFILE.TXT", &created_file) == VFS_OK &&
        created_file) {
        const char *msg = "Hello from Pith FAT32 file creation!";
        u64 written = vfs_write(created_file, 0, strlen(msg), msg);
        char read_back[128];
        u64 read = vfs_read(created_file, 0, sizeof(read_back) - 1,
                            read_back);
        read_back[read] = '\0';
        kprint("[OK] VFS File Lifecycle Test: '%s' (%u written, %u read)\n",
               read_back, (u32)written, (u32)read);
    }

    vfs_node_t *new_dir = NULL;
    if (vfs_mkdir(root_node, "DOCS", &new_dir) == VFS_OK && new_dir) {
        kprint("[OK] VFS Directory Creation Test: Created '/DOCS' (inode: %u)\n",
               (u32)new_dir->inode);
        vfs_node_t *sub_file = NULL;
        if (vfs_create(new_dir, "NOTES.TXT", &sub_file) == VFS_OK && sub_file) {
            const char *sub_msg = "Nested file inside FAT32 /DOCS directory!";
            vfs_write(sub_file, 0, strlen(sub_msg), sub_msg);
            char sub_read[128];
            u64 sub_bytes = vfs_read(sub_file, 0, sizeof(sub_read) - 1,
                                     sub_read);
            sub_read[sub_bytes] = '\0';
            kprint("[OK] Nested File Lifecycle Test: '%s' (%u bytes read)\n",
                   sub_read, (u32)sub_bytes);
            if (vfs_unlink(new_dir, "NOTES.TXT") == VFS_OK)
                kprint("[OK] VFS Unlink Test: Deleted '/DOCS/NOTES.TXT'\n");
        }
        if (vfs_rmdir(root_node, "DOCS") == VFS_OK)
            kprint("[OK] VFS Rmdir Test: Removed empty '/DOCS'\n");
    }
    vfs_unlink(root_node, "NEWFILE.TXT");
}
#endif

static init_result_t init_rootfs(const char **reason)
{
    bool root_mounted = false;
    bool mgfs_mounted = false;
    bool fat32_mounted = false;

    for (u32 i = 0; i < block_device_count() && !root_mounted; i++) {
        block_device_t *bdev = block_get_device(i);
        vfs_fs_type_t *driver = vfs_find_fs("mgfs");
        if (!bdev || !driver || !driver->probe || !driver->probe(bdev))
            continue;
        if (vfs_mount_root("mgfs", bdev) == VFS_OK) {
            root_mounted = true;
            mgfs_mounted = true;
            KERNEL_BOOT_DEBUG_LOG("[ROOTFS] mounted MGFS\n");
        }
    }

    if (!root_mounted) {
        for (u32 i = 0; i < block_device_count() && !root_mounted; i++) {
            block_device_t *bdev = block_get_device(i);
            vfs_fs_type_t *driver = vfs_find_fs("fat32");
            if (!bdev || !driver || !driver->probe || !driver->probe(bdev))
                continue;
            if (vfs_mount_root("fat32", bdev) == VFS_OK) {
                root_mounted = true;
                fat32_mounted = true;
                KERNEL_BOOT_DEBUG_LOG("[ROOTFS] mounted FAT32\n");
            }
        }
    }

    if (!root_mounted && vfs_mount_root("initramfs", NULL) == VFS_OK) {
        root_mounted = true;
        KERNEL_BOOT_DEBUG_LOG("[ROOTFS] mounted initramfs\n");
    }

    if (g_xhc)
        xhci_print_boot_summary(g_xhc, mgfs_mounted);

#ifdef PITH_DEBUG_BOOT_TESTS
    if (root_mounted)
        init_debug_vfs_tests(fat32_mounted);
#endif

    if (!root_mounted) {
        *reason = "MGFS, FAT32, and initramfs root mounting all failed";
        return INIT_RESULT_FAILED;
    }
    return INIT_RESULT_OK;
}

#define INIT_BIT(id) (1ULL << (id))

typedef init_result_t (*init_start_fn)(const char **reason);

typedef struct {
    init_status_t status;
    u64 required_deps;
    u64 optional_deps;
    bool required_for_boot;
    init_start_fn start;
} init_descriptor_t;

static init_descriptor_t descriptors[INIT_COUNT] = {
    [INIT_PROCESS] = {{"process", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                      0, 0, true, init_process},
    [INIT_SCHEDULER] = {{"scheduler", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                        0, 0, true, init_scheduler},
    [INIT_IRQ_ROUTING] = {{"irq routing", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                          0, 0, false, init_irq_routing},
    [INIT_VFS] = {{"vfs", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                  0, 0, true, init_vfs},
    [INIT_PCI] = {{"pci", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                  INIT_BIT(INIT_IRQ_ROUTING), 0, false, init_pci},
    [INIT_AHCI] = {{"ahci", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                  INIT_BIT(INIT_PCI), 0, false, init_ahci},
    [INIT_NETWORK_CORE] = {{"network core", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                           0, INIT_BIT(INIT_AHCI), false, init_network_core},
    [INIT_NETWORK_DEVICE] = {{"network device", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                              INIT_BIT(INIT_PCI) | INIT_BIT(INIT_IRQ_ROUTING) |
                                  INIT_BIT(INIT_NETWORK_CORE),
                              0, false, init_network_device},
    [INIT_NETWORK_PROTOCOLS] = {{"network protocols", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                                INIT_BIT(INIT_NETWORK_CORE),
                                INIT_BIT(INIT_NETWORK_DEVICE), false,
                                init_network_protocols},
    [INIT_FILESYSTEMS] = {{"filesystems", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                          INIT_BIT(INIT_VFS),
                          INIT_BIT(INIT_NETWORK_PROTOCOLS), true, init_filesystems},
    [INIT_CPU_IRQS_ENABLED] = {{"cpu IRQs", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                               INIT_BIT(INIT_FILESYSTEMS), 0, true,
                               init_cpu_irqs_enabled},
    [INIT_NETWORK_CONFIG] = {{"network configuration", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                             INIT_BIT(INIT_CPU_IRQS_ENABLED) |
                                 INIT_BIT(INIT_NETWORK_PROTOCOLS) |
                                 INIT_BIT(INIT_NETWORK_DEVICE),
                             0, false,
                             init_network_config},
    [INIT_XHCI] = {{"xHCI", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                   INIT_BIT(INIT_PCI) | INIT_BIT(INIT_IRQ_ROUTING) |
                       INIT_BIT(INIT_CPU_IRQS_ENABLED) |
                       INIT_BIT(INIT_SCHEDULER),
                   INIT_BIT(INIT_NETWORK_CONFIG), false, init_xhci},
    [INIT_ROOTFS] = {{"root filesystem", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                     INIT_BIT(INIT_VFS) | INIT_BIT(INIT_FILESYSTEMS),
                     INIT_BIT(INIT_AHCI) | INIT_BIT(INIT_XHCI), true,
                     init_rootfs},
    [INIT_ACPI_EVENTS] = {{"ACPI events", INIT_UNINITIALIZED,
                           INIT_RESULT_OK, NULL},
                          INIT_BIT(INIT_IRQ_ROUTING) |
                              INIT_BIT(INIT_SCHEDULER) |
                              INIT_BIT(INIT_CPU_IRQS_ENABLED),
                          0, false, init_acpi_events}
};

static bool init_graph_error;

static void init_print_production_summary(void)
{
    bool root_ready = descriptors[INIT_ROOTFS].status.state == INIT_READY;
    bool network_ready =
        descriptors[INIT_NETWORK_CONFIG].status.state == INIT_READY;

    if (block_device_count() != 0)
        kprint("Storage ready\n");
    else if (root_ready)
        kprint("Storage unavailable; using initramfs\n");

    if (network_ready)
        kprint("Network ready\n");
    else
        kprint("Network unavailable\n");

    if (root_ready)
        kprint("Filesystem ready\n");
    else
        kprint("[FAIL] Filesystem unavailable\n");
}

#define KERNEL_BOOT_LOG_WRITE_CHUNK 4096U

static bool init_persist_boot_log(void)
{
    vfs_node_t *root = vfs_get_root_node();
    vfs_node_t *core = NULL;
    vfs_node_t *logs = NULL;
    vfs_node_t *existing = NULL;
    vfs_node_t *boot_file = NULL;
    char buffer[KERNEL_BOOT_LOG_WRITE_CHUNK];
    usize size;

    /* Stop capture before touching VFS so the snapshot cannot change while
     * it is being copied into the new file.  This runs in normal kernel
     * context, after root filesystem selection, never from an IRQ path. */
    kprint_boot_log_stop();

    if (!root || root->type != VFS_TYPE_DIRECTORY)
        return false;

    if (vfs_lookup("/core", &core) != VFS_OK) {
        if (vfs_mkdir(root, "core", &core) != VFS_OK)
            return false;
    }
    if (!core || core->type != VFS_TYPE_DIRECTORY)
        return false;

    if (vfs_lookup("/core/logs", &logs) != VFS_OK) {
        if (vfs_mkdir(core, "logs", &logs) != VFS_OK)
            return false;
    }
    if (!logs || logs->type != VFS_TYPE_DIRECTORY)
        return false;

    if (vfs_lookup("/core/logs/boot.log", &existing) == VFS_OK) {
        if (!existing || existing->type != VFS_TYPE_FILE ||
            vfs_unlink(logs, "boot.log") != VFS_OK)
            return false;
    }

    if (vfs_create(logs, "boot.log", &boot_file) != VFS_OK ||
        !boot_file || boot_file->type != VFS_TYPE_FILE)
        return false;

    size = kprint_boot_log_size();
    for (usize offset = 0; offset < size;) {
        usize remaining = size - offset;
        usize chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        usize read = kprint_boot_log_read(offset, buffer, chunk);
        if (read != chunk || vfs_write(boot_file, offset, read, buffer) != read)
            return false;
        offset += read;
    }

    return true;
}

static bool init_id_valid(init_id_t id)
{
    return id >= 0 && id < INIT_COUNT;
}

static bool init_start_node(init_id_t id)
{
    init_descriptor_t *node;
    const char *reason = NULL;

    if (!init_id_valid(id))
        return false;

    node = &descriptors[id];
    if (node->status.state == INIT_READY)
        return true;
    if (node->status.state == INIT_UNAVAILABLE ||
        node->status.state == INIT_FAILED ||
        node->status.state == INIT_SKIPPED)
        return false;
    if (node->status.state == INIT_INITIALIZING) {
        init_graph_error = true;
        node->status.state = INIT_FAILED;
        node->status.result = INIT_RESULT_DEPENDENCY;
        node->status.reason = "dependency cycle";
        return false;
    }

    node->status.state = INIT_INITIALIZING;

    for (init_id_t dep = 0; dep < INIT_COUNT; dep++) {
        if (!(node->required_deps & INIT_BIT(dep)))
            continue;
        if (!init_start_node(dep)) {
            node->status.state = INIT_SKIPPED;
            node->status.result = INIT_RESULT_DEPENDENCY;
            node->status.reason = descriptors[dep].status.reason ?
                descriptors[dep].status.reason : descriptors[dep].status.name;
            if (node->required_for_boot)
                kprint("[FAIL] %s skipped: dependency %s\n",
                       node->status.name, descriptors[dep].status.name);
            return false;
        }
    }

    for (init_id_t dep = 0; dep < INIT_COUNT; dep++) {
        if (!(node->optional_deps & INIT_BIT(dep)))
            continue;
        if (!init_start_node(dep) && init_graph_error) {
            node->status.state = INIT_FAILED;
            node->status.result = INIT_RESULT_DEPENDENCY;
            node->status.reason = "dependency cycle";
            return false;
        }
    }

    node->status.result = node->start(&reason);
    node->status.reason = reason;
    switch (node->status.result) {
    case INIT_RESULT_OK:
        node->status.state = INIT_READY;
        return true;
    case INIT_RESULT_UNAVAILABLE:
        node->status.state = INIT_UNAVAILABLE;
        if (node->required_for_boot)
            kprint("[FAIL] %s unavailable: %s\n", node->status.name,
                   reason ? reason : "not present");
        return false;
    case INIT_RESULT_DEPENDENCY:
        node->status.state = INIT_SKIPPED;
        if (node->required_for_boot)
            kprint("[FAIL] %s skipped: %s\n", node->status.name,
                   reason ? reason : "dependency");
        return false;
    default:
        node->status.state = INIT_FAILED;
        kprint("[FAIL] %s: %s\n", node->status.name,
               reason ? reason : "initialization error");
        return false;
    }
}

bool kernel_bringup(void)
{
    bool required_ok = true;

    init_graph_error = false;

    for (init_id_t id = 0; id < INIT_COUNT; id++) {
        bool ready = init_start_node(id);
        if (!ready && descriptors[id].required_for_boot)
            required_ok = false;
    }

    init_print_production_summary();

    if (!init_persist_boot_log())
        kprint("[WARN] Kernel boot log could not be saved\n");

    return required_ok && !init_graph_error;
}

init_state_t kernel_init_state(init_id_t id)
{
    return init_id_valid(id) ? descriptors[id].status.state : INIT_FAILED;
}

const init_status_t *kernel_init_status(init_id_t id)
{
    return init_id_valid(id) ? &descriptors[id].status : NULL;
}
