#include <pci_class.h>

const char *pci_class_name(u8 class_code, u8 subclass, u8 prog_if)
{
    switch (class_code)
    {
        case 0x01: /* Mass Storage */
            switch (subclass)
            {
                case 0x00: return "SCSI Controller";
                case 0x01: return "IDE Controller";
                case 0x02: return "Floppy Controller";
                case 0x03: return "IPI Controller";
                case 0x04: return "RAID Controller";
                case 0x05: return "ATA Controller";

                case 0x06:
                    switch (prog_if)
                    {
                        case 0x01: return "SATA AHCI Controller";
                        default:   return "SATA Controller";
                    }

                case 0x07: return "Serial Attached SCSI Controller";
                case 0x08: return "Non-Volatile Memory Controller";
                case 0x80: return "Other Mass Storage Controller";
                default:   return "Unknown Mass Storage Controller";
            }

        case 0x02: /* Network */
            switch (subclass)
            {
                case 0x00: return "Ethernet Controller";
                case 0x80: return "Other Network Controller";
                default:   return "Network Controller";
            }

        case 0x03: /* Display */
            switch (subclass)
            {
                case 0x00: return "VGA Compatible Controller";
                case 0x01: return "XGA Controller";
                case 0x02: return "3D Controller";
                case 0x80: return "Other Display Controller";
                default:   return "Display Controller";
            }

        case 0x06: /* Bridge */
            switch (subclass)
            {
                case 0x00: return "Host Bridge";
                case 0x01: return "ISA Bridge";
                case 0x04: return "PCI-to-PCI Bridge";
                case 0x09: return "PCI Express Bridge";
                case 0x80: return "Other Bridge Device";
                default:   return "Bridge Device";
            }

        case 0x0C: /* Serial Bus */
            switch (subclass)
            {
                case 0x03:
                    switch (prog_if)
                    {
                        case 0x00: return "UHCI USB Controller";
                        case 0x10: return "OHCI USB Controller";
                        case 0x20: return "EHCI USB Controller";
                        case 0x30: return "xHCI USB Controller";
                        default:   return "USB Controller";
                    }

                case 0x05: return "SMBus Controller";
                default:   return "Serial Bus Controller";
            }

        default:
            return "Unknown Device";
    }
}

