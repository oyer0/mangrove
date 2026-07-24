#include <kmon/pci.h>
#include <pci.h>
#include <pci_class.h>
#include <pci_vendor.h>
#include <kprint.h>

void kmon_pci(void)
{
    u32 count = pci_get_device_count();

    if (count == 0)
    {
        kprint("No PCI devices found.\n");
        return;
    }

    kprint("Found %u PCI devices:\n", count);

    for (u32 i = 0; i < count; i++)
    {
        const pci_device_t *dev = pci_get_device(i);

        kprint("%x:%x.%x %s %s\n",
            dev->bus,
            dev->device,
            dev->function,
            pci_vendor_name(dev->vendor_id),
            pci_device_name(dev->vendor_id, dev->device_id),
            pci_class_name(
                dev->class_code,
                dev->subclass,
                dev->prog_if));
    }
}
