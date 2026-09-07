#include <xhci_storage.h>
#include <xhci_ring.h>
#include <xhci_trb.h>
#include <heap.h>
#include <string.h>
#include <kprint.h>
#include <storage/gpt.h>
#include <vfs.h>
#include <scheduler.h>
#include <timer.h>

extern void *xhci_dma_alloc(usize size, uintptr_t *phys_out);
extern void xhci_dma_free(void *virt, usize size);
extern xhci_ring_t *xhci_get_ep_ring(xhci_controller_t *, u8, u8);
extern volatile u32 *xhci_get_doorbell_ptr(xhci_controller_t *, u8);
extern xhci_status_t xhci_wait_for_transfer_completion_for(xhci_controller_t *, u8, u8, xhci_trb_t *);
extern void xhci_cancel_transfer_wait(xhci_controller_t *);
extern void xhci_mark_event_work_pending(xhci_controller_t *);
extern bool xhci_start_deferred_worker(xhci_controller_t *);

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

static void storage_mark_block_gone(usb_mass_storage_device_t *device)
{
    if (!device || !device->block_registered) return;
    for (u32 index = 0; index < block_device_count(); index++) {
        block_device_t *registered = block_get_device(index);
        if (registered && registered->driver_data == device) {
            (void)block_device_mark_gone(registered->id);
            return;
        }
    }
}

u32 xhci_storage_device_count(void)
{
    return device_count;
}

bool xhci_storage_remove_device(xhci_controller_t *xhc, u8 slot_id)
{
    bool removed = false;
    bool was_counted;
    usb_mass_storage_device_t *device = NULL;

    /* USB BOT has one command stream and its callbacks may sleep.  A
       try-acquire is intentional here: waiting in the xHCI service worker can
       deadlock an I/O callback that is waiting for that worker to drain its
       completion event. */
    for (u32 index = 0; index < 4U; index++) {
        usb_mass_storage_device_t *candidate = &devices[index];
        if ((!candidate->active && !candidate->teardown_pending) ||
            candidate->xhc != xhc || candidate->slot_id != slot_id)
            continue;
        device = candidate;
        break;
    }
    if (!device) return false;

    /* Publish logical loss before trying to acquire the serialized BOT
       operation gate.  The gate may be held by an in-flight filesystem I/O;
       that operation must observe a dead backing identity immediately, even
       though the USB structures themselves cannot be freed until it unwinds. */
    storage_mark_block_gone(device);
    gpt_mark_partitions_gone_for_parent(&device->block);

    if (!block_io_begin_quiesce()) {
        __atomic_store_n(&device->active, false, __ATOMIC_RELEASE);
        device->teardown_pending = true;
        return false;
    }

    __atomic_store_n(&device->active, false, __ATOMIC_RELEASE);
    device->teardown_pending = false;
    {
        was_counted = device->block_registered;
        gpt_remove_partitions_for_parent(&device->block);
        if (device->block_registered)
            (void)block_unregister_by_driver_data(device);
        memset(device, 0, sizeof(*device));
        if (was_counted && device_count) device_count--;
        removed = true;
    }
    block_io_end_quiesce();
    return removed;
}

bool xhci_storage_device_teardown_pending(xhci_controller_t *xhc,
                                          u8 slot_id)
{
    for (u32 index = 0; index < 4U; index++) {
        if (devices[index].xhc == xhc && devices[index].slot_id == slot_id &&
            devices[index].teardown_pending)
            return true;
    }
    return false;
}

