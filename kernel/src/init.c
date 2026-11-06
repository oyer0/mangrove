/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <init.h>
#include <csprng.h>

#include <acpi.h>
#include <ahci.h>
#include <block.h>
#include <cpu.h>
#include <net/dhcp.h>
#include <net/e1000.h>
#include <net/rtl8168.h>
#include <net/ethernet.h>
#include <net/arp.h>
#include <storage/fat32.h>
#include <storage/exfat.h>
#include <storage/gpt.h>
#include <net/icmp.h>
#include <initramfs.h>
#include <heap.h>
#include <ioapic.h>
#include <irq.h>
#include <kprint.h>
#include <lapic.h>
#include <storage/mgfs.h>
#include <net/net.h>
#include <net/config.h>
#include <net/user.h>
#include <pci.h>
#include <pmm.h>
#include <platform_power.h>
#include <process.h>
#include <scheduler.h>
#include <user_sync.h>
#include <kernel_stack.h>
#include <timer.h>
#include <net/udp.h>
#include <vfs.h>
#include <vmm.h>
#include <xhci.h>
#include <xhci_pci.h>
#include <xhci_storage.h>
#include <xhci_regs.h>
#include <net/dns.h>
#include <net/ipv4.h>
#include <net/tcp.h>
#include <net/lifecycle.h>
#include <memory_types.h>
#include <address_layout.h>
#include <mangrove_errors.h>
#include <identity.h>
#include <drivers/input/keyboard.h>
#include <drivers/input/ps2_keyboard.h>
#include <session.h>
#include <ipc.h>
#include <smp.h>
#include <string.h>
#include <x250_boot_trace.h>
#if X250_BOOT_TRACE
#include <boot_log_diagnostics.h>
#endif
#if defined(KERNEL_HYBRID_ALLOCATOR) || defined(SLAB_CORE_GUEST_TEST)
#include <slab.h>
#endif

#include <stddef.h>

#if defined(KERNEL_STACK_OVERFLOW_GUEST_TEST)
#include <panic.h>
#endif

#ifdef PITH_DEBUG_BOOT_TESTS
extern void kernel_debug_scheduler_tests(void);
extern void kernel_debug_runtime_tests(void);
#endif
#if defined(SCHEDULER_CONTENTION_GUEST_TEST)
extern bool kernel_scheduler_contention_guest_test(void);
#endif

static xhci_controller_t *g_xhc;
static volatile u32 g_xhci_irq_entries;

