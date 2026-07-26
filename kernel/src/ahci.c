#include <ahci.h>
#include <ahci_structs.h>
#include <pci.h>
#include <vmm.h>
#include <dma.h>
#include <block.h>
#include <stddef.h>
#include <string.h>

static bool present = false;
static u64 base = 0;
static volatile ahci_hba_registers_t *hba = NULL;
static ahci_device_t devices[AHCI_MAX_PORTS];
static bool ahci_identify_device(
    volatile ahci_port_registers_t *port,
    ahci_command_header_t *headers,
    ahci_command_table_t *table);

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

    dma_buffer_t command_list = dma_alloc(1024, 1024);
    dma_buffer_t received_fis = dma_alloc(256, 256);
    dma_buffer_t command_table = dma_alloc(sizeof(ahci_command_table_t), 128);

    if (!command_list.virt || !received_fis.virt || !command_table.virt)
    {
        // TODO: Free any successful allocations.
        return;
    }

    memset(command_list.virt, 0, 1024);
    memset(received_fis.virt, 0, 256);
    memset(command_table.virt, 0, sizeof(ahci_command_table_t));

    u64 command_list_phys = command_list.phys;
    u64 received_fis_phys = received_fis.phys;
    u64 command_table_phys = command_table.phys;

    port->clb = (u32)command_list_phys;
    port->clbu = (u32)(command_list_phys >> 32);

    port->fb = (u32)received_fis_phys;
    port->fbu = (u32)(received_fis_phys >> 32);

    ahci_command_header_t *headers =
        (ahci_command_header_t *)command_list.virt;

    headers[0].cfl = sizeof(fis_reg_h2d_t) / sizeof(u32);
    headers[0].prdtl = 1;

    headers[0].ctba = (u32)command_table_phys;
    headers[0].ctbau = (u32)(command_table_phys >> 32);

    port->is = (u32)-1;
    port->serr = (u32)-1;

    ahci_start_port(port);

    if (!ahci_identify_device(port, headers, (ahci_command_table_t *)command_table.virt)) {
        // TODO
        return;
    }

    ahci_device_t *ahci = &devices[port_number];
    ahci->port = port;

    block_device_t device;

    device.type = BLOCK_DEVICE_SATA;
    device.sector_size = 512;
    device.sector_count = 0; //TODO: Read from IDENTITY data

    device.read = NULL; // TODO: ahci_read
    device.write= NULL; // TODO: ahci_write

    device.driver_data = ahci;

    block_register(&device);
}

static bool ahci_identify_device(
    volatile ahci_port_registers_t *port,
    ahci_command_header_t *headers,
    ahci_command_table_t *table) {
    dma_buffer_t identify = dma_alloc(512, 2);

    if (!identify.virt) {
        return false;
    }

    memset(identify.virt, 0, 512);

    ahci_command_header_t *header = &headers[0];

    u64 identify_phys = identify.phys;
    ahci_prdt_entry_t *prdt = &table->prdt_entries[0];
    prdt->dba = (u32)identify_phys;
    prdt->dbau = (u32)(identify_phys >> 32);

    prdt->reserved0 = 0;
    prdt->dbc = 512 - 1;
    prdt->interrupt = 1;

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)table->command_fis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->command = 1;
    fis->command_register = 0xEC;

    header->cfl = sizeof(fis_reg_h2d_t) / sizeof(u32);
    header->prdtl = 1;
    header->prdbc = 0;
    header->write = 0;
    header->atapi = 0;
    header->prefetchable = 0;
    header->reset = 0;
    header->bist = 0;
    header->clear_busy = 0;

    while (port->tfd & (AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)) {}
    port->is = (u32)-1;

    port->ci = 1;

    u32 timeout = 1000000;

    while ((port->ci & 1) && timeout--) {}

    if (timeout == 0) {
        return false;
    }

    if (port->tfd & AHCI_PORT_TFD_BSY) {
        return false;
    }

    return true;
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
