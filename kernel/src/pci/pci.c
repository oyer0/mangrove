/* SPDX-License-Identifier: GPL-3.0-only */
#include <pci.h>
#include <io.h>
#include <kprint.h>
#include <stddef.h>
#include <string.h>
#include <vmm.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC
#define PCI_MAX_DEVICES    256

#define PCI_STATUS_CAP_LIST       (1U << 4)
#define PCI_CAP_PTR               0x34
#define PCI_CAP_ID_EXP            0x10
#define PCI_CAP_ID_MSIX           0x11
#define PCI_MSIX_TABLE_BIR_MASK   0x7U
#define PCI_MSIX_TABLE_OFFSET_MASK (~0x7U)
#define PCI_MSIX_TABLE_ENTRY_SIZE 16U
#define PCI_MSIX_VECTOR_MASK      (1U << 0)
#define PCI_MSIX_FUNCTION_MASK    (1U << 14)
#define PCI_MSIX_ENABLE           (1U << 15)
#define PCI_COMMAND_INTX_DISABLE  (1U << 10)
#define PCI_COMMAND_MEMORY        (1U << 1)
#define PCI_CLASS_NETWORK         0x02U
#define PCI_CLASS_BRIDGE          0x06U
#define PCI_BRIDGE_PCI_SUBCLASS   0x04U
#define PCI_EXP_SLT_CAP           0x14U
#define PCI_EXP_SLT_CTL           0x18U
#define PCI_EXP_SLT_STATUS        0x1AU
#define PCI_EXP_SLT_CAP_HPC       (1U << 6)
#define PCI_EXP_SLT_CTL_PDC_EN    (1U << 3)
#define PCI_EXP_SLT_CTL_HPIE      (1U << 5)
#define PCI_EXP_SLT_CTL_POWER_OFF (1U << 10)
#define PCI_EXP_SLT_STATUS_ABP    (1U << 0)
#define PCI_EXP_SLT_STATUS_CC     (1U << 4)
#define PCI_EXP_SLT_STATUS_PDC    (1U << 3)
#define PCI_EXP_SLT_STATUS_PDS    (1U << 6)
#define PCI_RUNTIME_MMIO_BASE     0x80000000ULL
#define PCI_RUNTIME_MMIO_LIMIT    0xF0000000ULL
#define PCI_RUNTIME_MMIO_GRANULE  0x00100000ULL
/* The runtime scanner does not probe BAR sizes by rewriting live hardware
 * configuration.  Reserve a conservative 16 MiB around an assigned BAR so
 * an unreported large BAR (notably the QEMU VGA BAR) cannot overlap a new
 * Ethernet mapping. */
#define PCI_RUNTIME_MMIO_RESERVE  0x01000000ULL
#define PCI_E1000_VENDOR_ID       0x8086U
#define PCI_E1000_DEVICE_82540EM  0x100EU
#define PCI_E1000_DEVICE_82545EM  0x100FU
#define PCI_RTL_VENDOR_ID         0x10ECU
#define PCI_RTL_DEVICE_8168       0x8168U
#define PCI_MSIX_MAX_CAPS         48U
#define PCI_MSI_ADDRESS_BASE      0xFEE00000U

static pci_device_t pci_devices[PCI_MAX_DEVICES];
static pci_device_t pci_scan_devices[PCI_MAX_DEVICES];
static u32 pci_device_count = 0;
static u64 next_pci_generation = 1;
static pci_device_t *scan_output;
static u32 scan_output_count;

static u32 pci_read32(u8 bus, u8 device, u8 function, u8 offset);
static u16 pci_read16(u8 bus, u8 device, u8 function, u8 offset);
static u8 pci_read8(u8 bus, u8 device, u8 function, u8 offset);
static void pci_write32(u8 bus, u8 device, u8 function, u8 offset, u32 value);
static void pci_write8(u8 bus, u8 device, u8 function, u8 offset, u8 value);
static void pci_write16(u8 bus, u8 device, u8 function, u8 offset, u16 value);

static void pci_scan_function(u8 bus, u8 device, u8 function);
static void pci_scan_device(u8 bus, u8 device);
static void pci_scan_bus(u8 bus);

static void pci_scan(void);

