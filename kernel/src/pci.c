#include <pci.h>
#include <io.h>
#include <stddef.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC
#define PCI_MAX_DEVICES    256

static pci_device_t pci_devices[PCI_MAX_DEVICES];
static u32 pci_device_count = 0;

static u32 pci_read32(u8 bus, u8 device, u8 function, u8 offset);
static u16 pci_read16(u8 bus, u8 device, u8 function, u8 offset);
static u8 pci_read8(u8 bus, u8 device, u8 function, u8 offset);

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