static void init_keyboard_led_sink(keyboard_lock_state_t state)
{
    if (g_xhc)
        xhci_keyboard_led_state_changed(g_xhc, state);
}

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
    user_sync_init();
    if (!identity_init() || !process_init() || !session_init() ||
        !ipc_init()) {
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
    if (!process_start_cleanup_worker()) {
        *reason = "process cleanup worker startup failed";
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
    if (!cpu_init_bsp()) {
        *reason = "BSP CPU-local state initialization failed";
        return INIT_RESULT_FAILED;
    }

    ioapic_init();
    if (!ioapic_present()) {
        *reason = "I/O APIC unavailable";
        return INIT_RESULT_FAILED;
    }

    u8 apic_id = lapic_current_id();
    if (!ioapic_route_gsi(acpi_irq_to_gsi(0), IRQ_VECTOR_PIT, apic_id,
                          acpi_irq_flags(0))) {
        *reason = "PIT I/O APIC route failed";
        return INIT_RESULT_FAILED;
    }

    /* Optional transport: a USB-only machine must still boot. Initialization
     * owns command replies while CPU interrupts are off; IRQ1 owns scan bytes
     * after the controller has enabled scanning and installed its handler. */
    const char *keyboard_reason;
    bool keyboard_ready = ps2_keyboard_init(&keyboard_reason);
    KERNEL_BOOT_DEBUG_LOG("[INPUT] PS/2 %s: %s\n",
                          keyboard_ready ? "ready" : "unavailable",
                          keyboard_reason);

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
    if (!net_lifecycle_init())
        KERNEL_BOOT_DEBUG_LOG("[NET] lifecycle worker unavailable\n");
    return INIT_RESULT_OK;
}

static init_result_t init_filesystems(const char **reason)
{
    if (initramfs_init() != VFS_OK ||
        fat32_init() != VFS_OK ||
        mgfs_init() != VFS_OK ||
        exfat_init() != VFS_OK) {
        *reason = "filesystem registration failed";
        return INIT_RESULT_FAILED;
    }
    return INIT_RESULT_OK;
}

static init_result_t init_cpu_irqs_enabled(const char **reason)
{
    (void)reason;
    heap_bootstrap_finish();
    __asm__ volatile("sti" ::: "memory");
    return INIT_RESULT_OK;
}

#if X250_BOOT_TRACE
static u16 xhci_pci_entry_command;
static xhci_pci_routing_t boot_usb_routing;
static bool boot_usb_routing_ok;

static void init_trace_usb_pci(void)
{
    if (boot_usb_routing.supported)
        x250_boot_trace_routing(boot_usb_routing_ok,
            boot_usb_routing.usb2_mask, boot_usb_routing.usb2_before,
            boot_usb_routing.usb2_after, boot_usb_routing.usb3_mask,
            boot_usb_routing.usb3_before, boot_usb_routing.usb3_after);
    x250_boot_trace_result(47U, true, "xHCI PCI command at entry",
                           false, true, xhci_pci_entry_command,
                           "OBSERVED", NULL);
    /* Read-only inventory: distinguish a companion controller owning
       USB2 ports from an xHCI reset/enumeration/MSC failure. */
    for (u32 i = 0; i < pci_get_device_count(); i++) {
        const pci_device_t *p = pci_get_device(i);
        if (!p || p->class_code != 0x0c || p->subclass != 0x03)
            continue;
        bool intel_xhc = p->vendor_id == 0x8086 && p->prog_if == 0x30;
        x250_boot_trace_pci(p->bus, p->device, p->function,
            p->vendor_id, p->device_id, p->prog_if,
            pci_read_config16(p, 4),
            intel_xhc ? pci_read_config32(p, 0xd0) : 0,
            intel_xhc ? pci_read_config32(p, 0xd4) : 0,
            intel_xhc ? pci_read_config32(p, 0xd8) : 0,
            intel_xhc ? pci_read_config32(p, 0xdc) : 0);
    }
}
#endif

/* Metadata-only predicate: do not probe/mount MGFS while USB setup owns the
   transfer stream. The original root callback performs all identity, duplicate
   role and filesystem checks after the discovery fence. */
static bool init_root_role_registered(void *context)
{
    (void)context;
    for (u32 i = 0; i < block_device_count(); i++) {
        block_device_t *device = block_get_device(i);
        gpt_partition_info_t info;
        if (device && gpt_get_partition_info(device, &info) &&
            strcmp(info.name, GPT_MANGROVE_ROOT_NAME) == 0)
            return true;
    }
    return false;
}

static bool init_block_registered(void *context)
{
    (void)context;
    return block_device_count() != 0U;
}

static init_result_t init_await_boot_discovery(bool root_required,
                                              const char **reason)
{
    if (!g_xhc || kernel_init_state(INIT_XHCI) != INIT_READY)
        return INIT_RESULT_OK;
#if X250_BOOT_TRACE
    u64 discovery_started_ms = timer_uptime_ms();
    x250_boot_trace_timing(discovery_started_ms,
        root_required ? "root discovery begin" : "block discovery begin",
        0, false, 0, NULL);
#endif
    x250_boot_trace_stage(47U, false, root_required ?
        "await native root discovery" : "await native block discovery");
    xhci_status_t status = xhci_wait_for_boot_discovery(
        g_xhc, root_required ? init_root_role_registered :
                              init_block_registered, NULL);
    xhci_trace_boot_discovery(g_xhc);
    x250_boot_trace_result(47U, status == XHCI_SUCCESS,
        root_required ? "native root discovery" : "native block discovery",
        true, true, (int)status,
        status == XHCI_SUCCESS ? "READY" :
            status == XHCI_ERR_TIMEOUT ? "DEADLINE" : "FAILED",
        status == XHCI_SUCCESS ? NULL : root_required ?
            "required root device not published" : "no block device published");
#if X250_BOOT_TRACE
    u64 discovery_finished_ms = timer_uptime_ms();
    x250_boot_trace_timing(discovery_finished_ms,
        root_required ? "root discovery end" : "block discovery end",
        0, true, discovery_finished_ms - discovery_started_ms,
        status == XHCI_SUCCESS ? "READY" :
            status == XHCI_ERR_TIMEOUT ? "DEADLINE" : "FAILED");
#endif
    if (status != XHCI_SUCCESS) {
        *reason = status == XHCI_ERR_TIMEOUT ?
            (root_required ?
                "Mangrove root volume not found (USB discovery deadline)" :
                "no persistent block device (USB discovery deadline)") :
            (root_required ? "native USB root discovery failed" :
                             "native USB storage discovery failed");
        return status == XHCI_ERR_TIMEOUT && !root_required ?
            INIT_RESULT_UNAVAILABLE : INIT_RESULT_FAILED;
    }
    return INIT_RESULT_OK;
}

static init_result_t init_storage(const char **reason)
{
    u32 block_devices = block_device_count();
    bool xhci_ready = kernel_init_state(INIT_XHCI) == INIT_READY;
    init_result_t discovery = INIT_RESULT_OK;

    /* Grant an empty registry a bounded publication opportunity. Any disk
       satisfies storage admission; root applies its own exact role check. */
    if (!block_devices && g_xhc && xhci_ready) {
        discovery = init_await_boot_discovery(false, reason);
        block_devices = block_device_count();
    }
    x250_boot_trace_storage_inventory(
        block_devices, xhci_storage_device_count(), xhci_ready);
#if X250_BOOT_TRACE
    init_trace_usb_pci();
#endif
    if (discovery != INIT_RESULT_OK)
        return discovery;

    /* Controller admission is distinct from root-role publication. Runtime
     * keyboard transfers need not be idle at this storage admission point. */
    if (g_xhc && !xhci_ready) {
        *reason = "xHCI boot discovery did not complete";
        return INIT_RESULT_UNAVAILABLE;
    }
    if (block_devices == 0) {
        *reason = "no persistent block device";
        return INIT_RESULT_UNAVAILABLE;
    }
    return INIT_RESULT_OK;
}

static init_result_t init_network_config(const char **reason)
{
    /* Runtime policy belongs to /core/networkd.  This phase only establishes
     * that the kernel network mechanisms are initialized; no configuration or
     * DHCP work is performed before the service starts. */
    (void)reason;
    return INIT_RESULT_OK;
}

static init_result_t init_xhci(const char **reason)
{
    const pci_device_t *xhci_pdev = NULL;
    phys_addr_t xhci_mmio_phys = 0;
    const usize xhci_mmio_size = XHCI_MMIO_BASE_APERTURE_SIZE;
    u8 xhci_irq = 11;
#if X250_BOOT_TRACE
    u64 xhci_started_ms = timer_uptime_ms();
    mg_monotonic_info_t monotonic_info;
    timer_monotonic_info(&monotonic_info);
    const char *clocksource =
        monotonic_info.source == MG_CLOCKSOURCE_HPET ? "HPET" :
        monotonic_info.source == MG_CLOCKSOURCE_ACPI_PM_TIMER ?
            "ACPI_PM_TIMER" : "PIT";
    x250_boot_trace_timing_value(
        xhci_started_ms, "monotonic clock", "period_fs",
        timer_monotonic_period_fs(), false, 0, clocksource);
    x250_boot_trace_timing(xhci_started_ms, "xHCI enter", 0, false, 0,
                           NULL);
#endif

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

#if X250_BOOT_TRACE
    xhci_pci_entry_command = pci_read_config16(xhci_pdev, 4);
#endif
    if (!pci_enable_memory_busmaster(xhci_pdev)) {
        *reason = "xHCI PCI memory/bus-master enable failed";
        return INIT_RESULT_FAILED;
    }

    uintptr_t xhci_mmio_base =
        (uintptr_t)vmm_map_mmio(xhci_mmio_phys, xhci_mmio_size);
    if (!xhci_mmio_base) {
        *reason = "xHCI MMIO mapping failed";
        return INIT_RESULT_FAILED;
    }

#if X250_BOOT_TRACE
    kprint("[X250] xHCI BAR=%016llx VA=%016llx mapped=%08x\n",
           (u64)xhci_mmio_phys, (u64)xhci_mmio_base,
           (u32)xhci_mmio_size);
#endif

    if (!irq_register_vector(IRQ_VECTOR_XHCI, init_xhci_irq_handler)) {
        *reason = "xHCI interrupt vector already owned";
        return INIT_RESULT_FAILED;
    }
    g_xhc = xhci_init(xhci_mmio_base, (u64)xhci_mmio_phys, xhci_irq);
    if (!g_xhc) {
        irq_unregister_vector(IRQ_VECTOR_XHCI);
        *reason = "xHCI controller initialization failed";
        return INIT_RESULT_FAILED;
    }
#if X250_BOOT_TRACE
    u64 controller_ready_ms = timer_uptime_ms();
    x250_boot_trace_timing(controller_ready_ms, "xHCI controller ready", 0,
                           true, controller_ready_ms - xhci_started_ms,
                           NULL);
#endif

    /* The generic legacy handshake claims the controller, not the chipset
       pin mux. Apply only a documented, firmware-authorized routing policy
       after that handoff/reset, before starting native port discovery. */
    xhci_pci_routing_t routing;
    bool routing_ok = xhci_pci_route_ports(xhci_pdev, &routing);
#if X250_BOOT_TRACE
    boot_usb_routing = routing;
    boot_usb_routing_ok = routing_ok;
#endif
    if (!routing_ok) {
        xhci_shutdown(g_xhc);
        irq_unregister_vector(IRQ_VECTOR_XHCI);
        g_xhc = NULL;
        *reason = "xHCI chipset port routing readback failed";
        return INIT_RESULT_FAILED;
    }
    xhci_trace_capture_ports(g_xhc, 1U);

    bool msix_prepared = false;
    pci_msix_info_t msix_info = {0};
    u8 apic_id = lapic_current_id();

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
#if X250_BOOT_TRACE
    u64 controller_started_ms = timer_uptime_ms();
    x250_boot_trace_timing(controller_started_ms, "xHCI controller started",
                           0, true, controller_started_ms - controller_ready_ms,
                           NULL);
#endif

    xhci_trace_capture_ports(g_xhc, 2U);
#if X250_BOOT_TRACE
    u64 port_scan_started_ms = timer_uptime_ms();
    x250_boot_trace_timing(port_scan_started_ms, "xHCI port scan begin", 0,
                           false, 0, NULL);
#endif
    xhci_probe_ports(g_xhc);
#if X250_BOOT_TRACE
    u64 port_scan_finished_ms = timer_uptime_ms();
    x250_boot_trace_timing(port_scan_finished_ms, "xHCI port scan end", 0,
                           true, port_scan_finished_ms - port_scan_started_ms,
                           NULL);
    u64 quiescence_started_ms = timer_uptime_ms();
    x250_boot_trace_timing(quiescence_started_ms,
                           "xHCI quiescence begin", 0, false, 0, NULL);
#endif
    if (!xhci_wait_for_boot_quiescence(g_xhc)) {
#if X250_BOOT_TRACE
        u64 quiescence_failed_ms = timer_uptime_ms();
        x250_boot_trace_timing(quiescence_failed_ms,
                               "xHCI quiescence end", 0, true,
                               quiescence_failed_ms - quiescence_started_ms,
                               "FAILED");
#endif
        *reason = "xHCI boot enumeration did not quiesce";
        return INIT_RESULT_FAILED;
    }
#if X250_BOOT_TRACE
    u64 quiescence_finished_ms = timer_uptime_ms();
    x250_boot_trace_timing(quiescence_finished_ms,
                           "xHCI quiescence end", 0, true,
                           quiescence_finished_ms - quiescence_started_ms,
                           "READY");
#endif
    xhci_trace_capture_ports(g_xhc, 3U);
    xhci_trace_boot_discovery(g_xhc);
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
    keyboard_register_led_sink(init_keyboard_led_sink);
    xhci_resume_keyboard(g_xhc);
#if X250_BOOT_TRACE
    u64 xhci_finished_ms = timer_uptime_ms();
    x250_boot_trace_timing(xhci_finished_ms, "xHCI ready", 0, true,
                           xhci_finished_ms - xhci_started_ms, NULL);
#endif
    return INIT_RESULT_OK;
}

#ifdef PITH_DEBUG_BOOT_TESTS
static void init_debug_vfs_tests(bool fat32_mounted)
{
    vfs_node_t *root_node = vfs_get_root_node();
    if (!root_node || !root_node->super)
        return;

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

static bool init_same_parent(const gpt_partition_info_t *left,
                             const gpt_partition_info_t *right)
{
    return left && right && left->parent && left->parent == right->parent;
}

static init_result_t init_rootfs(const char **reason)
{
    vfs_fs_type_t *mgfs = vfs_find_fs("mgfs");
    vfs_fs_type_t *fat32 = vfs_find_fs("fat32");
    block_device_t *root_device = NULL;
    block_device_t *boot_device = NULL;
    block_device_t *boot_partition = NULL;
    gpt_partition_info_t root_info = {0};
    gpt_partition_info_t boot_info = {0};
    vfs_node_t *root_node;
    vfs_node_t *boot_node;
#if X250_BOOT_TRACE
    u64 rootfs_started_ms = timer_uptime_ms();
    x250_boot_trace_timing(rootfs_started_ms, "root filesystem enter", 0,
                           false, 0, NULL);
#endif

    if (!mgfs || !mgfs->probe || !fat32 || !fat32->probe) {
        *reason = "filesystem drivers unavailable";
        return INIT_RESULT_FAILED;
    }
    init_result_t discovery = init_await_boot_discovery(true, reason);

#if X250_BOOT_TRACE
    {
        u32 block_count = block_device_count();
        u32 whole_disks = 0;
        u32 partition_devices = 0;
        u32 root_candidates = 0;

        for (u32 index = 0; index < block_count; index++) {
            block_device_t *device = block_get_device(index);
            gpt_partition_info_t partition;

            if (!device)
                continue;
            if (device->type == BLOCK_DEVICE_PARTITION) {
                if (gpt_get_partition_info(device, &partition)) {
                    partition_devices++;
                    if (strcmp(partition.name,
                               GPT_MANGROVE_ROOT_NAME) == 0)
                        root_candidates++;
                }
                continue;
            }

            whole_disks++;
            u32 registered_partitions = 0;
            u32 disk_root_candidates = 0;
            for (u32 child_index = 0; child_index < block_count;
                 child_index++) {
                block_device_t *child = block_get_device(child_index);
                gpt_partition_info_t child_info;

                if (child && child->type == BLOCK_DEVICE_PARTITION &&
                    gpt_get_partition_info(child, &child_info) &&
                    gpt_partition_parent_matches(&child_info, device)) {
                    registered_partitions++;
                    if (strcmp(child_info.name,
                               GPT_MANGROVE_ROOT_NAME) == 0)
                        disk_root_candidates++;
                }
            }

            gpt_table_info_t table = {0};
            int gpt_result = device->type == BLOCK_DEVICE_USB
                ? X250_TRACE_GPT_USB_SCAN_REPORTED
                : gpt_get_table_info(device, &table);
            x250_boot_trace_block_device(
                index, block_type_name(device->type), device->sector_size,
                device->sector_count, device->model, gpt_result,
                (u32)table.type, table.partition_count,
                registered_partitions, disk_root_candidates);
        }
        x250_boot_trace_root_inventory(block_count, whole_disks,
                                       partition_devices, root_candidates);
    }
#endif

    if (discovery != INIT_RESULT_OK)
        return discovery;

    /* The GPT partition name is a persistent installation role marker.  It
       is deliberately required so an unrelated MGFS volume cannot become /. */
    for (u32 i = 0; i < block_device_count(); i++) {
        block_device_t *device = block_get_device(i);
        gpt_partition_info_t info;
        if (!device || !gpt_get_partition_info(device, &info) ||
            strcmp(info.name, GPT_MANGROVE_ROOT_NAME) != 0) continue;
        if (root_device) {
            *reason = "multiple Mangrove root volumes";
            return INIT_RESULT_FAILED;
        }
#if X250_BOOT_TRACE
        u64 mgfs_probe_started_ms = timer_uptime_ms();
        x250_boot_trace_timing(mgfs_probe_started_ms, "MGFS probe begin", 0,
                               false, 0, NULL);
#endif
        bool mgfs_valid = mgfs->probe(device);
#if X250_BOOT_TRACE
        u64 mgfs_probe_finished_ms = timer_uptime_ms();
        x250_boot_trace_timing(mgfs_probe_finished_ms, "MGFS probe end", 0,
                               true,
                               mgfs_probe_finished_ms - mgfs_probe_started_ms,
                               mgfs_valid ? "VALID" : "INVALID");
#endif
        if (!mgfs_valid) {
            *reason = "Mangrove root volume is not MGFS";
            return INIT_RESULT_FAILED;
        }
        root_device = device;
        root_info = info;
    }
    if (!root_device) {
        *reason = "Mangrove root volume not found";
        return INIT_RESULT_FAILED;
    }
#if X250_BOOT_TRACE
    u64 mount_started_ms = timer_uptime_ms();
    x250_boot_trace_timing(mount_started_ms, "MGFS mount begin", 0,
                           false, 0, NULL);
#endif
    if (vfs_mount_root("mgfs", root_device) != VFS_OK) {
        *reason = "persistent root filesystem mount failed";
        return INIT_RESULT_FAILED;
    }
#if X250_BOOT_TRACE
    u64 mount_finished_ms = timer_uptime_ms();
    x250_boot_trace_timing(mount_finished_ms, "MGFS mount end", 0, true,
                           mount_finished_ms - mount_started_ms, "READY");
#endif

    root_node = vfs_get_root_node();
    if (!root_node) {
        *reason = "root filesystem did not publish a root node";
        return INIT_RESULT_FAILED;
    }
#if X250_BOOT_TRACE
    u64 rootfs_ready_ms = timer_uptime_ms();
    x250_boot_trace_timing(rootfs_ready_ms, "root filesystem ready", 0,
                           true, rootfs_ready_ms - rootfs_started_ms, NULL);
#endif

    /* Only the explicitly associated Mangrove ESP on the root disk may own
       /boot.  Other ESPs remain ordinary additional volumes. */
    for (u32 i = 0; i < block_device_count(); i++) {
        block_device_t *device = block_get_device(i);
        gpt_partition_info_t info;
        bool same_parent;
        bool is_esp;

        if (!device || !gpt_get_partition_info(device, &info)) continue;
        same_parent = init_same_parent(&info, &root_info);
        is_esp = gpt_partition_is_esp(&info);
        if (strcmp(info.name, GPT_MANGROVE_BOOT_NAME) == 0) {
            KERNEL_BOOT_DEBUG_LOG(
                "[VOLUME] MANGROVE_ESP candidate partition=%u lba=%llu-%llu "
                "same-root=%u esp=%u dev=%llu\n",
                info.number, info.first_lba, info.last_lba,
                same_parent ? 1U : 0U, is_esp ? 1U : 0U, device->id);
        }
        if (!same_parent || !is_esp ||
            strcmp(info.name, GPT_MANGROVE_BOOT_NAME) != 0) continue;
        if (boot_device) {
            KERNEL_BOOT_DEBUG_LOG("[VOLUME] multiple Mangrove ESPs; /boot left unmounted\n");
            boot_device = NULL;
            break;
        }
        boot_device = device;
        boot_partition = device;
        boot_info = info;
    }

    boot_node = vfs_finddir_trusted(root_node, "boot");
    if (boot_device) {
        KERNEL_BOOT_DEBUG_LOG(
            "[VOLUME] MANGROVE_ESP selected partition=%u lba=%llu-%llu dev=%llu\n",
            boot_info.number, boot_info.first_lba, boot_info.last_lba,
            boot_device->id);
    }
    if (boot_device && boot_node && boot_node->type == VFS_TYPE_DIRECTORY) {
        bool fat32_ok = fat32->probe(boot_device);
        KERNEL_BOOT_DEBUG_LOG("[VOLUME] /boot node=%llu fat32-probe=%u\n",
                              boot_node->inode, fat32_ok ? 1U : 0U);
        if (fat32_ok) {
            int mount_result = vfs_mount_path(boot_node, "efi", "/boot/efi",
                                               "fat32", boot_device,
                                               VFS_MOUNT_ROLE_BOOT, true);
            KERNEL_BOOT_DEBUG_LOG(
                "[VOLUME] role=boot fs=fat32 mount=/boot/efi result=%d\n",
                mount_result);
            if (mount_result == VFS_OK) {
                vfs_node_t *efi_node = NULL;
                int lookup_result = vfs_lookup("/boot/efi", &efi_node);
                KERNEL_BOOT_DEBUG_LOG(
                    "[VOLUME] /boot/efi registered lookup=%d inode=%llu\n",
                    lookup_result, efi_node ? efi_node->inode : 0ULL);
            } else {
                boot_device = NULL;
            }
        } else {
            boot_device = NULL;
        }
    } else {
        KERNEL_BOOT_DEBUG_LOG("[VOLUME] Mangrove ESP or MGFS /boot unavailable\n");
    }

    /* Root and its associated ESP are system mounts.  Every other volume is
     * intentionally left unmounted here: volumed reconstructs the current
     * block/VFS state after userspace starts and owns removable-media policy,
     * label-derived naming, suppression, and lifecycle recovery. */

    if (g_xhc)
        xhci_print_boot_summary(g_xhc, true);

#ifdef PITH_DEBUG_BOOT_TESTS
    init_debug_vfs_tests(false);
#endif

    return INIT_RESULT_OK;
}

static init_result_t init_accounts(const char **reason)
{
    if (!identity_registry_reload()) {
        *reason = identity_registry_error();
        return INIT_RESULT_FAILED;
    }
    return INIT_RESULT_OK;
}

#if defined(KERNEL_STACK_OVERFLOW_GUEST_TEST)
static __attribute__((noinline)) void stack_overflow_recurse(u32 depth)
{
    volatile u8 frame[2048];
    frame[0] = (u8)depth;
    frame[1023] = (u8)(depth >> 8);
    frame[2047] = (u8)(depth >> 16);
    stack_overflow_recurse(depth + 1U);
    __asm__ volatile("" : : "m"(frame) : "memory");
}

static void stack_overflow_thread(void *argument)
{
    (void)argument;
    stack_overflow_recurse(0U);
}
#endif

static init_result_t init_smp(const char **reason)
{
    if (!smp_start()) {
        *reason = "application processors unavailable";
        return INIT_RESULT_UNAVAILABLE;
    }
#if defined(SCHEDULER_CONTENTION_GUEST_TEST)
    if (!kernel_scheduler_contention_guest_test()) {
        *reason = "scheduler contention/fairness guest test failed";
        return INIT_RESULT_FAILED;
    }
#endif
    kernel_stack_usage_report();
#if defined(KERNEL_STACK_OVERFLOW_GUEST_TEST)
    kprint("[STACK-TEST] deliberate guarded thread-stack overflow\n");
    if (!thread_create("stack-overflow-test", stack_overflow_thread, NULL)) {
        *reason = "could not create guarded-stack overflow test thread";
        return INIT_RESULT_FAILED;
    }
    for (;;) {
        if (!scheduler_yield())
            panic("stack test: scheduler yield failed");
    }
#endif
#ifdef KERNEL_HYBRID_ALLOCATOR
    if (!slab_init((void *)KERNEL_VMM_RANGE_BASE,
                   (usize)(KERNEL_VMM_RANGE_LIMIT -
                           KERNEL_VMM_RANGE_BASE))) {
        kprint("[WARN] hybrid slab backend unavailable; indexed heap remains active\n");
#ifdef SLAB_CORE_GUEST_TEST
        *reason = "required hybrid slab backend initialization failed";
        return INIT_RESULT_FAILED;
#else
        /* Slab activation is optional: do not report SMP unavailable after
         * the CPUs have already started successfully. kmalloc remains on the
         * indexed heap while slab_cpu_alloc observes the uninitialized core. */
        return INIT_RESULT_OK;
#endif
    }
#endif
#ifdef SLAB_CORE_GUEST_TEST
    if (!slab_guest_test_run()) {
        *reason = "diagnostic slab execution failed";
        return INIT_RESULT_FAILED;
    }
#endif
#ifdef VMM_KERNEL_RANGE_GUEST_TEST
    if (!vmm_kernel_range_guest_test_run()) {
        *reason = "kernel virtual-range shootdown test failed";
        return INIT_RESULT_FAILED;
    }
#endif
    return INIT_RESULT_OK;
}

static init_result_t init_csprng(const char **reason)
{
    if (csprng_initialize() == MG_OK)
        return INIT_RESULT_OK;
    *reason = "no supported CPU random source is ready";
    return INIT_RESULT_UNAVAILABLE;
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
                          0, 0, true, init_irq_routing},
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
                                INIT_BIT(INIT_NETWORK_CORE) |
                                    INIT_BIT(INIT_PCI) |
                                    INIT_BIT(INIT_SCHEDULER),
                                INIT_BIT(INIT_NETWORK_DEVICE), false,
                                init_network_protocols},
    [INIT_FILESYSTEMS] = {{"filesystems", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                          INIT_BIT(INIT_VFS),
                          INIT_BIT(INIT_NETWORK_PROTOCOLS), true, init_filesystems},
    [INIT_CPU_IRQS_ENABLED] = {{"cpu IRQs", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                               INIT_BIT(INIT_FILESYSTEMS) |
                                   INIT_BIT(INIT_IRQ_ROUTING),
                               0, true,
                               init_cpu_irqs_enabled},
    [INIT_NETWORK_CONFIG] = {{"network configuration", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                             INIT_BIT(INIT_CPU_IRQS_ENABLED) |
                                 INIT_BIT(INIT_NETWORK_PROTOCOLS) |
                                 INIT_BIT(INIT_ROOTFS),
                             INIT_BIT(INIT_NETWORK_DEVICE), false,
                             init_network_config},
    [INIT_XHCI] = {{"xHCI", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                   INIT_BIT(INIT_PCI) | INIT_BIT(INIT_IRQ_ROUTING) |
                       INIT_BIT(INIT_CPU_IRQS_ENABLED) |
                       INIT_BIT(INIT_SCHEDULER),
                   0, false, init_xhci},
    [INIT_STORAGE] = {{"storage", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                      INIT_BIT(INIT_FILESYSTEMS),
                      INIT_BIT(INIT_AHCI) | INIT_BIT(INIT_XHCI), true,
                      init_storage},
    [INIT_ROOTFS] = {{"root filesystem", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                     INIT_BIT(INIT_VFS) | INIT_BIT(INIT_STORAGE),
                     0, true,
                     init_rootfs},
    [INIT_ACCOUNTS] = {{"accounts", INIT_UNINITIALIZED,
                        INIT_RESULT_OK, NULL},
                       INIT_BIT(INIT_STORAGE) | INIT_BIT(INIT_ROOTFS),
                       0, true, init_accounts},
    [INIT_CSPRNG] = {{"secure random generator", INIT_UNINITIALIZED,
                      INIT_RESULT_OK, NULL},
                     INIT_BIT(INIT_SCHEDULER), 0, false, init_csprng},
    [INIT_SMP] = {{"SMP", INIT_UNINITIALIZED, INIT_RESULT_OK, NULL},
                  INIT_BIT(INIT_IRQ_ROUTING) | INIT_BIT(INIT_ACCOUNTS),
                  INIT_BIT(INIT_CSPRNG), false, init_smp},
    [INIT_ACPI_EVENTS] = {{"ACPI events", INIT_UNINITIALIZED,
                           INIT_RESULT_OK, NULL},
                          INIT_BIT(INIT_IRQ_ROUTING) |
                              INIT_BIT(INIT_SCHEDULER) |
                              INIT_BIT(INIT_CPU_IRQS_ENABLED),
                          0, false, init_acpi_events}
};

static bool init_graph_error;

static void init_report_optional_network_status(void)
{
    const init_status_t *status = &descriptors[INIT_NETWORK_DEVICE].status;
    init_state_t state = status->state;

    if (state == INIT_UNAVAILABLE || state == INIT_FAILED ||
        state == INIT_SKIPPED)
        kprint("[WARN] Network unavailable\n");

    x250_boot_trace_result(40U, state == INIT_READY,
                           "network device", false, true,
                           status->result, state == INIT_READY ? "READY" :
                           state == INIT_UNAVAILABLE ? "UNAVAILABLE" :
                           state == INIT_FAILED ? "FAILED" : "SKIPPED",
                           status->reason);
}

#if X250_BOOT_TRACE
static void init_trace_boot_log_storage_end(
    const char *phase, x250_boot_trace_measurement_snapshot_t *measurements)
{
    xhci_storage_trace_snapshot_t io;
    x250_boot_trace_measurement_snapshot_t discarded_measurements;

    x250_boot_trace_measurements_end(measurements ? measurements :
                                                   &discarded_measurements);
    xhci_storage_trace_end(&io);
    x250_boot_trace_storage_io(timer_uptime_ms(), phase, &io);
}

#define INIT_BOOT_LOG_MGFS_BLOCK_BYTES 4096U

static bool init_prepare_boot_log_file(vfs_node_t *logs,
                                       vfs_node_t **out_file,
                                       u64 *out_previous_bytes,
                                       bool *out_previous_present)
{
    vfs_node_t *current = NULL;
    vfs_node_t *previous = NULL;
    int current_status;
    int previous_status;

    if (!logs || !out_file || !out_previous_bytes ||
        !out_previous_present)
        return false;
    *out_file = NULL;
    *out_previous_bytes = 0;
    *out_previous_present = false;

    current_status = vfs_lookup("/sys/logs/boot.log", &current);
    previous_status = vfs_lookup("/sys/logs/boot.previous.log", &previous);
    if ((current_status != VFS_OK && current_status != VFS_ERR_NOT_FOUND) ||
        (previous_status != VFS_OK &&
         previous_status != VFS_ERR_NOT_FOUND))
        return false;
    if (current_status == VFS_OK &&
        (!current || current->type != VFS_TYPE_FILE))
        return false;
    if (previous_status == VFS_OK &&
        (!previous || previous->type != VFS_TYPE_FILE))
        return false;

    if (current_status == VFS_OK) {
        *out_previous_bytes = current->size;
        *out_previous_present = true;
        if (previous_status == VFS_OK &&
            vfs_unlink(logs, "boot.previous.log") != VFS_OK)
            return false;
        if (vfs_rename(logs, "boot.log", logs,
                       "boot.previous.log") != VFS_OK)
            return false;
    }

    return vfs_create(logs, "boot.log", out_file) == VFS_OK &&
           *out_file && (*out_file)->type == VFS_TYPE_FILE;
}

static bool init_write_boot_log_diagnostic(vfs_node_t *boot_file,
                                           u64 snapshot_size,
                                           u64 preparation_ms,
                                           u64 save_started_ms,
        const x250_boot_trace_measurement_snapshot_t *preparation_measurements)
{
    const usize trace_header_bytes =
        sizeof(BOOT_LOG_DIAGNOSTIC_TRACE_HEADER) - 1U;
    const usize trace_capacity = X250_BOOT_TRACE_EXPORT_CAPACITY;
    const u64 footer_bytes = BOOT_LOG_DIAGNOSTIC_FOOTER_BYTES;
    usize allocation_bytes;
    char *buffer;
    usize snapshot_bytes;
    usize trace_bytes;
    u64 payload_bytes;
    u64 required_bytes;
    u64 file_bytes;
    u64 footer_offset;
    u64 write_started_ms;
    u64 write_finished_ms;
    u64 finalize_started_ms;
    u64 finalize_finished_ms;
    u32 trace_lines = 0;
    bool trace_complete = false;
    bool saved;
    xhci_storage_trace_snapshot_t io;
    boot_log_diagnostic_footer_t footer;
    u8 encoded_footer[BOOT_LOG_DIAGNOSTIC_FOOTER_BYTES];

    if (!boot_file || snapshot_size > (u64)-1 - trace_header_bytes ||
        snapshot_size + trace_header_bytes > (u64)-1 - trace_capacity ||
        snapshot_size + trace_header_bytes + trace_capacity >
            (u64)-1 - footer_bytes -
                     (INIT_BOOT_LOG_MGFS_BLOCK_BYTES - 1U))
        return false;

    allocation_bytes = (usize)(snapshot_size + trace_header_bytes +
        trace_capacity + footer_bytes + INIT_BOOT_LOG_MGFS_BLOCK_BYTES - 1U);
    buffer = kmalloc(allocation_bytes);
    if (!buffer)
        return false;

    snapshot_bytes = kprint_boot_log_read(0, buffer, (usize)snapshot_size);
    if (snapshot_bytes != snapshot_size) {
        kfree(buffer);
        return false;
    }
    __builtin_memcpy(buffer + snapshot_bytes,
                     BOOT_LOG_DIAGNOSTIC_TRACE_HEADER,
                     trace_header_bytes);
    trace_bytes = x250_boot_trace_export(
        buffer + snapshot_bytes + trace_header_bytes, trace_capacity,
        &trace_lines, &trace_complete);
    payload_bytes = snapshot_bytes + trace_header_bytes + trace_bytes;
    if (payload_bytes > (u64)-1 - footer_bytes -
                              (INIT_BOOT_LOG_MGFS_BLOCK_BYTES - 1U)) {
        kfree(buffer);
        return false;
    }
    required_bytes = payload_bytes + footer_bytes;
    file_bytes = ((required_bytes + INIT_BOOT_LOG_MGFS_BLOCK_BYTES - 1U) /
                  INIT_BOOT_LOG_MGFS_BLOCK_BYTES) *
                 INIT_BOOT_LOG_MGFS_BLOCK_BYTES;
    footer_offset = file_bytes - footer_bytes;
    if (file_bytes > allocation_bytes) {
        kfree(buffer);
        return false;
    }
    for (u64 index = payload_bytes; index < file_bytes; index++)
        buffer[index] = 0;

    __builtin_memset(&footer, 0, sizeof(footer));
    footer.state = BOOT_LOG_DIAGNOSTIC_STATE_INCOMPLETE;
    footer.flags = trace_complete ? 0U :
                   BOOT_LOG_DIAGNOSTIC_FLAG_TRACE_INCOMPLETE;
    footer.trace_lines = trace_lines;
    footer.snapshot_bytes = snapshot_bytes;
    footer.trace_bytes = trace_bytes;
    footer.preparation_ms = preparation_ms;
    if (preparation_measurements)
        footer.preparation_measurements = *preparation_measurements;
    boot_log_diagnostic_footer_encode(encoded_footer, &footer,
                                      (const u8 *)buffer,
                                      (usize)footer_offset);
    __builtin_memcpy(buffer + footer_offset, encoded_footer, footer_bytes);

    xhci_storage_trace_begin();
    x250_boot_trace_measurements_begin();
    write_started_ms = timer_uptime_ms();
    if (vfs_write(boot_file, 0, file_bytes, buffer) != file_bytes) {
        x250_boot_trace_measurements_end(NULL);
        xhci_storage_trace_end(&io);
        x250_boot_trace_storage_io(timer_uptime_ms(),
                                   "boot log payload write failed", &io);
        kfree(buffer);
        return false;
    }
    write_finished_ms = timer_uptime_ms();
    x250_boot_trace_measurements_end(&footer.payload_measurements);
    xhci_storage_trace_end(&io);
    x250_boot_trace_storage_io(write_finished_ms, "boot log payload write",
                               &io);
    x250_boot_trace_timing_value(
        write_finished_ms, "boot log payload write complete", "bytes",
        snapshot_bytes + trace_bytes, true,
        write_finished_ms - write_started_ms, "WRITTEN");

    footer.state = BOOT_LOG_DIAGNOSTIC_STATE_COMPLETE;
    footer.payload_write_ms = write_finished_ms - write_started_ms;
    footer.elapsed_before_finalize_ms = timer_uptime_ms() - save_started_ms;
    footer.storage = io;
    boot_log_diagnostic_footer_encode(encoded_footer, &footer,
                                      (const u8 *)buffer,
                                      (usize)footer_offset);

    finalize_started_ms = timer_uptime_ms();
    saved = vfs_write(boot_file, footer_offset, footer_bytes,
                      encoded_footer) == footer_bytes;
    finalize_finished_ms = timer_uptime_ms();
    x250_boot_trace_timing(
        finalize_finished_ms, "boot log evidence finalize", 0, true,
        finalize_finished_ms - finalize_started_ms,
        saved ? "COMPLETE" : "INCOMPLETE");
    kfree(buffer);
    return saved;
}
#endif

static bool init_write_boot_log_snapshot(vfs_node_t *boot_file)
{
    usize size = kprint_boot_log_size();
    char *buffer = NULL;
    usize read = 0;
    bool saved = false;
#if X250_BOOT_TRACE
    xhci_storage_trace_begin();
    u64 write_started_ms = timer_uptime_ms();
#endif

    /* A single MGFS write is one mutation and one confirmed device flush.
     * Split writes multiply the same persistence boundary and made removable
     * USB media pay for it once per 4 KiB chunk. Keep the bounded snapshot on
     * the heap rather than growing the boot stack. */
    if (size == 0U) {
        saved = true;
    } else {
        buffer = kmalloc(size);
        if (buffer) {
            read = kprint_boot_log_read(0, buffer, size);
            if (read == size && vfs_write(boot_file, 0, read, buffer) == read)
                saved = true;
            kfree(buffer);
        }
    }

#if X250_BOOT_TRACE
    u64 write_finished_ms = timer_uptime_ms();
    init_trace_boot_log_storage_end("boot log write", NULL);
    x250_boot_trace_timing_value(
        write_finished_ms, "boot log data write", "bytes", size, true,
        write_finished_ms - write_started_ms, saved ? "SAVED" : "FAILED");
#endif
    return saved;
}

static bool init_persist_boot_log(void)
{
    vfs_node_t *root = vfs_get_root_node();
    vfs_node_t *sys = NULL;
    vfs_node_t *logs = NULL;
#if !X250_BOOT_TRACE
    vfs_node_t *existing = NULL;
#endif
    vfs_node_t *boot_file = NULL;
#if X250_BOOT_TRACE
    u64 previous_log_bytes = 0;
    bool previous_log_present = false;
    bool created_sys = false;
    bool created_logs = false;
    x250_boot_trace_measurement_snapshot_t preparation_measurements;
    u64 file_setup_started_ms = timer_uptime_ms();
    u64 save_started_ms = file_setup_started_ms;
    xhci_storage_trace_begin();
    x250_boot_trace_measurements_begin();
#endif

    /* Stop capture before touching VFS so the snapshot cannot change while
     * it is being copied into the new file.  This runs in normal kernel
     * context, after root filesystem selection, never from an IRQ path. */
    kprint_boot_log_stop();

    if (!root || root->type != VFS_TYPE_DIRECTORY) {
#if X250_BOOT_TRACE
        init_trace_boot_log_storage_end("boot log prepare failed", NULL);
#endif
        return false;
    }

    if (vfs_lookup("/sys", &sys) != VFS_OK) {
        if (vfs_mkdir(root, "sys", &sys) != VFS_OK) {
#if X250_BOOT_TRACE
            init_trace_boot_log_storage_end("boot log prepare failed", NULL);
#endif
            return false;
        }
#if X250_BOOT_TRACE
        created_sys = true;
#endif
    }
    if (!sys || sys->type != VFS_TYPE_DIRECTORY) {
#if X250_BOOT_TRACE
        init_trace_boot_log_storage_end("boot log prepare failed", NULL);
#endif
        return false;
    }

    if (vfs_lookup("/sys/logs", &logs) != VFS_OK) {
        if (vfs_mkdir(sys, "logs", &logs) != VFS_OK) {
#if X250_BOOT_TRACE
            init_trace_boot_log_storage_end("boot log prepare failed", NULL);
#endif
            return false;
        }
#if X250_BOOT_TRACE
        created_logs = true;
#endif
    }
    if (!logs || logs->type != VFS_TYPE_DIRECTORY) {
#if X250_BOOT_TRACE
        init_trace_boot_log_storage_end("boot log prepare failed", NULL);
#endif
        return false;
    }

#if X250_BOOT_TRACE
    if (!init_prepare_boot_log_file(logs, &boot_file,
                                    &previous_log_bytes,
                                    &previous_log_present)) {
        init_trace_boot_log_storage_end("boot log prepare failed", NULL);
        return false;
    }
#else
    if (vfs_lookup("/sys/logs/boot.log", &existing) == VFS_OK) {
        if (!existing || existing->type != VFS_TYPE_FILE) {
            return false;
        }
        if (vfs_unlink(logs, "boot.log") != VFS_OK) {
            return false;
        }
    }

    if (vfs_create(logs, "boot.log", &boot_file) != VFS_OK ||
        !boot_file || boot_file->type != VFS_TYPE_FILE) {
        return false;
    }
#endif

#if X250_BOOT_TRACE
    u64 file_setup_finished_ms = timer_uptime_ms();
    init_trace_boot_log_storage_end("boot log preparation",
                                    &preparation_measurements);
    const char *path_state = previous_log_present ? "replaced" :
        created_sys && created_logs ? "new-sys+logs" :
        created_logs ? "new-logs" : "new-file";
    x250_boot_trace_timing_value(file_setup_finished_ms,
        "boot log prep path", "old_bytes", previous_log_bytes,
        false, 0, path_state);
    x250_boot_trace_timing(
        file_setup_finished_ms, "boot log file prepared", 0, true,
        file_setup_finished_ms - file_setup_started_ms, "READY");
#endif
#if X250_BOOT_TRACE
    return init_write_boot_log_diagnostic(
        boot_file, kprint_boot_log_size(),
        file_setup_finished_ms - file_setup_started_ms, save_started_ms,
        &preparation_measurements);
#else
    return init_write_boot_log_snapshot(boot_file);
#endif
}

static bool init_id_valid(init_id_t id)
{
    return id >= 0 && id < INIT_COUNT;
}

#if X250_BOOT_TRACE
static const char *init_state_name(init_state_t state)
{
    switch (state) {
    case INIT_UNINITIALIZED: return "UNINITIALIZED";
    case INIT_INITIALIZING: return "INITIALIZING";
    case INIT_READY: return "READY";
    case INIT_UNAVAILABLE: return "UNAVAILABLE";
    case INIT_FAILED: return "FAILED";
    case INIT_SKIPPED: return "SKIPPED";
    default: return "INVALID";
    }
}

static void init_trace_status(const init_descriptor_t *node)
{
    x250_boot_trace_result(37U, node->status.state == INIT_READY,
                           node->status.name, node->required_for_boot, true,
                           node->status.result,
                           init_state_name(node->status.state),
                           node->status.reason);
}
#else
#define init_trace_status(node) ((void)0)
#endif

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
        init_trace_status(node);
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
            init_trace_status(node);
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
            init_trace_status(node);
            return false;
        }
    }

    if (id == INIT_XHCI) {
        x250_boot_trace_begin();
        x250_boot_trace_gop_modes();
    }
    x250_boot_trace_stage(37, false, node->status.name);
    node->status.result = node->start(&reason);
    node->status.reason = reason;
    switch (node->status.result) {
    case INIT_RESULT_OK:
        node->status.state = INIT_READY;
        init_trace_status(node);
        return true;
    case INIT_RESULT_UNAVAILABLE:
        node->status.state = INIT_UNAVAILABLE;
        init_trace_status(node);
        if (node->required_for_boot)
            kprint("[FAIL] %s unavailable: %s\n", node->status.name,
                   reason ? reason : "not present");
        return false;
    case INIT_RESULT_DEPENDENCY:
        node->status.state = INIT_SKIPPED;
        init_trace_status(node);
        if (node->required_for_boot)
            kprint("[FAIL] %s skipped: %s\n", node->status.name,
                   reason ? reason : "dependency");
        return false;
    default:
        node->status.state = INIT_FAILED;
        init_trace_status(node);
        kprint("[FAIL] %s: %s\n", node->status.name,
               reason ? reason : "initialization error");
        return false;
    }
}

bool kernel_bringup(void)
{
    bool required_ok = true;
    bool bringup_ok;
    bool boot_log_saved;
    const char *failure_reason = NULL;

    init_graph_error = false;

    for (init_id_t id = 0; id < INIT_COUNT; id++) {
        bool ready = init_start_node(id);
        if (!ready && descriptors[id].required_for_boot)
            required_ok = false;
    }

    init_report_optional_network_status();

    bringup_ok = required_ok && !init_graph_error;
    if (!bringup_ok) {
        for (init_id_t id = 0; id < INIT_COUNT; id++) {
            const init_descriptor_t *node = &descriptors[id];
            if (node->required_for_boot && node->status.state != INIT_READY) {
                failure_reason = node->status.reason ? node->status.reason :
                    "required subsystem did not become ready";
                break;
            }
        }
        if (!failure_reason && init_graph_error)
            failure_reason = "initialization dependency graph cycle";
    }
    x250_boot_trace_result(21U, bringup_ok, "required kernel bring-up", true,
                           true, bringup_ok ? 0 : 1,
                           bringup_ok ? "READY" : "FAILED", failure_reason);

#if X250_BOOT_TRACE
    u64 boot_log_started_ms = timer_uptime_ms();
    x250_boot_trace_timing(boot_log_started_ms, "boot log save begin", 0,
                           false, 0, NULL);
#endif
    boot_log_saved = init_persist_boot_log();
#if X250_BOOT_TRACE
    u64 boot_log_finished_ms = timer_uptime_ms();
    x250_boot_trace_timing(boot_log_finished_ms, "boot log save end", 0,
                           true, boot_log_finished_ms - boot_log_started_ms,
                           boot_log_saved ? "SAVED" : "UNAVAILABLE");
#endif
    if (!boot_log_saved)
        kprint("[WARN] Kernel boot log could not be saved\n");

    x250_boot_trace_result(41U, boot_log_saved, "kernel boot log", false,
                           true, boot_log_saved ? 0 : 1,
                           boot_log_saved ? "SAVED" : "UNAVAILABLE",
                           boot_log_saved ? NULL :
                               "boot.log persistence failed");

    /* The existing first-failure presentation pause has already elapsed.
       Sample again without adding a delay or retrying enumeration/mounts. */
    if (!bringup_ok)
        xhci_trace_report_ports(g_xhc);

    return bringup_ok;
}

init_state_t kernel_init_state(init_id_t id)
{
    return init_id_valid(id) ? descriptors[id].status.state : INIT_FAILED;
}

const init_status_t *kernel_init_status(init_id_t id)
{
    return init_id_valid(id) ? &descriptors[id].status : NULL;
}