static void storage_teardown_wakeup(usb_mass_storage_device_t *dev)
{
    if (dev && dev->teardown_pending &&
        !__atomic_load_n(&dev->active, __ATOMIC_ACQUIRE)) {
        xhci_mark_event_work_pending(dev->xhc);
        (void)xhci_start_deferred_worker(dev->xhc);
    }
}

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static usize storage_copy_inquiry_field(char *out, usize capacity,
                                        const u8 *field, usize field_length)
{
    usize start = 0;
    usize end = field_length;

    if (!out || capacity == 0U || !field) return 0;
    while (start < end && (field[start] == ' ' || field[start] == '\0'))
        start++;
    while (end > start && (field[end - 1U] == ' ' || field[end - 1U] == '\0'))
        end--;
    if (end - start >= capacity) end = start + capacity - 1U;
    for (usize index = start; index < end; index++) {
        u8 value = field[index];
        if (value < 0x20U || value > 0x7eU) {
            out[0] = '\0';
            return 0;
        }
        out[index - start] = (char)value;
    }
    out[end - start] = '\0';
    return end - start;
}

static void storage_copy_inquiry_model(char *out, usize capacity,
                                       const u8 *inquiry)
{
    char vendor[9];
    char product[17];
    usize vendor_length;
    usize product_length;
    usize position = 0;

    if (!out || capacity == 0U) return;
    out[0] = '\0';
    if (!inquiry) return;
    vendor_length = storage_copy_inquiry_field(vendor, sizeof(vendor),
                                                inquiry + 8U, 8U);
    product_length = storage_copy_inquiry_field(product, sizeof(product),
                                                 inquiry + 16U, 16U);
    /* Some devices repeat their vendor at the beginning of the product
     * field (QEMU reports QEMU / QEMU HARDDISK).  Keep the canonical model
     * readable without duplicating that prefix. */
    if (vendor_length && product_length > vendor_length &&
        !memcmp(product, vendor, vendor_length) &&
        product[vendor_length] == ' ') vendor_length = 0;
    if (vendor_length) {
        usize copy = vendor_length < capacity - 1U ? vendor_length :
                     capacity - 1U;
        memcpy(out, vendor, copy);
        position = copy;
    }
    if (product_length && position < capacity - 1U) {
        if (position) out[position++] = ' ';
        if (product_length > capacity - 1U - position)
            product_length = capacity - 1U - position;
        memcpy(out + position, product, product_length);
        position += product_length;
    }
    out[position] = '\0';
}

