#include <pci_vendor.h>

const char *pci_vendor_name(u16 vendor_id)
{
    switch (vendor_id)
    {
        case 0x8086: return "Intel";
        case 0x1022: return "AMD";
        case 0x1002: return "AMD (ATI)";
        case 0x10DE: return "NVIDIA";
        case 0x1234: return "QEMU";
        case 0x1AF4: return "Red Hat / VirtIO";
        default:     return "Unknown Vendor";
    }
}

const char *pci_device_name(u16 vendor_id, u16 device_id)
{
    switch (vendor_id)
    {
        case 0x8086:
            switch (device_id)
            {
                case 0x29C0: return "ICH9 Host Bridge";
                case 0x2918: return "ICH9 ISA Bridge";
                case 0x2922: return "ICH9 SATA AHCI Controller";
                case 0x2930: return "ICH9 SMBus Controller";
                case 0x10D3: return "82574L Gigabit Ethernet Controller";
                default:     return "Unknown Intel Device";
            }

        case 0x1234:
            switch (device_id)
            {
                case 0x1111: return "Standard VGA";
                default:     return "Unknown QEMU Device";
            }

        default:
            return "Unknown Device";
    }
}
