#include <xhci_storage.h>
#include <xhci_ring.h>
#include <xhci_trb.h>
#include <heap.h>
#include <string.h>
#include <kprint.h>
#include <storage/gpt.h>

extern void *xhci_dma_alloc(usize size, uintptr_t *phys_out);
extern void xhci_dma_free(void *virt, usize size);
extern xhci_ring_t *xhci_get_ep_ring(xhci_controller_t *, u8, u8);
extern volatile u32 *xhci_get_doorbell_ptr(xhci_controller_t *, u8);
extern xhci_status_t xhci_wait_for_transfer_completion_for(xhci_controller_t *, u8, u8, xhci_trb_t *);

typedef struct __attribute__((packed)) {
    u32 signature;
    u32 tag;
    u32 data_length;
    u8 flags;
    u8 lun;
    u8 cb_length;
    u8 cb[16];
} usb_cbw_t;

typedef struct __attribute__((packed)) {
    u32 signature;
    u32 tag;
    u32 residue;
    u8 status;
} usb_csw_t;

static usb_mass_storage_device_t devices[4];
static u32 device_count;

u32 xhci_storage_device_count(void)
{
    return device_count;
}

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static bool bulk_transfer(usb_mass_storage_device_t *dev, u8 dci, void *buffer,
                          uintptr_t phys, u32 length, bool in)
{
    xhci_ring_t *ring;
    xhci_trb_t event;
    xhci_status_t status;
    u32 control;
    u64 flags;
    bool ok = false;
    if (!dev || !buffer || !phys || !length) return false;
    ring = xhci_get_ep_ring(dev->xhc, dev->slot_id, dci);
    if (!ring) return false;
    control = XHCI_TRB_CTRL_TYPE_SET(XHCI_TRB_TYPE_NORMAL) | XHCI_TRB_CTRL_IOC;
    if (in) control |= XHCI_TRB_CTRL_DIR_IN;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    status = xhci_ring_enqueue(ring, XHCI_TRB_PARAM1_PTR(phys),
                               XHCI_TRB_PARAM2_PTR(phys),
                               XHCI_TRB_STS_XFER_LEN_SET(length), control);
    if (status != XHCI_SUCCESS) goto out;
    volatile u32 *doorbell = xhci_get_doorbell_ptr(dev->xhc, dev->slot_id);
    if (!doorbell) goto out;
    *doorbell = dci;
    memset(&event, 0, sizeof(event));
    status = xhci_wait_for_transfer_completion_for(dev->xhc, dev->slot_id, dci, &event);
    if (status != XHCI_SUCCESS) goto out;
    if (xhci_ring_reclaim_transfer(ring, XHCI_TRB_PTR_GET(event.param1, event.param2)) != XHCI_SUCCESS) goto out;
    ok = true;
out:
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
    return ok;
}

static bool bot_command(usb_mass_storage_device_t *dev, const u8 *cdb, u8 cdb_len,
                        void *data, uintptr_t data_phys, u32 data_len, bool data_in)
{
    usb_cbw_t cbw;
    usb_csw_t csw;
    uintptr_t cbw_phys, csw_phys;
    void *cbw_dma = xhci_dma_alloc(sizeof(cbw), &cbw_phys);
    void *csw_dma = xhci_dma_alloc(sizeof(csw), &csw_phys);
    bool ok;
    if (!cbw_dma || !csw_dma) return false;
    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = 0x43425355U;
    cbw.tag = ++dev->tag;
    cbw.data_length = data_len;
    cbw.flags = data_in ? 0x80 : 0;
    cbw.cb_length = cdb_len;
    memcpy(cbw.cb, cdb, cdb_len);
    memcpy(cbw_dma, &cbw, sizeof(cbw));
    ok = bulk_transfer(dev, dev->bulk_out_dci, cbw_dma, cbw_phys, sizeof(cbw), false);
    if (ok && data_len) ok = bulk_transfer(dev, data_in ? dev->bulk_in_dci : dev->bulk_out_dci,
                                           data, data_phys, data_len, data_in);
    if (ok) ok = bulk_transfer(dev, dev->bulk_in_dci, csw_dma, csw_phys, sizeof(csw), true);
    if (ok) {
        memcpy(&csw, csw_dma, sizeof(csw));
        ok = csw.signature == 0x53425355U && csw.tag == cbw.tag && csw.status == 0;
    }
    xhci_dma_free(cbw_dma, sizeof(cbw));
    xhci_dma_free(csw_dma, sizeof(csw));
    return ok;
}