static bool bulk_transfer(usb_mass_storage_device_t *dev, u8 dci, void *buffer,
                          uintptr_t phys, u32 length, bool in)
{
    xhci_ring_t *ring;
    xhci_trb_t event;
    xhci_status_t status;
    u32 control;
    u32 transfer_index;
    u32 ring_before_enqueue;
    u32 ring_before_cycle;
    u32 producer_cycle;
    u64 flags;
    bool ok = false;
    uintptr_t transfer_trb_phys;
    if (!dev || !buffer || !phys || !length) return false;
    ring = xhci_get_ep_ring(dev->xhc, dev->slot_id, dci);
    if (!ring) return false;
    transfer_index = ring->enqueue_idx;
    ring_before_enqueue = ring->enqueue_idx;
    ring_before_cycle = ring->cycle_state;
    control = XHCI_TRB_CTRL_TYPE_SET(XHCI_TRB_TYPE_NORMAL) | XHCI_TRB_CTRL_IOC;
    if (in) control |= XHCI_TRB_CTRL_DIR_IN;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    status = xhci_ring_enqueue_unpublished(
        ring, XHCI_TRB_PARAM1_PTR(phys), XHCI_TRB_PARAM2_PTR(phys),
        XHCI_TRB_STS_XFER_LEN_SET(length), control, &transfer_trb_phys,
        &producer_cycle);
    if (status != XHCI_SUCCESS) {
#ifdef NETWORK_BOOT_DIAG
        kprint("[USB-XFER] enqueue failed slot=%u dci=%u idx=%u status=%d\n",
               dev->slot_id, dci, transfer_index, status);
#endif
        goto out;
    }
    volatile u32 *doorbell = xhci_get_doorbell_ptr(dev->xhc, dev->slot_id);
    if (!doorbell) {
#ifdef NETWORK_BOOT_DIAG
        kprint("[USB-XFER] doorbell missing slot=%u dci=%u idx=%u\n",
               dev->slot_id, dci, transfer_index);
#endif
        xhci_ring_abort_unpublished(ring, ring_before_enqueue,
                                    ring_before_cycle);
        goto out;
    }
    if (!xhci_arm_transfer_wait(dev->xhc, dev->slot_id, dci,
                                transfer_trb_phys, transfer_trb_phys,
                                transfer_trb_phys)) {
#ifdef NETWORK_BOOT_DIAG
        kprint("[USB-XFER] arm failed slot=%u dci=%u idx=%u enq=%u deq=%u\n",
               dev->slot_id, dci, transfer_index, ring->enqueue_idx,
               ring->dequeue_idx);
#endif
        xhci_ring_abort_unpublished(ring, ring_before_enqueue,
                                    ring_before_cycle);
        goto out;
    }
    if (xhci_ring_publish_trb(ring, transfer_trb_phys, producer_cycle) !=
        XHCI_SUCCESS) {
        xhci_cancel_transfer_wait(dev->xhc);
        xhci_ring_abort_unpublished(ring, ring_before_enqueue,
                                    ring_before_cycle);
        goto out;
    }
    *doorbell = dci;
    /* The service owner must be able to receive the IRQ while this caller
       waits.  Keep interrupts disabled only across ring publication and
       doorbell ordering, never across the completion wait. */
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
    flags = 0;
    memset(&event, 0, sizeof(event));
    status = xhci_wait_for_transfer_completion_for(dev->xhc, dev->slot_id, dci, &event);
    if (status != XHCI_SUCCESS) {
#ifdef NETWORK_BOOT_DIAG
        kprint("[USB-XFER] failed slot=%u dci=%u idx=%u enq=%u deq=%u cycle=%u in=%u status=%d cc=%u\n",
               dev->slot_id, dci, transfer_index, ring->enqueue_idx,
               ring->dequeue_idx, ring->cycle_state, in ? 1U : 0U, status,
               XHCI_TRB_STS_COMP_CODE_GET(event.status));
#endif
        goto out;
    }
    if (xhci_ring_reclaim_transfer(ring, XHCI_TRB_PTR_GET(event.param1, event.param2)) != XHCI_SUCCESS) {
#ifdef NETWORK_BOOT_DIAG
        kprint("[USB-XFER] reclaim failed slot=%u dci=%u event=%p\n",
               dev->slot_id, dci,
               (void *)XHCI_TRB_PTR_GET(event.param1, event.param2));
#endif
        goto out;
    }
    ok = true;
out:
    if (flags)
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
#ifdef NETWORK_BOOT_DIAG
    if (!ok) kprint("[USB-BOT] CBW failed tag=%u\n", cbw.tag);
#endif
    if (ok && data_len) {
        ok = bulk_transfer(dev, data_in ? dev->bulk_in_dci : dev->bulk_out_dci,
                           data, data_phys, data_len, data_in);
#ifdef NETWORK_BOOT_DIAG
        if (!ok) kprint("[USB-BOT] data failed tag=%u in=%u bytes=%u\n",
                        cbw.tag, data_in ? 1U : 0U, data_len);
#endif
    }
    if (ok) {
        ok = bulk_transfer(dev, dev->bulk_in_dci, csw_dma, csw_phys,
                           sizeof(csw), true);
#ifdef NETWORK_BOOT_DIAG
        if (!ok) kprint("[USB-BOT] CSW transfer failed tag=%u\n", cbw.tag);
#endif
    }
    if (ok) {
        memcpy(&csw, csw_dma, sizeof(csw));
        ok = csw.signature == 0x53425355U && csw.tag == cbw.tag && csw.status == 0;
#ifdef NETWORK_BOOT_DIAG
        if (!ok)
            kprint("[USB-BOT] bad CSW want-tag=%u got-signature=%x got-tag=%u status=%u\n",
                   cbw.tag, csw.signature, csw.tag, csw.status);
#endif
    }
    xhci_dma_free(cbw_dma, sizeof(cbw));
    xhci_dma_free(csw_dma, sizeof(csw));
    return ok;
}

