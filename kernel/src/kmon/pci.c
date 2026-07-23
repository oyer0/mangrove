#include <kmon/pci.h>
#include <pci.h>
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

        kprint("%x:%x.%x Vendor:%x Device:%x Class:%x Sub:%x\n",
            dev->bus,
            dev->device,
            dev->function,
            dev->vendor_id,
            dev->device_id,
            dev->class_code,
            dev->subclass);
    }
}
