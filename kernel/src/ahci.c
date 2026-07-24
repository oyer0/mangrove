#include <ahci.h>
#include <ahci_structs.h>
#include <pci.h>
#include <vmm.h>
#include <dma.h>
#include <stddef.h>
#include <string.h>

static bool present = false;
static u64 base = 0;
static volatile ahci_hba_registers_t *hba = NULL;

bool ahci_present(void) {
    return present;
}

u64 ahci_base(void) {
    return base;
}

void ahci_init(void) {
    present = false;
    base = 0;
    hba = NULL;

    u32 count = pci_get_device_count();

    for (u32 i = 0; i < count; i++)
    {
        const pci_device_t *dev = pci_get_device(i);

        if (dev->class_code != 0x01)
            continue;

        if (dev->subclass != 0x06)
            continue;

        if (dev->prog_if != 0x01)
            continue;

        pci_bar_t bar = pci_get_bar(dev, 5);

        if (bar.io)
            continue;

        present = true;
        base = bar.address;
        hba = (volatile ahci_hba_registers_t *)vmm_map_mmio(
            (void *)base,
            sizeof(ahci_hba_registers_t));

        // Enable AHCI mode
        hba->ghc |= AHCI_GHC_AE;

        for (u8 port = 0; port < AHCI_MAX_PORTS; port++)
        {
            if (!ahci_port_implemented(port))
                continue;
        
            if (!ahci_port_present(port))
                continue;
        
            ahci_port_init(port);
        }

        break;
    }
}

static void ahci_stop_port(volatile ahci_port_registers_t *port)
{
    port->cmd &= ~AHCI_PORT_CMD_ST;
    port->cmd &= ~AHCI_PORT_CMD_FRE;

    while (port->cmd & AHCI_PORT_CMD_CR)
    {
    }

    while (port->cmd & AHCI_PORT_CMD_FR)
    {
    }
}

static void ahci_start_port(volatile ahci_port_registers_t *port)
{
    while (port->cmd & AHCI_PORT_CMD_CR)
    {
    }

    port->cmd |= AHCI_PORT_CMD_FRE;
    port->cmd |= AHCI_PORT_CMD_ST;
}

static volatile ahci_port_registers_t *ahci_get_port(u8 port_number)
{
    if (!hba)
        return NULL;

    if (port_number >= AHCI_MAX_PORTS)
        return NULL;

    return &hba->ports[port_number];
}

void ahci_port_init(u8 port_number)
{
    volatile ahci_port_registers_t *port = ahci_get_port(port_number);

    if (!port)
    {
        return;
    }

    if (!ahci_port_present(port_number))
    {
        return;
    }

    ahci_stop_port(port);

    void *command_list = dma_alloc(1024, 1024);
    void *received_fis = dma_alloc(256, 256);
    void *command_table = dma_alloc(sizeof(ahci_command_table_t), 128);

    if (!command_list || !received_fis || !command_table)
    {
        // TODO: Free any successful allocations.
        return;
    }

    memset(command_list, 0, 1024);
    memset(received_fis, 0, 256);
    memset(command_table, 0, sizeof(ahci_command_table_t));

    u64 command_list_phys = dma_phys_addr(command_list);
    u64 received_fis_phys = dma_phys_addr(received_fis);

    port->clb = (u32)command_list_phys;
    port->clbu = (u32)(command_list_phys >> 32);

    port->fb = (u32)received_fis_phys;
    port->fbu = (u32)(received_fis_phys >> 32);

    ahci_command_header_t *headers =
        (ahci_command_header_t *)command_list;

    headers[0].cfl = sizeof(fis_reg_h2d_t) / sizeof(u32);
    headers[0].prdtl = 1;

    u64 command_table_phys = dma_phys_addr(command_table);

    headers[0].ctba = (u32)command_table_phys;
    headers[0].ctbau = (u32)(command_table_phys >> 32);

    port->is = (u32)-1;

    ahci_start_port(port);
}

bool ahci_port_present(u8 port_number)
{
    volatile ahci_port_registers_t *port = ahci_get_port(port_number);

    if (!port)
        return false;

    u32 ssts = port->ssts;

    u8 det = ssts & 0xF;
    u8 ipm = (ssts >> 8) & 0xF;

    return det == AHCI_SSTS_DET_PRESENT &&
        ipm == AHCI_SSTS_IPM_ACTIVE;
}

bool ahci_port_implemented(u8 port_number)
{
    if (!hba || port_number >= AHCI_MAX_PORTS)
        return false;

    return (hba->pi & (1U << port_number)) != 0;
}

u32 ahci_capabilities(void)
{
    if (!hba)
        return 0;

    return hba->cap;
}

u32 ahci_version(void)
{
    if (!hba)
        return 0;

    return hba->vs;
}

u32 ahci_ports_implemented(void)
{
    if (!hba)
        return 0;

    return hba->pi;
}
