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
    ahci_command_table_t *table,
    dma_buffer_t *identify);

static bool ata_supports_lba48(
    const ata_identify_data_t *id);

static u64 ata_lba48_sector_count(
    const ata_identify_data_t *id);

static u64 ata_lba28_sector_count(
    const ata_identify_data_t *id);

static bool ahci_read(
    block_device_t *device,
    u64 lba,
    u32 sector_count,
    void *buffer);

static bool ahci_write(
    block_device_t *device,
    u64 lba,
    u32 sector_count,
    const void *buffer);

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

    dma_buffer_t identify;

    if (!ahci_identify_device(port, headers, (ahci_command_table_t *)command_table.virt, &identify)) {
        // TODO
        return;
    }

    ata_identify_data_t *id = (ata_identify_data_t *)identify.virt;

    ahci_device_t *ahci = &devices[port_number];
    ahci->port = port;

    block_device_t device;
    memset(&device, 0, sizeof(device));

    device.type = BLOCK_DEVICE_SATA;
    device.sector_size = 512;

    if (ata_supports_lba48(id))
    {
        device.sector_count = ata_lba48_sector_count(id);
    }
    else
    {
        device.sector_count = ata_lba28_sector_count(id);
    }

    device.read = ahci_read;
    device.write= ahci_write;

    device.driver_data = ahci;

    block_register(&device);
}

static bool ahci_identify_device(
    volatile ahci_port_registers_t *port,
    ahci_command_header_t *headers,
    ahci_command_table_t *table,
    dma_buffer_t *identify) {

    *identify = dma_alloc(512, 2);

    if (!identify->virt) {
        return false;
    }

    memset(identify->virt, 0, 512);

    ahci_command_header_t *header = &headers[0];

    u64 identify_phys = identify->phys;
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

static bool ata_supports_lba48(const ata_identify_data_t *id)
{
    return (id->words[83] & (1 << 10)) != 0;
}

static u64 ata_lba48_sector_count(const ata_identify_data_t *id)
{
    return
        ((u64)id->words[100]) |
        ((u64)id->words[101] << 16) |
        ((u64)id->words[102] << 32) |
        ((u64)id->words[103] << 48);
}

static u64 ata_lba28_sector_count(const ata_identify_data_t *id)
{
    return
        ((u64)id->words[60]) |
        ((u64)id->words[61] << 16);
}

static bool ahci_read(
    block_device_t *device,
    u64 lba,
    u32 sector_count,
    void *buffer)
{
    if (!device || !buffer)
    {
        return false;
    }

    if (sector_count == 0)
    {
        return false;
    }

    ahci_device_t *ahci =
        (ahci_device_t *)device->driver_data;

    if (!ahci)
    {
        return false;
    }

    volatile ahci_port_registers_t *port = ahci->port;

    if (!port)
    {
        return false;
    }

    ahci_command_header_t *headers =
        (ahci_command_header_t *)(u64)(
            ((u64)port->clb) |
            ((u64)port->clbu << 32));

    ahci_command_header_t *header = &headers[0];

    ahci_command_table_t *table =
        (ahci_command_table_t *)(u64)(
            ((u64)header->ctba) |
            ((u64)header->ctbau << 32));

    memset(table, 0, sizeof(*table));

    ahci_prdt_entry_t *prdt = &table->prdt_entries[0];

    u64 buffer_phys = vmm_virtual_to_physical(buffer);

    if (buffer_phys == 0) {
        return false;
    }
    
    prdt->dba = (u32)buffer_phys;
    prdt->dbau = (u32)(buffer_phys >> 32);

    prdt->reserved0 = 0;
    prdt->dbc = (sector_count * device->sector_size) - 1;
    prdt->interrupt = 1;

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)table->command_fis;

    memset(fis, 0, sizeof(*fis));

    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->command = 1;
    fis->command_register = 0x25;

    fis->lba0 = (u8)(lba);
    fis->lba1 = (u8)(lba >> 8);
    fis->lba2 = (u8)(lba >> 16);
    
    fis->device = 1 << 6;
    
    fis->lba3 = (u8)(lba >> 24);
    fis->lba4 = (u8)(lba >> 32);
    fis->lba5 = (u8)(lba >> 40);
    
    fis->count_low = (u8)sector_count;
    fis->count_high = (u8)(sector_count >> 8);

    header->cfl = sizeof(fis_reg_h2d_t) / sizeof(u32);
    header->prdtl = 1;
    header->prdbc = 0;
    
    header->write = 0;
    header->atapi = 0;
    header->prefetchable = 0;
    header->reset = 0;
    header->bist = 0;
    header->clear_busy = 0;

    while (port->tfd & (AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)) {
    }
    
    port->is = (u32)-1;
    
    port->ci = 1;
    
    u32 timeout = 1000000;
    
    while ((port->ci & 1) && timeout--) {
    }
    
    if (timeout == 0) {
        return false;
    }
    
    if (port->tfd & AHCI_PORT_TFD_BSY) {
        return false;
    }
    
    return true;
}

static bool ahci_write(
    block_device_t *device,
    u64 lba,
    u32 sector_count,
    const void *buffer)
{
    if (!device || !buffer)
    {
        return false;
    }

    if (sector_count == 0)
    {
        return false;
    }

    ahci_device_t *ahci =
        (ahci_device_t *)device->driver_data;

    if (!ahci)
    {
        return false;
    }

    volatile ahci_port_registers_t *port = ahci->port;

    if (!port)
    {
        return false;
    }

    ahci_command_header_t *headers =
        (ahci_command_header_t *)(u64)(
            ((u64)port->clb) |
            ((u64)port->clbu << 32));

    ahci_command_header_t *header = &headers[0];

    ahci_command_table_t *table =
        (ahci_command_table_t *)(u64)(
            ((u64)header->ctba) |
            ((u64)header->ctbau << 32));

    memset(table, 0, sizeof(*table));

    ahci_prdt_entry_t *prdt = &table->prdt_entries[0];

    u64 buffer_phys = vmm_virtual_to_physical((void *)buffer);

    if (buffer_phys == 0) {
        return false;
    }
    
    prdt->dba = (u32)buffer_phys;
    prdt->dbau = (u32)(buffer_phys >> 32);

    prdt->reserved0 = 0;
    prdt->dbc = (sector_count * device->sector_size) - 1;
    prdt->interrupt = 1;

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)table->command_fis;

    memset(fis, 0, sizeof(*fis));

    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->command = 1;
    fis->command_register = 0x35;

    fis->lba0 = (u8)(lba);
    fis->lba1 = (u8)(lba >> 8);
    fis->lba2 = (u8)(lba >> 16);
    
    fis->device = 1 << 6;
    
    fis->lba3 = (u8)(lba >> 24);
    fis->lba4 = (u8)(lba >> 32);
    fis->lba5 = (u8)(lba >> 40);
    
    fis->count_low = (u8)sector_count;
    fis->count_high = (u8)(sector_count >> 8);

    header->cfl = sizeof(fis_reg_h2d_t) / sizeof(u32);
    header->prdtl = 1;
    header->prdbc = 0;
    
    header->write = 1;
    header->atapi = 0;
    header->prefetchable = 0;
    header->reset = 0;
    header->bist = 0;
    header->clear_busy = 0;

    while (port->tfd & (AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)) {
    }
    
    port->is = (u32)-1;
    
    port->ci = 1;
    
    u32 timeout = 1000000;
    
    while ((port->ci & 1) && timeout--) {
    }
    
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
