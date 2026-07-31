#include <kmon/pci.h>
#include <pci/pci.h>
#include <kprint.h>

void kmon_pci(int argc, char **argv) {
    (void)argc; (void)argv;
    u32 count = pci_get_device_count();
    kprint("PCI Devices (%u):\n", count);

    for (u32 i = 0; i < count; i++) {
        const pci_device_t *dev = pci_get_device(i);
        if (dev) {
            kprint("  [%u] Bus %u Dev %u Func %u: Vendor 0x%04x Device 0x%04x (Class 0x%02x, Subclass 0x%02x)\n",
                   i, dev->bus, dev->device, dev->function,
                   dev->vendor_id, dev->device_id,
                   dev->class_code, dev->subclass);
        }
    }
}