/* A newly inserted QEMU/USB medium may report UNIT ATTENTION on its first
 * media command.  Clear that transient SCSI condition through the normal BOT
 * request-sense path before probing capacity.  The retry count is deliberately
 * small: a nonresponsive device must still fail enumeration promptly. */
typedef struct {
    bool valid;
    u8 key;
    u8 asc;
    u8 ascq;
} bot_sense_t;

static bool bot_request_sense(usb_mass_storage_device_t *dev, u8 failed_opcode,
                              bot_sense_t *out_sense, bool log_failure)
{
    u8 cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    uintptr_t sense_phys;
    u8 *sense = (u8 *)xhci_dma_alloc(32U, &sense_phys);

    if (out_sense) memset(out_sense, 0, sizeof(*out_sense));
    if (!sense) return false;
    if (bot_command(dev, cdb, sizeof(cdb), sense, sense_phys, 18U, true)) {
        if (out_sense) {
            out_sense->valid = true;
            out_sense->key = sense[2] & 0x0fU;
            out_sense->asc = sense[12];
            out_sense->ascq = sense[13];
        }
        if (log_failure) {
            /* This is emitted only after a failed SCSI command.  It makes an
             * actual medium rejection distinguishable from transport loss
             * while keeping normal I/O quiet. */
            kprint("[USB storage] SCSI opcode %02x failed: sense=%02x/%02x/%02x\n",
                   failed_opcode, sense[2] & 0x0fU, sense[12], sense[13]);
        }
    } else if (log_failure) {
        kprint("[USB storage] SCSI opcode %02x failed; REQUEST SENSE failed\n",
               failed_opcode);
    }
    xhci_dma_free(sense, 32U);
    return out_sense ? out_sense->valid : true;
}

/* BOT retries can originate in a ring-3 storage syscall, whose saved IF is
 * clear.  PIT-backed timer_sleep() would then wait for an IRQ that cannot be
 * delivered to that caller.  The HPET path is interrupt independent; on a
 * platform without it, yielding once still lets the xHCI service worker and
 * timer owner run without turning a transient SCSI condition into a hang. */
static void bot_retry_backoff(void)
{
    if (!timer_monotonic_delay_us(10000U))
        (void)scheduler_yield();
}

static bool bot_wait_until_ready(usb_mass_storage_device_t *dev)
{
    u8 cdb[6] = { 0x00, 0, 0, 0, 0, 0 };

    for (u32 attempt = 0; attempt < 4U; attempt++) {
        if (bot_command(dev, cdb, sizeof(cdb), NULL, 0, 0, false))
            return true;
        (void)bot_request_sense(dev, cdb[0], NULL, true);
        if (attempt + 1U < 4U)
            bot_retry_backoff();
    }
    return false;
}

static bool bot_command_with_sense_retry(usb_mass_storage_device_t *dev,
                                         const u8 *cdb, u8 cdb_len,
                                         void *data, uintptr_t data_phys,
                                         u32 data_len, bool data_in)
{
    for (u32 attempt = 0; attempt < 4U; attempt++) {
        if (bot_command(dev, cdb, cdb_len, data, data_phys, data_len, data_in))
            return true;
        (void)bot_request_sense(dev, cdb[0], NULL, true);
        if (attempt + 1U < 4U)
            bot_retry_backoff();
    }
    return false;
}

