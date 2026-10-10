/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <ahci.h>
#include <ahci_structs.h>
#include <pci.h>
#include <vmm.h>
#include <dma.h>
#include <block.h>
#include <stddef.h>
#include <string.h>
#include <timer.h>
#include <kprint.h>

#define AHCI_OPERATION_TIMEOUT_US 1000000ULL

static bool present = false;
static u64 base = 0;
static volatile ahci_hba_registers_t *hba = NULL;

static ahci_device_t devices[AHCI_MAX_PORTS];
static bool ata_supports_lba48(
    const ata_identify_data_t *id);

static u64 ata_lba48_sector_count(
    const ata_identify_data_t *id);

static u64 ata_lba28_sector_count(
    const ata_identify_data_t *id);

static void ata_copy_model(const ata_identify_data_t *id, char *out,
                           usize capacity);

static bool ahci_read(
    block_device_t *device,
    u64 lba,
    u32 sector_count,
    void *buffer);

static bool ahci_flush(block_device_t *device);

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

        if (!dev)
            continue;

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
            base,
            sizeof(ahci_hba_registers_t));
        if (!hba) {
            present = false;
            continue;
        }

        if (!pci_enable_memory_busmaster(dev)) {
            present = false;
            return;
        }

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

static bool ahci_wait_clear(volatile u32 *register_address, u32 mask)
{
    timer_monotonic_deadline_t deadline;

    if (!register_address ||
        !timer_monotonic_deadline_start(&deadline,
                                        AHCI_OPERATION_TIMEOUT_US)) {
        return false;
    }
    while (*register_address & mask) {
        if (timer_monotonic_deadline_expired(&deadline))
            return false;
        __asm__ volatile("pause");
    }
    return true;
}

static bool ahci_stop_port(volatile ahci_port_registers_t *port)
{
    /* AHCI 1.3.1 section 10.3: stop the command engine before stopping
     * FIS reception. A timeout never licenses reuse of DMA memory. */
    port->cmd &= ~AHCI_PORT_CMD_ST;
    if (!ahci_wait_clear(&port->cmd, AHCI_PORT_CMD_CR)) return false;
    port->cmd &= ~AHCI_PORT_CMD_FRE;
    return ahci_wait_clear(&port->cmd, AHCI_PORT_CMD_FR);
}

static bool ahci_start_port(volatile ahci_port_registers_t *port)
{
    if (!ahci_wait_clear(&port->cmd, AHCI_PORT_CMD_CR))
        return false;

    port->cmd |= AHCI_PORT_CMD_FRE;
    port->cmd |= AHCI_PORT_CMD_ST;
    return true;
}

static volatile ahci_port_registers_t *ahci_get_port(u8 port_number)
{
    if (!hba)
        return NULL;

    if (port_number >= AHCI_MAX_PORTS)
        return NULL;

    return &hba->ports[port_number];
}

/* Called only under the block I/O gate, or during serialized boot setup.
 * No retry or restart after failure: retain every DMA allocation and deny
 * future commands even when CR/FR cannot be cleared. */
static void ahci_isolate(ahci_device_t *device)
{
    device->isolated = true;
    device->quiesced = ahci_stop_port(device->port);
    kprint("[AHCI] port isolated; DMA retained; engines %s\n",
           device->quiesced ? "stopped" : "not confirmed stopped");
}

static bool ahci_ready(ahci_device_t *device)
{
    if (!device || !device->port || device->isolated) return false;
    if (device->port->ci || device->port->sact ||
        !ahci_wait_clear(&device->port->tfd,
                         AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)) {
        ahci_isolate(device);
        return false;
    }
    return true;
}

