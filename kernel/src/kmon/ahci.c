#include <kmon/ahci.h>

#include <ahci.h>
#include <kprint.h>

void kmon_ahci(void)
{
    if (!ahci_present())
    {
        kprint("No AHCI controller found.\n");
        return;
    }

    u32 version = ahci_version();
    u16 major = (version >> 16) & 0xFFFF;
    u8 minor = (version >> 8) & 0xFF;
    u8 subminor = version & 0xFF;
    u32 pi = ahci_ports_implemented();
    u8 implemented = 0;
    for (u8 port = 0; port < AHCI_MAX_PORTS; port++) {
        if (pi & (1U << port))
            implemented++;}
    kprint("AHCI controller found.\n");
    kprint("MMIO Base: %p\n", ahci_base());
    kprint("Version: %u.%u.%u\n", major, minor, subminor);
    kprint("Capabilities: 0x%x\n", ahci_capabilities());
    kprint("Ports Implemented: 0x%x (%u)\n", pi, implemented);
    kprint("\nPorts:\n");
    
    for (u8 port = 0; port < 32; port++)
    {
        if (!ahci_port_implemented(port))
            continue;
    
        if (!ahci_port_implemented(port))
            continue;
        
        if (ahci_port_present(port))
            kprint("  Port %u: Device Present\n", port);
        else
            kprint("  Port %u: Empty\n", port);
    }
}
