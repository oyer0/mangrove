#include <pci.h>
#include <io.h>
#include <stddef.h>
#include <vmm.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC
#define PCI_MAX_DEVICES    256

#define PCI_STATUS_CAP_LIST       (1U << 4)
#define PCI_CAP_PTR               0x34
#define PCI_CAP_ID_MSIX           0x11
#define PCI_MSIX_TABLE_BIR_MASK   0x7U
#define PCI_MSIX_TABLE_OFFSET_MASK (~0x7U)
#define PCI_MSIX_TABLE_ENTRY_SIZE 16U
#define PCI_MSIX_VECTOR_MASK      (1U << 0)
#define PCI_MSIX_FUNCTION_MASK    (1U << 14)
#define PCI_MSIX_ENABLE           (1U << 15)
#define PCI_COMMAND_INTX_DISABLE  (1U << 10)
#define PCI_MSIX_MAX_CAPS         48U
#define PCI_MSI_ADDRESS_BASE      0xFEE00000U

static pci_device_t pci_devices[PCI_MAX_DEVICES];
static u32 pci_device_count = 0;

static u32 pci_read32(u8 bus, u8 device, u8 function, u8 offset);
static u16 pci_read16(u8 bus, u8 device, u8 function, u8 offset);
static u8 pci_read8(u8 bus, u8 device, u8 function, u8 offset);
static void pci_write32(u8 bus, u8 device, u8 function, u8 offset, u32 value);
static void pci_write16(u8 bus, u8 device, u8 function, u8 offset, u16 value);

static void pci_scan_function(u8 bus, u8 device, u8 function);
static void pci_scan_device(u8 bus, u8 device);
static void pci_scan_bus(u8 bus);

static void pci_scan(void);

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

void pci_init(void)
{
    pci_scan();
}

void pci_scan(void)
{
    pci_device_count = 0;

    for (u16 bus = 0; bus < 256; bus++)
        pci_scan_bus((u8)bus);
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

    if (pci_device_count >= PCI_MAX_DEVICES)
        return;

    pci_device_t *dev = &pci_devices[pci_device_count++];

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
    if (index >= pci_device_count)
        return NULL;

    return &pci_devices[index];
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

u8 pci_read_config8(const pci_device_t *device, u8 offset)
{
    if (!device) return 0xFF;
    return pci_read8(device->bus, device->device, device->function, offset);
}

void pci_write_config16(const pci_device_t *device, u8 offset, u16 value)
{
    if (!device) return;
    pci_write16(device->bus, device->device, device->function, offset, value);
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