/* Runtime reconciliation follows the buses reachable through bridges that
 * already exist (or are discovered on a reachable bus).  The previous
 * Stage 19.11 implementation probed every one of the 256 possible PCI buses
 * every 500 ms.  Legacy CF8/CFC accesses are comparatively expensive under
 * virtualization, so that scan could keep the background lifecycle thread
 * runnable for hundreds of milliseconds at a time.  A hot-pluggable slot's
 * bridge exists before its child is inserted; scanning bus zero plus each
 * bridge's secondary bus is therefore sufficient for the supported Q35 NIC
 * lifecycle without turning an absent PCI address space into periodic work. */
static void pci_runtime_scan(void)
{
    bool reachable[256] = {0};
    bool scanned[256] = {0};
    bool progress;

    reachable[0] = true;
    for (u32 index = 0; index < pci_device_count; index++) {
        const pci_device_t *device = &pci_devices[index];
        u8 secondary;

        if (!device->present || device->class_code != PCI_CLASS_BRIDGE ||
            device->subclass != PCI_BRIDGE_PCI_SUBCLASS)
            continue;
        secondary = pci_read8(device->bus, device->device,
                               device->function, 0x19);
        if (secondary != 0 && secondary != 0xffU)
            reachable[secondary] = true;
    }

    do {
        progress = false;
        for (u16 bus = 0; bus < 256; bus++) {
            u32 first;

            if (!reachable[bus] || scanned[bus]) continue;
            scanned[bus] = true;
            progress = true;
            first = scan_output_count;
            pci_scan_bus((u8)bus);
            for (u32 index = first; index < scan_output_count; index++) {
                const pci_device_t *device = &pci_scan_devices[index];
                u8 secondary;

                if (device->class_code != PCI_CLASS_BRIDGE ||
                    device->subclass != PCI_BRIDGE_PCI_SUBCLASS)
                    continue;
                secondary = pci_read8(device->bus, device->device,
                                       device->function, 0x19);
                if (secondary != 0 && secondary != 0xffU)
                    reachable[secondary] = true;
            }
        }
    } while (progress);
}

static u8 pci_find_capability(const pci_device_t *device, u8 capability_id)
{
    u8 offset;

    if (!device ||
        !(pci_read16(device->bus, device->device, device->function, 0x06) &
          PCI_STATUS_CAP_LIST))
        return 0;
    offset = pci_read8(device->bus, device->device, device->function,
                       PCI_CAP_PTR);
    for (u32 count = 0; offset && count < 48U; count++) {
        u8 id;
        u8 next;

        if (offset < 0x40U || (offset & 3U)) return 0;
        id = pci_read8(device->bus, device->device, device->function,
                       offset);
        next = pci_read8(device->bus, device->device, device->function,
                         (u8)(offset + 1U));
        if (id == capability_id) return offset;
        offset = next;
    }
    return 0;
}

static u32 pci_read32(u8 bus, u8 device, u8 function, u8 offset)
{
    u32 address =
        (1U << 31) |
        ((u32)bus << 16) |
        ((u32)device << 11) |
        ((u32)function << 8) |
        (offset & 0xFC);

    outl(PCI_CONFIG_ADDRESS, address);

    return inl(PCI_CONFIG_DATA);
}

static u16 pci_read16(u8 bus, u8 device, u8 function, u8 offset)
{
    u32 value = pci_read32(bus, device, function, offset);

    return (value >> ((offset & 2) * 8)) & 0xFFFF;
}

static u8 pci_read8(u8 bus, u8 device, u8 function, u8 offset)
{
    u32 value = pci_read32(bus, device, function, offset);

    return (value >> ((offset & 3) * 8)) & 0xFF;
}