static bool storage_read(block_device_t *block, u64 lba, u32 count, void *buffer)
{
    usb_mass_storage_device_t *dev = (usb_mass_storage_device_t *)block->driver_data;
    u8 *out = (u8 *)buffer;
    uintptr_t phys;
    u8 *dma;
    if (!dev || !__atomic_load_n(&dev->active, __ATOMIC_ACQUIRE) ||
        !buffer || block->driver_data != dev ||
        block->id != dev->block.id || block->sector_size != 512 ||
        lba >= dev->block_count ||
        count > dev->block_count - lba) return false;
    dma = (u8 *)xhci_dma_alloc(4096U, &phys);
    if (!dma) return false;
    while (count) {
        if (!__atomic_load_n(&dev->active, __ATOMIC_ACQUIRE)) {
            xhci_dma_free(dma, 4096U);
            storage_teardown_wakeup(dev);
            return false;
        }
        u16 chunk = count > 8U ? 8U : (u16)count;
        u8 cdb[10] = { 0x28, 0, (u8)(lba >> 24), (u8)(lba >> 16), (u8)(lba >> 8),
                       (u8)lba, 0, (u8)(chunk >> 8), (u8)chunk, 0 };
        /* Real removable media may report a transient UNIT ATTENTION after
         * insertion or a cache-state transition.  Use the same short,
         * sense-clearing retry policy as capacity discovery; it is bounded
         * and rechecks liveness between chunks rather than turning a single
         * recoverable BOT status into a permanently unusable volume. */
        if (!bot_command_with_sense_retry(dev, cdb, sizeof(cdb), dma, phys,
                                          (u32)chunk * 512U, true)) {
#ifdef NETWORK_BOOT_DIAG
            kprint("[USB-READ] failed slot=%u lba=%llu sectors=%u\n",
                   dev->slot_id, lba, (u32)chunk);
#endif
            xhci_dma_free(dma, 4096U);
            storage_teardown_wakeup(dev);
            return false;
        }
        memcpy(out, dma, (usize)chunk * 512U);
        out += (usize)chunk * 512U;
        lba += chunk;
        count -= chunk;
    }
    xhci_dma_free(dma, 4096U);
    storage_teardown_wakeup(dev);
    return true;
}

static bool storage_write(block_device_t *block, u64 lba, u32 count,
                          const void *buffer)
{
    usb_mass_storage_device_t *dev = (usb_mass_storage_device_t *)block->driver_data;
    const u8 *input = (const u8 *)buffer;
    uintptr_t phys;
    u8 *dma;

    if (!dev || !__atomic_load_n(&dev->active, __ATOMIC_ACQUIRE) ||
        !buffer || block->driver_data != dev ||
        block->id != dev->block.id || block->sector_size != 512 ||
        lba >= dev->block_count ||
        count > dev->block_count - lba) return false;
    dma = (u8 *)xhci_dma_alloc(4096U, &phys);
    if (!dma) return false;
    while (count) {
        if (!__atomic_load_n(&dev->active, __ATOMIC_ACQUIRE)) {
            xhci_dma_free(dma, 4096U);
            storage_teardown_wakeup(dev);
            return false;
        }
        u16 chunk = count > 8U ? 8U : (u16)count;
        u8 cdb[10] = { 0x2a, 0, (u8)(lba >> 24), (u8)(lba >> 16),
                       (u8)(lba >> 8), (u8)lba, 0, (u8)(chunk >> 8),
                       (u8)chunk, 0 };
        usize bytes = (usize)chunk * 512U;
        memcpy(dma, input, bytes);
        if (!bot_command_with_sense_retry(dev, cdb, sizeof(cdb), dma, phys,
                                          (u32)bytes, false)) {
            xhci_dma_free(dma, 4096U);
            storage_teardown_wakeup(dev);
            return false;
        }
        input += bytes;
        lba += chunk;
        count -= chunk;
    }
    xhci_dma_free(dma, 4096U);
    storage_teardown_wakeup(dev);
    return true;
}

/* SCSI SYNCHRONIZE CACHE(10) is the BOT-visible durability boundary for a
 * removable USB disk with volatile write cache.  A small class of ordinary
 * flash bridges correctly reject it when they advertise no write cache at
 * all; only that exact capability combination may use completed WRITE(10)
 * commands as the available durability boundary. */