static bool storage_read(block_device_t *block, u64 lba, u32 count, void *buffer)
{
    usb_mass_storage_device_t *dev = (usb_mass_storage_device_t *)block->driver_data;
    u8 *out = (u8 *)buffer;
    uintptr_t phys;
    u8 *dma;
    if (!dev || !buffer || block->sector_size != 512 || lba >= dev->block_count ||
        count > dev->block_count - lba) return false;
    dma = (u8 *)xhci_dma_alloc(4096U, &phys);
    if (!dma) return false;
    while (count) {
        u16 chunk = count > 8U ? 8U : (u16)count;
        u8 cdb[10] = { 0x28, 0, (u8)(lba >> 24), (u8)(lba >> 16), (u8)(lba >> 8),
                       (u8)lba, 0, (u8)(chunk >> 8), (u8)chunk, 0 };
        if (!bot_command(dev, cdb, sizeof(cdb), dma, phys, (u32)chunk * 512U, true)) {
            xhci_dma_free(dma, 4096U);
            return false;
        }
        memcpy(out, dma, (usize)chunk * 512U);
        out += (usize)chunk * 512U;
        lba += chunk;
        count -= chunk;
    }
    xhci_dma_free(dma, 4096U);
    return true;
}

bool xhci_storage_init_device(xhci_controller_t *xhc, u8 slot_id,
                              u8 bulk_in_ep, u8 bulk_out_ep,
                              xhci_storage_probe_result_t *out_result)
{
    xhci_storage_probe_result_t local_result;
    usb_mass_storage_device_t *dev;
    uintptr_t inquiry_phys, capacity_phys;
    u8 *inquiry, *capacity;
    u8 inquiry_cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    u8 capacity_cdb[10] = { 0x25 };

    if (!out_result) out_result = &local_result;
    memset(out_result, 0, sizeof(*out_result));
    if (!xhc || device_count >= 4) return false;

    dev = &devices[device_count];
    memset(dev, 0, sizeof(*dev));
    dev->xhc = xhc;
    dev->slot_id = slot_id;
    dev->bulk_in_dci = ((bulk_in_ep & 0x0f) * 2) + 1;
    dev->bulk_out_dci = (bulk_out_ep & 0x0f) * 2;

    out_result->stage = XHCI_STORAGE_STAGE_DMA;
    inquiry = (u8 *)xhci_dma_alloc(36, &inquiry_phys);
    capacity = (u8 *)xhci_dma_alloc(8, &capacity_phys);
    if (!inquiry || !capacity) {
        if (inquiry) xhci_dma_free(inquiry, 36);
        if (capacity) xhci_dma_free(capacity, 8);
        return false;
    }

    out_result->stage = XHCI_STORAGE_STAGE_INQUIRY;
    if (!bot_command(dev, inquiry_cdb, 6, inquiry, inquiry_phys, 36, true)) {
        xhci_dma_free(inquiry, 36);
        xhci_dma_free(capacity, 8);
        return false;
    }

    out_result->stage = XHCI_STORAGE_STAGE_CAPACITY;
    if (!bot_command(dev, capacity_cdb, 10, capacity, capacity_phys, 8, true)) {
        xhci_dma_free(inquiry, 36);
        xhci_dma_free(capacity, 8);
        return false;
    }

    dev->block_size = be32(capacity + 4);
    dev->block_count = (u64)be32(capacity) + 1ULL;
    xhci_dma_free(inquiry, 36);
    xhci_dma_free(capacity, 8);

    out_result->stage = XHCI_STORAGE_STAGE_GEOMETRY;
    if (dev->block_size != 512 || !dev->block_count) return false;
    dev->block.type = BLOCK_DEVICE_USB;
    dev->block.sector_size = dev->block_size;
    dev->block.sector_count = dev->block_count;
    dev->block.read = storage_read;
    dev->block.write = NULL;
    dev->block.driver_data = dev;

    out_result->stage = XHCI_STORAGE_STAGE_BLOCK_REGISTER;
    if (!block_register(&dev->block)) return false;
    device_count++;
    out_result->block_registered = true;
    kprint("[OK] USB block device registered (%llu sectors)\n", dev->block_count);

    out_result->gpt_scan_ran = true;
    out_result->gpt_found = gpt_scan_device(&dev->block);
    out_result->stage = XHCI_STORAGE_STAGE_READY;
    return true;
}