static void pci_write32(u8 bus, u8 device, u8 function, u8 offset, u32 value)
{
    u32 address =
        (1U << 31) |
        ((u32)bus << 16) |
        ((u32)device << 11) |
        ((u32)function << 8) |
        (offset & 0xFC);

    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

static void pci_write16(u8 bus, u8 device, u8 function, u8 offset, u16 value)
{
    u32 shift = (offset & 2U) * 8U;
    u32 current = pci_read32(bus, device, function, offset);
    current = (current & ~(0xFFFFU << shift)) | ((u32)value << shift);
    pci_write32(bus, device, function, offset, current);
}

static void pci_write8(u8 bus, u8 device, u8 function, u8 offset, u8 value)
{
    u32 shift = (offset & 3U) * 8U;
    u32 current = pci_read32(bus, device, function, offset);
    current = (current & ~(0xFFU << shift)) | ((u32)value << shift);
    pci_write32(bus, device, function, offset, current);
}

void pci_init(void)
{
    pci_scan();
}

void pci_scan(void)
{
    memset(pci_devices, 0, sizeof(pci_devices));
    scan_output = pci_devices;
    scan_output_count = 0;

    for (u16 bus = 0; bus < 256; bus++)
        pci_scan_bus((u8)bus);

    pci_device_count = scan_output_count;
    for (u32 index = 0; index < pci_device_count; index++) {
        pci_devices[index].present = true;
        pci_devices[index].generation = next_pci_generation++;
        if (next_pci_generation == 0) next_pci_generation = 1;
    }
}

static void pci_scan_bus(u8 bus)
{
    for (u8 device = 0; device < 32; device++)
        pci_scan_device(bus, device);
}

static void pci_scan_function(u8 bus, u8 device, u8 function)
{
    u16 vendor = pci_read16(bus, device, function, 0x00);

    if (vendor == 0xFFFF)
        return;

    if (scan_output_count >= PCI_MAX_DEVICES)
        return;

    pci_device_t *dev = &scan_output[scan_output_count++];

    memset(dev, 0, sizeof(*dev));

    dev->bus = bus;
    dev->device = device;
    dev->function = function;

    dev->vendor_id = vendor;
    dev->device_id = pci_read16(bus, device, function, 0x02);

    dev->revision = pci_read8(bus, device, function, 0x08);
    dev->prog_if = pci_read8(bus, device, function, 0x09);
    dev->subclass = pci_read8(bus, device, function, 0x0A);
    dev->class_code = pci_read8(bus, device, function, 0x0B);

    dev->header_type = pci_read8(bus, device, function, 0x0E);

}

static void pci_scan_device(u8 bus, u8 device)
{
    pci_scan_function(bus, device, 0);

    u16 vendor = pci_read16(bus, device, 0, 0x00);

    if (vendor == 0xFFFF)
        return;

    u8 header_type = pci_read8(bus, device, 0, 0x0E);

    if (!(header_type & 0x80))
        return;

    for (u8 function = 1; function < 8; function++)
        pci_scan_function(bus, device, function);
}

u32 pci_get_device_count(void)
{
    return pci_device_count;
}

const pci_device_t *pci_get_device(u32 index)
{
    if (index >= pci_device_count || !pci_devices[index].present)
        return NULL;

    return &pci_devices[index];
}

static bool pci_same_function(const pci_device_t *left,
                              const pci_device_t *right)
{
    return left && right && left->bus == right->bus &&
           left->device == right->device &&
           left->function == right->function;
}

static bool pci_same_identity(const pci_device_t *left,
                              const pci_device_t *right)
{
    return pci_same_function(left, right) &&
           left->vendor_id == right->vendor_id &&
           left->device_id == right->device_id &&
           left->revision == right->revision &&
           left->class_code == right->class_code &&
           left->subclass == right->subclass &&
           left->prog_if == right->prog_if &&
           left->header_type == right->header_type;
}

static bool pci_is_supported_ethernet(const pci_device_t *device)
{
    if (!device || device->class_code != PCI_CLASS_NETWORK)
        return false;
    return (device->vendor_id == PCI_E1000_VENDOR_ID &&
            (device->device_id == PCI_E1000_DEVICE_82540EM ||
             device->device_id == PCI_E1000_DEVICE_82545EM)) ||
           (device->vendor_id == PCI_RTL_VENDOR_ID &&
            device->device_id == PCI_RTL_DEVICE_8168);
}

static u64 pci_ethernet_bar_size(const pci_device_t *device, u8 *bar_index)
{
    if (!device || !bar_index) return 0;
    if (device->vendor_id == PCI_E1000_VENDOR_ID &&
        (device->device_id == PCI_E1000_DEVICE_82540EM ||
         device->device_id == PCI_E1000_DEVICE_82545EM)) {
        *bar_index = 0;
        return 0x20000U;
    }
    if (device->vendor_id == PCI_RTL_VENDOR_ID &&
        device->device_id == PCI_RTL_DEVICE_8168) {
        *bar_index = 2;
        return 0x1000U;
    }
    return 0;
}

static bool pci_ranges_overlap(u64 left, u64 left_size,
                               u64 right, u64 right_size)
{
    u64 left_end;
    u64 right_end;

    if (!left_size || !right_size ||
        left > ~(u64)0 - left_size || right > ~(u64)0 - right_size)
        return true;
    left_end = left + left_size;
    right_end = right + right_size;
    return left < right_end && right < left_end;
}

static bool pci_mmio_region_in_use(u64 address, u64 size)
{
    for (u32 index = 0; index < pci_device_count; index++) {
        const pci_device_t *device = &pci_devices[index];
        if (!device->present) continue;
        for (u8 bar_index = 0; bar_index < 6; bar_index++) {
            pci_bar_t bar = pci_get_bar(device, bar_index);
            if (!bar.address || bar.io) continue;
            if (pci_ranges_overlap(address, size, bar.address,
                                   PCI_RUNTIME_MMIO_RESERVE))
                return true;
        }
    }

    for (u32 index = 0; index < scan_output_count; index++) {
        const pci_device_t *device = &pci_scan_devices[index];
        for (u8 bar_index = 0; bar_index < 6; bar_index++) {
            pci_bar_t bar = pci_get_bar(device, bar_index);
            if (!bar.address || bar.io) continue;
            if (pci_ranges_overlap(address, size, bar.address,
                                   PCI_RUNTIME_MMIO_RESERVE))
                return true;
        }
    }
    return false;
}

static bool pci_find_mmio_region(u64 window_start, u64 window_end,
                                 u64 size, u64 *address)
{
    u64 start = window_start;

    if (!address || !size || window_start >= window_end)
        return false;
    if (start % PCI_RUNTIME_MMIO_GRANULE)
        start += PCI_RUNTIME_MMIO_GRANULE -
                 (start % PCI_RUNTIME_MMIO_GRANULE);
    for (; start < window_end && size <= window_end - start;
         start += PCI_RUNTIME_MMIO_GRANULE) {
        if (!pci_mmio_region_in_use(start, size)) {
            *address = start;
            return true;
        }
    }
    return false;
}

static const pci_device_t *pci_find_parent_bridge(const pci_device_t *child)
{
    if (!child) return NULL;
    for (u32 index = 0; index < scan_output_count; index++) {
        const pci_device_t *candidate = &pci_scan_devices[index];
        if (candidate->class_code != PCI_CLASS_BRIDGE ||
            candidate->subclass != PCI_BRIDGE_PCI_SUBCLASS ||
            pci_read8(candidate->bus, candidate->device,
                      candidate->function, 0x19) != child->bus)
            continue;
        return candidate;
    }
    return NULL;
}

static bool pci_bridge_memory_window(const pci_device_t *bridge,
                                     u64 *start, u64 *end)
{
    u16 base;
    u16 limit;

    if (!bridge || !start || !end) return false;
    base = pci_read16(bridge->bus, bridge->device, bridge->function, 0x20);
    limit = pci_read16(bridge->bus, bridge->device, bridge->function, 0x22);
    *start = (u64)(base & 0xFFF0U) << 16;
    *end = ((u64)(limit & 0xFFF0U) + 0x10U) << 16;
    return *start < *end;
}

static bool pci_prepare_bridge_window(const pci_device_t *bridge,
                                      u64 child_size, u64 *child_address)
{
    u64 window_start;
    u64 window_end;
    u64 address;
    u16 base;
    u16 limit;

    if (!bridge || !child_address) return false;
    if (pci_bridge_memory_window(bridge, &window_start, &window_end) &&
        pci_find_mmio_region(window_start, window_end, child_size, &address)) {
        *child_address = address;
        return true;
    }

    if (!pci_find_mmio_region(PCI_RUNTIME_MMIO_BASE,
                              PCI_RUNTIME_MMIO_LIMIT,
                              child_size, &address))
        return false;

    base = (u16)((address >> 16) & 0xFFF0U);
    limit = (u16)(((address + PCI_RUNTIME_MMIO_GRANULE - 1U) >> 16) &
                 0xFFF0U);
    pci_write_config32(bridge, 0x20, ((u32)limit << 16) | base);
    pci_write_config16(bridge, 0x04,
                       pci_read_config16(bridge, 0x04) |
                       PCI_COMMAND_MEMORY);
    *child_address = address;
    return true;
}

static bool pci_ensure_bridge_window(const pci_device_t *bridge,
                                     u64 child_address, u64 child_size)
{
    u64 window_start;
    u64 window_end;
    u16 base;
    u16 limit;

    if (!bridge || !child_size || child_address > ~(u64)0 - child_size)
        return false;
    if (pci_bridge_memory_window(bridge, &window_start, &window_end) &&
        child_address >= window_start &&
        child_address + child_size <= window_end)
        return true;

    base = (u16)((child_address >> 16) & 0xFFF0U);
    limit = (u16)(((child_address + PCI_RUNTIME_MMIO_GRANULE - 1U) >> 16) &
                 0xFFF0U);
    pci_write_config32(bridge, 0x20, ((u32)limit << 16) | base);
    pci_write_config16(bridge, 0x04,
                       pci_read_config16(bridge, 0x04) |
                       PCI_COMMAND_MEMORY);
    return true;
}

static void pci_assign_ethernet_irq(const pci_device_t *device)
{
    u8 irq;

    if (!device || device->vendor_id != PCI_E1000_VENDOR_ID)
        return;
    irq = pci_read_config8(device, 0x3C);
    if (irq && irq != 0xFFU) return;
    for (u32 index = 0; index < pci_device_count; index++) {
        const pci_device_t *existing = &pci_devices[index];
        if (existing->present && existing->vendor_id == PCI_E1000_VENDOR_ID) {
            irq = pci_read_config8(existing, 0x3C);
            if (irq && irq != 0xFFU) break;
        }
    }
    if (!irq || irq == 0xFFU) irq = 11;
    pci_write_config8(device, 0x3C, irq);
}

static void pci_prepare_runtime_resources(void)
{
    for (u32 index = 0; index < scan_output_count; index++) {
        pci_device_t *device = &pci_scan_devices[index];
        u8 bar_index;
        u64 bar_size;
        pci_bar_t bar;
        u64 address;
        const pci_device_t *bridge;

        if (!pci_is_supported_ethernet(device)) continue;
        bar_size = pci_ethernet_bar_size(device, &bar_index);
        bar = pci_get_bar(device, bar_index);
        bridge = pci_find_parent_bridge(device);
        if (bar.address) {
            if (bridge &&
                !pci_ensure_bridge_window(bridge, bar.address, bar_size))
                continue;
            pci_assign_ethernet_irq(device);
            continue;
        }
        if (bridge && !pci_prepare_bridge_window(bridge, bar_size, &address)) {
            continue;
        }
        if (!bridge && !pci_find_mmio_region(PCI_RUNTIME_MMIO_BASE,
                                             PCI_RUNTIME_MMIO_LIMIT,
                                             bar_size, &address)) {
            continue;
        }
        pci_write_config32(device, (u8)(0x10U + bar_index * 4U),
                           (u32)address);
        pci_write_config16(device, 0x04,
                           pci_read_config16(device, 0x04) |
                           PCI_COMMAND_MEMORY);
        pci_assign_ethernet_irq(device);
    }
}

bool pci_rescan(void)
{
    bool changed = false;
    bool matched[PCI_MAX_DEVICES] = {0};
    u32 discovered_count;

    scan_output = pci_scan_devices;
    scan_output_count = 0;
    pci_runtime_scan();
    discovered_count = scan_output_count;
    /* Firmware does not necessarily assign resources to a device added to a
     * hot-pluggable root port.  Give the two supported Ethernet drivers a
     * bounded MMIO window before their normal probe path sees the device. */
    pci_prepare_runtime_resources();

    /* First mark removed functions and refresh unchanged functions in place.
     * Never compact this array: boot consumers may retain a PCI entry while
     * the lifecycle worker is reconciling a different function. */
    for (u32 index = 0; index < pci_device_count; index++) {
        pci_device_t *current = &pci_devices[index];
        u32 found = discovered_count;

        if (!current->present) continue;
        for (u32 candidate = 0; candidate < discovered_count; candidate++) {
            if (pci_same_function(current, &pci_scan_devices[candidate])) {
                found = candidate;
                break;
            }
        }
        if (found == discovered_count) {
            current->present = false;
            changed = true;
            continue;
        }
        matched[found] = true;
        if (!pci_same_identity(current, &pci_scan_devices[found])) {
            *current = pci_scan_devices[found];
            current->present = true;
            current->generation = next_pci_generation++;
            if (next_pci_generation == 0) next_pci_generation = 1;
            changed = true;
        }
    }

    /* Append new functions into unused registry slots.  Reusing an old slot
     * is safe only after assigning a new generation, so a stale driver
     * instance cannot become attached to a replacement at the same BDF. */
    for (u32 candidate = 0; candidate < discovered_count; candidate++) {
        u32 slot = pci_device_count;
        if (matched[candidate]) continue;
        for (u32 index = 0; index < pci_device_count; index++) {
            if (!pci_devices[index].present) {
                slot = index;
                break;
            }
        }
        if (slot >= PCI_MAX_DEVICES) continue;
        pci_devices[slot] = pci_scan_devices[candidate];
        pci_devices[slot].present = true;
        pci_devices[slot].generation = next_pci_generation++;
        if (next_pci_generation == 0) next_pci_generation = 1;
        if (slot == pci_device_count) pci_device_count++;
        changed = true;
    }
    return changed;
}

void pci_process_hotplug(void)
{
    for (u32 index = 0; index < pci_device_count; index++) {
        const pci_device_t *bridge = &pci_devices[index];
        u8 capability;
        u32 slot_capabilities;
        u16 slot_status;
        u16 slot_control;
        bool removal_requested;

        if (!bridge->present || bridge->class_code != PCI_CLASS_BRIDGE ||
            bridge->subclass != PCI_BRIDGE_PCI_SUBCLASS)
            continue;
        capability = pci_find_capability(bridge, PCI_CAP_ID_EXP);
        if (!capability) continue;
        slot_capabilities = pci_read_config32(
            bridge, (u8)(capability + PCI_EXP_SLT_CAP));
        if (!(slot_capabilities & PCI_EXP_SLT_CAP_HPC)) continue;
        slot_control = pci_read_config16(
            bridge, (u8)(capability + PCI_EXP_SLT_CTL));
        /* Enable the native presence-change notification path.  The
         * controller also reconciles by bounded rescan, so a missed
         * interrupt cannot make the registry stale. */
        if ((slot_control & (PCI_EXP_SLT_CTL_PDC_EN |
                             PCI_EXP_SLT_CTL_HPIE)) !=
            (PCI_EXP_SLT_CTL_PDC_EN | PCI_EXP_SLT_CTL_HPIE)) {
            slot_control |= PCI_EXP_SLT_CTL_PDC_EN |
                            PCI_EXP_SLT_CTL_HPIE;
            pci_write_config16(bridge, (u8)(capability + PCI_EXP_SLT_CTL),
                               slot_control);
        }
        /* A native root port may start with its power controller off when
         * empty.  Keep the supported runtime slot powered so a later device
         * add can expose configuration space and receive resources. */
        if (slot_control & PCI_EXP_SLT_CTL_POWER_OFF) {
            slot_control &= (u16)~PCI_EXP_SLT_CTL_POWER_OFF;
            pci_write_config16(bridge, (u8)(capability + PCI_EXP_SLT_CTL),
                               slot_control);
        }
        slot_status = pci_read_config16(
            bridge, (u8)(capability + PCI_EXP_SLT_STATUS));
        /* Command-completed is an acknowledgement for a previous slot
         * control write, not a topology transition.  Some QEMU root ports
         * retain that W1C bit after an empty-slot power operation.  Do not
         * turn it into periodic reconciliation work: only presence and
         * attention changes describe a device lifecycle transition. */
        slot_status &= PCI_EXP_SLT_STATUS_ABP | PCI_EXP_SLT_STATUS_PDC;
        if (!slot_status) continue;

        /* The status register is write-one-to-clear.  Clear only the bounded
         * hotplug events handled here, preserving unrelated slot status. */
        pci_write_config16(bridge, (u8)(capability + PCI_EXP_SLT_STATUS),
                           slot_status);
        /* Presence-change with a populated slot is an insertion.  An
         * attention-button event without presence-change is QEMU's native
         * unplug request; acknowledge it by powering the slot off. */
        removal_requested =
            ((slot_status & PCI_EXP_SLT_STATUS_PDC) &&
             !(slot_status & PCI_EXP_SLT_STATUS_PDS)) ||
            ((slot_status & PCI_EXP_SLT_STATUS_ABP) &&
             (slot_status & PCI_EXP_SLT_STATUS_PDS) &&
             !(slot_status & PCI_EXP_SLT_STATUS_PDC));
        if (!removal_requested)
            continue;

        /* QEMU keeps a device_del pending until the guest services the slot
         * removal.  Powering the empty slot off completes that handshake;
         * the following rescan then observes no child function. */
        slot_control = pci_read_config16(
            bridge, (u8)(capability + PCI_EXP_SLT_CTL));
        pci_write_config16(bridge, (u8)(capability + PCI_EXP_SLT_CTL),
                           slot_control | PCI_EXP_SLT_CTL_POWER_OFF);
    }
}

pci_bar_t pci_get_bar(const pci_device_t *device, u8 bar)
{
    pci_bar_t result;
    
    result.address = 0;
    result.io = false;
    result.prefetchable = false;
    result.is_64bit = false;

    if (bar >= 6)
        return result;

    u32 value = pci_read32(
        device->bus,
        device->device,
        device->function,
        0x10 + (bar * 4));

    result.io = value & 0x1;

    if (result.io)
    {
        result.address = value & ~0x3;
        return result;
    }

    result.address = value & ~0xF;
    result.prefetchable = value & (1 << 3);
    u8 type = (value >> 1) & 0x3;
    result.is_64bit = (type == 0x2);

    if (result.is_64bit)
    {
        u32 high = pci_read32(
            device->bus,
            device->device,
            device->function,
            0x10 + ((bar + 1) * 4));
        
        result.address |= ((u64)high << 32);
    }

    return result;
}

u16 pci_read_config16(const pci_device_t *device, u8 offset)
{
    return pci_read16(
        device->bus,
        device->device,
        device->function,
        offset);
}

u32 pci_read_config32(const pci_device_t *device, u8 offset)
{
    if (!device) return 0xFFFFFFFFU;
    return pci_read32(device->bus, device->device, device->function, offset);
}

u8 pci_read_config8(const pci_device_t *device, u8 offset)
{
    if (!device) return 0xFF;
    return pci_read8(device->bus, device->device, device->function, offset);
}

void pci_write_config8(const pci_device_t *device, u8 offset, u8 value)
{
    if (!device) return;
    pci_write8(device->bus, device->device, device->function, offset, value);
}

void pci_write_config16(const pci_device_t *device, u8 offset, u16 value)
{
    if (!device) return;
    pci_write16(device->bus, device->device, device->function, offset, value);
}

void pci_write_config32(const pci_device_t *device, u8 offset, u32 value)
{
    if (!device) return;
    pci_write32(device->bus, device->device, device->function, offset, value);
}

bool pci_enable_memory_busmaster(const pci_device_t *device)
{
    const u16 command_bits = (1U << 1) | (1U << 2);
    const u16 intx_disable = 1U << 10;
    u16 command;

    if (!device) return false;
    command = pci_read_config16(device, 0x04);
    command = (command | command_bits) & ~intx_disable;
    pci_write_config16(device, 0x04, command);
    command = pci_read_config16(device, 0x04);
    return (command & command_bits) == command_bits &&
           (command & intx_disable) == 0;
}

bool pci_get_msix_info(const pci_device_t *device, pci_msix_info_t *info)
{
    if (!device || !info ||
        !(pci_read16(device->bus, device->device, device->function, 0x06) &
          PCI_STATUS_CAP_LIST)) {
        return false;
    }

    u8 cap = pci_read8(device->bus, device->device, device->function,
                       PCI_CAP_PTR) & 0xFCU;
    for (u32 count = 0; cap >= 0x40 && count < PCI_MSIX_MAX_CAPS; count++) {
        u8 id = pci_read8(device->bus, device->device, device->function, cap);
        u8 next = pci_read8(device->bus, device->device, device->function,
                            (u8)(cap + 1)) & 0xFCU;

        if (id == PCI_CAP_ID_MSIX) {
            u16 control = pci_read16(device->bus, device->device,
                                     device->function, (u8)(cap + 2));
            u32 table = pci_read32(device->bus, device->device,
                                   device->function, (u8)(cap + 4));
            u8 bir = (u8)(table & PCI_MSIX_TABLE_BIR_MASK);
            u32 table_offset = table & PCI_MSIX_TABLE_OFFSET_MASK;
            pci_bar_t bar = pci_get_bar(device, bir);
            if (!bar.address || bar.io ||
                bar.address > ~(u64)0 - table_offset)
                return false;

            info->bar_address = bar.address;
            info->table_address = bar.address + table_offset;
            info->table_offset = table_offset;
            info->table_size = (u16)((control & 0x7FFU) + 1U);
            info->capability_offset = cap;
            info->bir = bir;
            info->table_virt = NULL;
            return (info->table_address & 0x7U) == 0;
        }

        if (!next || next == cap)
            break;
        cap = next;
    }
    return false;
}

bool pci_map_msix_table(pci_msix_info_t *info)
{
    u64 size;

    if (!info || !info->table_address || !info->table_size) return false;
    if (info->table_virt) return true;
    size = (u64)info->table_size * PCI_MSIX_TABLE_ENTRY_SIZE;
    info->table_virt = vmm_map_mmio(info->table_address, size);
    return info->table_virt != NULL;
}

static volatile u32 *pci_msix_entry(const pci_msix_info_t *info, u16 entry)
{
    if (!info || !info->table_virt || entry >= info->table_size) return NULL;
    return (volatile u32 *)((u8 *)info->table_virt +
        (u64)entry * PCI_MSIX_TABLE_ENTRY_SIZE);
}

void pci_disable_msix(const pci_device_t *device,
                      const pci_msix_info_t *info, u16 entry)
{
    if (!device || !info || entry >= info->table_size)
        return;

    volatile u32 *msix = pci_msix_entry(info, entry);
    if (!msix) return;
    u8 control_offset = (u8)(info->capability_offset + 2);
    u16 control = pci_read16(device->bus, device->device,
                             device->function, control_offset);

    msix[3] = PCI_MSIX_VECTOR_MASK;
    __asm__ volatile("sfence" ::: "memory");
    (void)msix[3];
    control &= ~(PCI_MSIX_ENABLE | PCI_MSIX_FUNCTION_MASK);
    pci_write16(device->bus, device->device, device->function,
                control_offset, control);
}

bool pci_prepare_msix_vector(const pci_device_t *device,
                             const pci_msix_info_t *info,
                             u16 entry, u8 apic_id, u8 vector)
{
    if (!device || !info || entry >= info->table_size || vector < 0x20)
        return false;

    volatile u32 *msix = pci_msix_entry(info, entry);
    if (!msix) return false;
    u8 control_offset = (u8)(info->capability_offset + 2);
    u16 control = pci_read16(device->bus, device->device,
                             device->function, control_offset);

    /* Mask globally and per-vector while replacing firmware's table entry. */
    pci_write16(device->bus, device->device, device->function,
                control_offset, control | PCI_MSIX_FUNCTION_MASK);
    msix[3] = PCI_MSIX_VECTOR_MASK;
    __asm__ volatile("sfence" ::: "memory");

    msix[0] = PCI_MSI_ADDRESS_BASE | ((u32)apic_id << 12);
    msix[1] = 0;
    msix[2] = vector;
    __asm__ volatile("sfence" ::: "memory");
    (void)msix[2];

    /* Enable the capability while both masking levels are still asserted. */
    control |= PCI_MSIX_ENABLE | PCI_MSIX_FUNCTION_MASK;
    pci_write16(device->bus, device->device, device->function,
                control_offset, control);

    u16 verify = pci_read16(device->bus, device->device,
                            device->function, control_offset);
    bool prepared = (verify & PCI_MSIX_ENABLE) != 0 &&
                    (verify & PCI_MSIX_FUNCTION_MASK) != 0 &&
                    (msix[3] & PCI_MSIX_VECTOR_MASK) != 0 &&
                    msix[0] == (PCI_MSI_ADDRESS_BASE | ((u32)apic_id << 12)) &&
                    msix[1] == 0 && msix[2] == vector;
    if (!prepared) {
        pci_disable_msix(device, info, entry);
        return false;
    }

    return true;
}

bool pci_unmask_msix_vector(const pci_device_t *device,
                            const pci_msix_info_t *info, u16 entry)
{
    if (!device || !info || entry >= info->table_size)
        return false;

    volatile u32 *msix = pci_msix_entry(info, entry);
    if (!msix) return false;
    u8 control_offset = (u8)(info->capability_offset + 2);
    u16 control = pci_read16(device->bus, device->device,
                             device->function, control_offset);
    if (!(control & PCI_MSIX_ENABLE) ||
        !(control & PCI_MSIX_FUNCTION_MASK) ||
        !(msix[3] & PCI_MSIX_VECTOR_MASK)) {
        pci_disable_msix(device, info, entry);
        return false;
    }

    /* The function remains masked while entry 0 is exposed. */
    msix[3] = 0;
    __asm__ volatile("sfence" ::: "memory");
    (void)msix[3];

    control &= ~PCI_MSIX_FUNCTION_MASK;
    pci_write16(device->bus, device->device, device->function,
                control_offset, control);

    u16 verify = pci_read16(device->bus, device->device,
                            device->function, control_offset);
    bool enabled = (verify & PCI_MSIX_ENABLE) != 0 &&
                   (verify & PCI_MSIX_FUNCTION_MASK) == 0 &&
                   (msix[3] & PCI_MSIX_VECTOR_MASK) == 0;
    if (!enabled) {
        pci_disable_msix(device, info, entry);
        return false;
    }

    /* MSI-X and pin interrupts must not be active at the same time. */
    u16 command = pci_read16(device->bus, device->device,
                             device->function, 0x04);
    pci_write16(device->bus, device->device, device->function, 0x04,
                command | PCI_COMMAND_INTX_DISABLE);
    return true;
}