static bool ahci_execute(ahci_device_t *device, u8 command, u64 lba,
                          u16 count, usize bytes, bool write)
{
    volatile ahci_port_registers_t *port = device->port;
    ahci_command_header_t *header = device->command_list.virt;
    ahci_command_table_t *table = device->command_table.virt;
    fis_reg_h2d_t *fis;
    timer_monotonic_deadline_t deadline;

    if (bytes > 4096U || !ahci_ready(device) ||
        !timer_monotonic_deadline_start(&deadline,
                                        AHCI_OPERATION_TIMEOUT_US))
        return false;
    memset(table, 0, sizeof(*table));
    memset(header, 0, sizeof(*header));
    header->ctba = (u32)device->command_table.phys;
    header->ctbau = (u32)(device->command_table.phys >> 32);
    header->cfl = sizeof(fis_reg_h2d_t) / sizeof(u32);
    header->write = write;
    header->prdtl = bytes ? 1U : 0U;
    if (bytes) {
        table->prdt_entries[0].dba = (u32)device->transfer.phys;
        table->prdt_entries[0].dbau = (u32)(device->transfer.phys >> 32);
        table->prdt_entries[0].dbc = bytes - 1U;
    }
    fis = (fis_reg_h2d_t *)table->command_fis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->command = 1;
    fis->command_register = command;
    fis->lba0 = (u8)lba;
    fis->lba1 = (u8)(lba >> 8);
    fis->lba2 = (u8)(lba >> 16);
    fis->device = 1U << 6;
    if (device->lba48) {
        fis->lba3 = (u8)(lba >> 24);
        fis->lba4 = (u8)(lba >> 32);
        fis->lba5 = (u8)(lba >> 40);
    } else {
        fis->device |= (u8)((lba >> 24) & 0x0fU);
    }
    fis->count_low = (u8)count;
    fis->count_high = (u8)(count >> 8);
    port->is = (u32)-1;
    port->serr = (u32)-1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    port->ci = 1U;
    while (port->ci & 1U) {
        if ((port->is & AHCI_PORT_IS_ERROR) ||
            (port->tfd & (AHCI_PORT_TFD_ERR | AHCI_PORT_TFD_DF)) ||
            timer_monotonic_deadline_expired(&deadline)) {
            ahci_isolate(device);
            return false;
        }
        __asm__ volatile("pause");
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if ((port->is & AHCI_PORT_IS_ERROR) || port->serr ||
        (port->tfd & (AHCI_PORT_TFD_ERR | AHCI_PORT_TFD_DF |
                      AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)) ||
        header->prdbc != bytes) {
        ahci_isolate(device);
        return false;
    }
    return true;
}

void ahci_port_init(u8 port_number)
{
    volatile ahci_port_registers_t *port = ahci_get_port(port_number);
    ahci_device_t *ahci;
    ata_identify_data_t *id;
    block_device_t device;

    if (!port || !ahci_port_present(port_number)) return;
    ahci = &devices[port_number];
    /* Never republish or overwrite allocations belonging to a stopped or
     * failed instance: unconfirmed DMA may still reference them. */
    if (ahci->port) return;
    ahci->port = port;
    ahci->isolated = true;
    if (!ahci_stop_port(port)) return;
    ahci->quiesced = true;
    /* Page alignment also makes every DMA region physically contiguous:
     * each allocation's payload fits entirely within one mapped page. */
    ahci->command_list = dma_alloc(1024U, 4096U);
    ahci->received_fis = dma_alloc(256U, 4096U);
    ahci->command_table = dma_alloc(sizeof(ahci_command_table_t), 4096U);
    ahci->transfer = dma_alloc(4096U, 4096U);
    if (!ahci->command_list.virt || !ahci->received_fis.virt ||
        !ahci->command_table.virt || !ahci->transfer.virt ||
        (!(hba->cap & (1U << 31)) &&
         ((ahci->command_list.phys >> 32) ||
          (ahci->received_fis.phys >> 32) ||
          (ahci->command_table.phys >> 32) ||
          (ahci->transfer.phys >> 32)))) {
        /* Nothing has been published to hardware yet. */
        dma_free(ahci->command_list);
        dma_free(ahci->received_fis);
        dma_free(ahci->command_table);
        dma_free(ahci->transfer);
        ahci->command_list = ahci->received_fis = ahci->command_table =
            ahci->transfer = (dma_buffer_t){0};
        return;
    }
    memset(ahci->command_list.virt, 0, 1024U);
    memset(ahci->received_fis.virt, 0, 256U);
    port->clb = (u32)ahci->command_list.phys;
    port->clbu = (u32)(ahci->command_list.phys >> 32);
    port->fb = (u32)ahci->received_fis.phys;
    port->fbu = (u32)(ahci->received_fis.phys >> 32);
    if (!ahci_start_port(port)) return;
    ahci->quiesced = false;
    ahci->isolated = false;
    if (!ahci_execute(ahci, 0xecU, 0, 0, 512U, false)) return;
    id = ahci->transfer.virt;
    ahci->lba48 = ata_supports_lba48(id);
    ahci->flush_supported = (id->words[83] & (1U << 12)) != 0U;
    memset(&device, 0, sizeof(device));
    device.type = BLOCK_DEVICE_SATA;
    device.sector_size = 512;
    device.sector_count = ahci->lba48 ? ata_lba48_sector_count(id)
                                      : ata_lba28_sector_count(id);
    device.read = ahci_read;
    device.write = ahci_write;
    device.flush = ahci_flush;
    device.driver_data = ahci;
    strncpy(device.connection, "sata", sizeof(device.connection) - 1U);
    ata_copy_model(id, device.model, sizeof(device.model));
    (void)block_register(&device);
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

static void ata_copy_model(const ata_identify_data_t *id, char *out,
                           usize capacity)
{
    char raw[41];
    usize start = 0;
    usize end = sizeof(raw) - 1U;

    if (!out || capacity == 0U) return;
    out[0] = '\0';
    if (!id) return;
    for (usize index = 0; index < 20U; index++) {
        u16 word = id->words[27U + index];
        raw[index * 2U] = (char)(word >> 8);
        raw[index * 2U + 1U] = (char)(word & 0xffU);
    }
    raw[40] = '\0';
    while (start < end && raw[start] == ' ') start++;
    while (end > start && raw[end - 1U] == ' ') end--;
    if (end == start) return;
    for (usize index = start; index < end && index - start + 1U < capacity;
         index++) {
        unsigned char value = (unsigned char)raw[index];
        if (value < 0x20U || value > 0x7eU) return;
        out[index - start] = raw[index];
        out[index - start + 1U] = '\0';
    }
}

static bool ahci_transfer(block_device_t *device, u64 lba, u32 count,
                           void *buffer, bool write)
{
    ahci_device_t *ahci;
    u8 *bytes = buffer;
    if (!device || !buffer || !count || device->sector_size != 512U ||
        lba >= device->sector_count || count > device->sector_count - lba)
        return false;
    ahci = device->driver_data;
    if (!ahci || (!ahci->lba48 &&
        (lba >= (1ULL << 28) || count > (1ULL << 28) - lba)) ||
        (ahci->lba48 && (lba >= (1ULL << 48) ||
                         count > (1ULL << 48) - lba))) return false;
    while (count) {
        u16 chunk = count > 8U ? 8U : (u16)count;
        usize length = (usize)chunk * 512U;
        /* Check before touching the permanently owned DMA buffer. */
        if (!ahci_ready(ahci)) return false;
        if (write) memcpy(ahci->transfer.virt, bytes, length);
        if (!ahci_execute(ahci, write ? (ahci->lba48 ? 0x35U : 0xcaU)
                                      : (ahci->lba48 ? 0x25U : 0xc8U),
                           lba, chunk, length, write)) return false;
        if (!write) memcpy(bytes, ahci->transfer.virt, length);
        bytes += length;
        lba += chunk;
        count -= chunk;
    }
    return true;
}

static bool ahci_read(block_device_t *device, u64 lba, u32 count, void *buffer)
{
    return ahci_transfer(device, lba, count, buffer, false);
}

static bool ahci_write(block_device_t *device, u64 lba, u32 count,
                        const void *buffer)
{
    return ahci_transfer(device, lba, count, (void *)buffer, true);
}

static bool ahci_flush(block_device_t *device)
{
    ahci_device_t *ahci = device ? device->driver_data : NULL;
    /* Never invent successful flush completion for an unsupported command. */
    return ahci && ahci->flush_supported &&
           ahci_execute(ahci, 0xe7U, 0, 0, 0, false);
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