static bool storage_flush(block_device_t *block)
{
    usb_mass_storage_device_t *dev;
    const u8 cdb[10] = { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    bool result = false;

    if (!block || !(dev = (usb_mass_storage_device_t *)block->driver_data) ||
        !__atomic_load_n(&dev->active, __ATOMIC_ACQUIRE) ||
        block->driver_data != dev || block->id != dev->block.id ||
        block->sector_size != 512U) return false;
    if (dev->flush_unsupported) {
        result = dev->write_cache_known && !dev->write_cache_enabled;
        storage_teardown_wakeup(dev);
        return result && __atomic_load_n(&dev->active, __ATOMIC_ACQUIRE);
    }
    for (u32 attempt = 0; attempt < 4U; attempt++) {
        bot_sense_t sense;

        if (bot_command(dev, cdb, sizeof(cdb), NULL, 0, 0, false)) {
            result = true;
            break;
        }
        (void)bot_request_sense(dev, cdb[0], &sense, false);
        if (sense.valid && sense.key == 0x05U && sense.asc == 0x20U &&
            sense.ascq == 0x00U) {
            dev->flush_unsupported = true;
            if (dev->write_cache_known && !dev->write_cache_enabled) {
                kprint("[USB storage] SYNCHRONIZE CACHE unavailable; device reports no write cache\n");
                result = true;
            } else {
                kprint("[USB storage] SYNCHRONIZE CACHE unavailable with volatile cache state; refusing flush\n");
            }
            break;
        }
        if (attempt + 1U < 4U)
            bot_retry_backoff();
    }
    storage_teardown_wakeup(dev);
    return result && __atomic_load_n(&dev->active, __ATOMIC_ACQUIRE);
}

/* MODE SENSE(6)'s four-byte mode-parameter header carries the device
 * specific write-protect bit.  This is advisory only when a USB bridge does
 * not implement MODE SENSE: discovery must remain usable, but a successful
 * report is authoritative enough to prevent writes before they reach the
 * transport. */
static bool storage_query_write_protect(usb_mass_storage_device_t *dev,
                                        bool *out_read_only)
{
    static const u8 cdb[6] = { 0x1a, 0x08, 0x3f, 0, 4, 0 };
    uintptr_t parameters_phys;
    u8 *parameters;
    bool result;

    if (!dev || !out_read_only) return false;
    *out_read_only = false;
    parameters = (u8 *)xhci_dma_alloc(4U, &parameters_phys);
    if (!parameters) return false;
    result = bot_command(dev, cdb, sizeof(cdb), parameters,
                         parameters_phys, 4U, true);
    if (result) *out_read_only = (parameters[2] & 0x80U) != 0U;
    xhci_dma_free(parameters, 4U);
    return result;
}

/* MODE SENSE(6), caching page 0x08.  DBD keeps the page immediately after
 * the four-byte mode header, so the WCE bit can be checked without trusting a
 * variable block descriptor.  Failure merely leaves the cache state unknown;
 * it never relaxes a later flush failure. */
static bool storage_query_write_cache(usb_mass_storage_device_t *dev)
{
    /* The SBC caching page has an 18-byte body plus its two-byte page
       header; with MODE SENSE(6)'s four-byte header the standard response is
       exactly 24 bytes.  Request that exact size: current USB BOT transport
       deliberately treats an unexpected short data phase as a failed command
       rather than guessing whether its CSW still belongs to this request. */
    static const u8 cdb[6] = { 0x1a, 0x08, 0x08, 0, 24, 0 };
    uintptr_t parameters_phys;
    u8 *parameters;
    usize returned;
    bool result;

    if (!dev) return false;
    dev->write_cache_known = false;
    dev->write_cache_enabled = false;
    parameters = (u8 *)xhci_dma_alloc(24U, &parameters_phys);
    if (!parameters) return false;
    memset(parameters, 0, 24U);
    result = bot_command(dev, cdb, sizeof(cdb), parameters,
                         parameters_phys, 24U, true);
    if (result) {
        returned = (usize)parameters[0] + 1U;
        if (returned > 24U) returned = 24U;
        if (returned >= 7U && parameters[3] == 0U &&
            (parameters[4] & 0x3fU) == 0x08U && parameters[5] >= 1U) {
            dev->write_cache_known = true;
            dev->write_cache_enabled = (parameters[6] & 0x04U) != 0U;
        }
    }
    xhci_dma_free(parameters, 64U);
    return dev->write_cache_known;
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

    dev = NULL;
    for (u32 index = 0; index < 4U; index++) {
        if (!devices[index].active) {
            dev = &devices[index];
            break;
        }
    }
    if (!dev) return false;
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

    if (!bot_wait_until_ready(dev)) {
        xhci_dma_free(inquiry, 36);
        xhci_dma_free(capacity, 8);
        return false;
    }

    out_result->stage = XHCI_STORAGE_STAGE_CAPACITY;
    if (!bot_command_with_sense_retry(dev, capacity_cdb, 10, capacity,
                                      capacity_phys, 8, true)) {
        xhci_dma_free(inquiry, 36);
        xhci_dma_free(capacity, 8);
        return false;
    }

    out_result->bot_initialized = true;

    dev->block_size = be32(capacity + 4);
    dev->block_count = (u64)be32(capacity) + 1ULL;
    storage_copy_inquiry_model(dev->block.model, sizeof(dev->block.model),
                               inquiry);
    xhci_dma_free(inquiry, 36);
    xhci_dma_free(capacity, 8);

    out_result->stage = XHCI_STORAGE_STAGE_GEOMETRY;
    if (dev->block_size != 512 || !dev->block_count) {
        gpt_remove_partitions_for_parent(&dev->block);
        return false;
    }
    out_result->capacity_known = true;
    dev->block.type = BLOCK_DEVICE_USB;
    dev->block.sector_size = dev->block_size;
    dev->block.sector_count = dev->block_count;
    dev->block.read = storage_read;
    dev->block.write = storage_write;
    dev->block.flush = storage_flush;
    dev->block.driver_data = dev;
    /* A compliant USB mass-storage device exposes physical write protection
     * through MODE SENSE.  Keep an unsupported optional MODE SENSE command
     * from making otherwise-readable media disappear, but honor a positive
     * report before publishing the block instance. */
    (void)storage_query_write_protect(dev, &dev->block.read_only);
    (void)storage_query_write_cache(dev);
    strncpy(dev->block.connection, "usb", sizeof(dev->block.connection) - 1U);
    dev->block.connection[sizeof(dev->block.connection) - 1U] = '\0';

    /* GPT discovery reads through the not-yet-published parent block object.
       Keep the driver instance live for that bounded probe; publication and
       device_count still happen only after the complete scan succeeds. */
    dev->active = true;

    out_result->gpt_scan_ran = true;
    out_result->gpt_found = gpt_scan_device(&dev->block);

    /* Publish the storage device only after GPT discovery has completed.
       INIT_STORAGE uses block registration as its readiness boundary; the
       parent disk and all of its partitions must therefore appear as one
       completed discovery result, never half-way through a GPT scan. */
    out_result->stage = XHCI_STORAGE_STAGE_BLOCK_REGISTER;
    if (!block_register(&dev->block)) {
        gpt_remove_partitions_for_parent(&dev->block);
        return false;
    }
    dev->block_registered = true;
    device_count++;
    out_result->block_registered = true;
    KERNEL_BOOT_DEBUG_LOG("[OK] USB block device registered (%llu sectors)\n",
                          dev->block_count);

    out_result->stage = XHCI_STORAGE_STAGE_READY;
    return true;
}
