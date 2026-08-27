#include <xhci.h>
#include <xhci_ring.h>
#include <xhci_trb.h>
#include <xhci_regs.h>
#include <stddef.h>
/* ==============================================================================
 * External Dependencies
 * These accessors map to the internal state managed in the final xhci.c file.
 * The controller must track the DMA buffers assigned to each Endpoint Context.
 * ============================================================================== */

extern xhci_ring_t* xhci_get_ep_ring(xhci_controller_t *xhc, u8 slot_id, u8 dci);
extern volatile u32* xhci_get_doorbell_ptr(xhci_controller_t *xhc, u8 target_idx);

/* Retrieves the virtual pointer to the 8-byte DMA buffer for a given endpoint */
extern uintptr_t xhci_get_ep_dma_phys_for_trb(xhci_controller_t *xhc,
                                               u8 slot_id, u8 dci,
                                               uintptr_t trb_phys);
extern u8* xhci_get_ep_dma_buffer_for_trb(xhci_controller_t *xhc,
                                           u8 slot_id, u8 dci,
                                           uintptr_t trb_phys);
extern bool xhci_is_hid_endpoint(xhci_controller_t *xhc, u8 slot_id, u8 dci);
extern bool xhci_complete_async_transfer(xhci_controller_t *xhc, u8 slot_id,
                                          u8 dci, uintptr_t completion_trb);

/* Retrieves the OS-registered keyboard callback function */
extern xhci_hid_keyboard_callback_t xhci_get_keyboard_callback(xhci_controller_t *xhc);

/* Logging subsystem */
extern void kprint(const char *fmt, ...);
extern u64 timer_uptime_ms(void);

static uintptr_t hid_last_completion_trb;
static u32 hid_last_completion_code;
static bool hid_arm_logged[256];
static u8 hid_event_log_count[256];
static u8 hid_transfer_log_count;
#if XHCI_DEBUG
static bool hid_last_report_valid[256];
static u8 hid_last_report[256][8];
#endif


static const char *xhci_hid_queue_reason(xhci_status_t status)
{
    switch (status) {
        case XHCI_ERR_NO_MEMORY: return "ring-full";
        case XHCI_ERR_INVALID_PARAM: return "invalid-ring";
        default: return "other";
    }
}


/* ==============================================================================
 * Internal Helper Functions
 * ============================================================================== */

/*
 * Rings the doorbell for a specific Slot and Endpoint Context Index (DCI).
 */
static bool xhci_ring_ep_doorbell(xhci_controller_t *xhc, u8 slot_id, u8 dci) {
    volatile u32 *db = xhci_get_doorbell_ptr(xhc, slot_id);
    if (db) {
        *db = XHCI_DB_TARGET(dci);
        return true;
    }
    return false;
}


/* ==============================================================================
 * HID Interrupt IN Operations (Phase 7)
 * ============================================================================== */

/*
 * Queues a Normal TRB onto the target Interrupt IN transfer ring.
 * * CRITICAL xHCI CONCEPT: USB is host-driven. Even for "Interrupt" endpoints, 
 * the device cannot send data unless the host controller has explicitly provided 
 * an empty DMA buffer via a TRB on the transfer ring. 
 * * This function must be called initially during setup, and then RE-CALLED 
 * every single time a keystroke is received to re-arm the endpoint.
 *
 * @param xhc      The controller instance.
 * @param slot_id  The Slot ID of the keyboard device.
 * @param dci      The Device Context Index of the Interrupt IN endpoint.
 */
bool xhci_hid_queue_read(xhci_controller_t *xhc, u8 slot_id, u8 dci) {
    if (!xhc || slot_id == 0 || dci == 0) return false;

    xhci_ring_t *ep_ring = xhci_get_ep_ring(xhc, slot_id, dci);

    if (!ep_ring) {
        kprint("[xHCI] HID Queue Error: Missing ring for Slot %d, EP %d\n",
               slot_id, dci);
        return false;
    }

    u32 trb_index = ep_ring->enqueue_idx;
    uintptr_t transfer_trb_phys = ep_ring->phys_base +
        trb_index * sizeof(xhci_trb_t);
    uintptr_t buffer_phys = xhci_get_ep_dma_phys_for_trb(
        xhc, slot_id, dci, transfer_trb_phys);
    if (!buffer_phys) {
        kprint("[xHCI] HID Queue Error: Missing buffer for Slot %d, EP %d\n",
               slot_id, dci);
        return false;
    }

    u32 param1 = XHCI_TRB_PARAM1_PTR(buffer_phys);
    u32 param2 = XHCI_TRB_PARAM2_PTR(buffer_phys);
    
    /* Request 8 bytes (Standard HID Boot Protocol Report Size) */
    u32 status = XHCI_TRB_STS_XFER_LEN_SET(8);
    
    /* Interrupt on Short Packet (ISP): Complete early if device sends < 8 bytes.
       Interrupt On Completion (IOC): Fire a hardware IRQ when this buffer is filled. */
    u32 control = XHCI_TRB_CTRL_TYPE_SET(XHCI_TRB_TYPE_NORMAL) | 
                       XHCI_TRB_CTRL_ISP | 
                       XHCI_TRB_CTRL_IOC;

    /* The controller may complete an interrupt transfer short.  Clear the
       exact buffer before handing it back so bytes not written by that
       transfer cannot be mistaken for a still-held key. */
    u8 *buffer = xhci_get_ep_dma_buffer_for_trb(xhc, slot_id, dci,
                                                transfer_trb_phys);
    if (!buffer)
        return false;
    for (u32 i = 0; i < 8; i++)
        buffer[i] = 0;

    if (!xhci_arm_async_transfer(xhc, slot_id, dci, transfer_trb_phys,
                                 transfer_trb_phys, transfer_trb_phys))
        return false;
    xhci_status_t err = xhci_ring_enqueue(ep_ring, param1, param2, status, control);
    if (err == XHCI_SUCCESS) {
        bool doorbell_rung = xhci_ring_ep_doorbell(xhc, slot_id, dci);
        if (!doorbell_rung)
            xhci_cancel_transfer_operation(xhc, slot_id, dci);
        if (!hid_arm_logged[slot_id]) {
            XHCI_DEBUG_LOG("[HID-RT] arm s%u d%u trb=%u db=%u q=%u/%u\n",
                           slot_id, dci, trb_index, doorbell_rung,
                           ep_ring->enqueue_idx, ep_ring->dequeue_idx);
            hid_arm_logged[slot_id] = true;
        }
        return doorbell_rung;
    } else {
        xhci_cancel_transfer_operation(xhc, slot_id, dci);
        kprint("[xHCI] HID Queue Error: Failed to enqueue TRB "
               "(slot=%d ep=%d reason=%s/%d enq=%d deq=%d cycle=%d capacity=%d "
               "last_trb=%p last_code=%d)\n",
               slot_id, dci, xhci_hid_queue_reason(err), err,
               ep_ring->enqueue_idx, ep_ring->dequeue_idx,
               ep_ring->cycle_state, ep_ring->size - 2,
               (void *)(uintptr_t)hid_last_completion_trb,
               hid_last_completion_code);
    }
    return false;
}


/* ==============================================================================
 * Asynchronous Transfer Event Dispatcher
 * Called strictly from xhci_event.c -> xhci_process_events() by the xHCI
 * service owner in normal kernel context.
 * ============================================================================== */

/*
 * Processes a completed transfer on an active endpoint.
 * Extracts the 8-byte HID payload from the DMA buffer, decodes the keys, 
 * passes them to the Mangrove OS input subsystem, and re-arms the transfer ring.
 *
 * @param xhc   The controller instance.
 * @param event The Transfer Event TRB popped from the Event Ring.
 */
void xhci_handle_transfer_event(xhci_controller_t *xhc, xhci_trb_t *event) {
    if (!xhc || !event) return;

    /* Extract routing information directly from the Event TRB */
    u8 slot_id = XHCI_TRB_CTRL_SLOT_ID_GET(event->control);
    u8 dci = XHCI_TRB_CTRL_EP_ID_GET(event->control);
    u32 comp_code = XHCI_TRB_STS_COMP_CODE_GET(event->status);
    uintptr_t completion_trb = XHCI_TRB_PTR_GET(event->param1, event->param2);
    xhci_ring_t *ep_ring;
    hid_last_completion_trb = completion_trb;
    hid_last_completion_code = comp_code;

    /* EP0 (DCI 1) transfers are synchronous and handled in xhci_control.c.
       We only asynchronously parse endpoints with DCI >= 2 (Interrupt/Bulk/Isoch). */
    if (dci < 2) {
        return;
    }

    bool hid_match = xhci_is_hid_endpoint(xhc, slot_id, dci);
    if (hid_transfer_log_count < 4) {
        XHCI_DEBUG_LOG("[HID-TE] s%u d%u cc=%u match=%u\n",
                       slot_id, dci, comp_code, hid_match);
        hid_transfer_log_count++;
    }

    if (!hid_match) {
        return;
    }

    ep_ring = xhci_get_ep_ring(xhc, slot_id, dci);
    if (!ep_ring || xhci_ring_reclaim_transfer(ep_ring, completion_trb) != XHCI_SUCCESS) {
        kprint("[xHCI] HID completion bookkeeping error "
               "(slot=%d ep=%d trb=%p code=%d enq=%d deq=%d cycle=%d capacity=%d)\n",
               slot_id, dci, (void *)(uintptr_t)completion_trb, comp_code,
               ep_ring ? ep_ring->enqueue_idx : 0,
               ep_ring ? ep_ring->dequeue_idx : 0,
               ep_ring ? ep_ring->cycle_state : 0,
               ep_ring ? ep_ring->size - 2 : 0);
        return;
    }
    if (!xhci_complete_async_transfer(xhc, slot_id, dci, completion_trb)) {
        kprint("[xHCI] HID completion ownership error "
               "(slot=%d ep=%d trb=%p code=%d)\n",
               slot_id, dci, (void *)(uintptr_t)completion_trb, comp_code);
        return;
    }
    u8 *buffer = NULL;
    u8 active_keys[6] = {0};
    u8 report[8] = {0};
    u8 raw[4] = {0};
    u8 count = 0;
    bool callback_called = false;
    bool report_ready = false;
    u8 modifier_mask = 0;
#if XHCI_DEBUG
    bool report_changed = false;
#endif

    /* Valid completions for HID are SUCCESS and SHORT_PACKET */
    if (comp_code == XHCI_COMP_SUCCESS || comp_code == XHCI_COMP_SHORT_PACKET) {
        u32 residual = XHCI_TRB_STS_XFER_LEN_GET(event->status);
        u32 received = residual < 8 ? 8 - residual : 0;
        buffer = xhci_get_ep_dma_buffer_for_trb(xhc, slot_id, dci,
                                                completion_trb);
        if (buffer) {
            for (u32 i = 0; i < sizeof(report); i++)
                report[i] = i < received ? buffer[i] : 0;
            for (u32 i = 0; i < sizeof(raw); i++) raw[i] = report[i];
#if XHCI_DEBUG
            if (!hid_last_report_valid[slot_id]) {
                report_changed = true;
                hid_last_report_valid[slot_id] = true;
            } else {
                for (u32 i = 0; i < sizeof(report); i++) {
                    if (hid_last_report[slot_id][i] != report[i]) {
                        report_changed = true;
                        break;
                    }
                }
            }
            if (report_changed) {
                for (u32 i = 0; i < sizeof(report); i++)
                    hid_last_report[slot_id][i] = report[i];
            }
#endif
            /* * Standard USB HID Boot Protocol Keyboard Report Layout:
             * Byte 0: Modifier keys (Bit 0: LCtrl, 1: LShift, 2: LAlt, 3: LGUI, 4: RCtrl, 5: RShift, 6: RAlt, 7: RGUI)
             * Byte 1: Reserved (Usually 0x00)
             * Byte 2 to 7: Array of currently pressed key scancodes (Usage Page 0x07)
             */
            modifier_mask = report[0];

            for (int i = 0; i < 6; i++) {
                u8 key_code = report[2 + i];
                
                /* Filter empty slots (0x00) and rollover error states (0x01) */
                if (key_code != 0x00 && key_code != 0x01) {
                    active_keys[count++] = key_code;
                }
            }

            report_ready = true;
        }
    } 
    else {
        kprint("[xHCI] Transfer Error on Slot %d EP %d (Code: %d)\n", slot_id, dci, comp_code);
        /* If we hit a STALL error or similar, endpoint reset logic would go here. */
    }
    /* Re-arm before invoking the OS callback.  The callback may wake a
       userspace reader or start command processing; the next HID report must
       already have a TRB available while that work runs.  The current report
       has been copied into stack-local fields, so reusing the DMA buffer is
       safe before callback dispatch. */
    bool rearmed = xhci_hid_queue_read(xhc, slot_id, dci);
    if (report_ready) {
        xhci_hid_keyboard_callback_t callback =
            xhci_get_keyboard_callback(xhc);
        if (callback) {
            callback(modifier_mask, active_keys, count);
            callback_called = true;
        }
    }
    if (hid_event_log_count[slot_id] < 4) {
        u32 residual = XHCI_TRB_STS_XFER_LEN_GET(event->status);
        XHCI_DEBUG_LOG("[HID-EV] s%u d%u cc=%u rem=%u raw=%02x/%02x/%02x/%02x keys=%u cb=%u re=%u\n",
                       slot_id, dci, comp_code, residual,
                       raw[0], raw[1], raw[2], raw[3], count,
                       callback_called, rearmed);
        hid_event_log_count[slot_id]++;
    }
#if XHCI_DEBUG
    if (report_changed && ep_ring) {
        XHCI_DEBUG_LOG(
            "[HID-REPORT] t=%llu s%u d%u ev=%p trb=%p cc=%u "
            "report=%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x "
            "keys=%u q=%u/%u re=%u\n",
            (unsigned long long)timer_uptime_ms(), slot_id, dci,
            (void *)event, (void *)(uintptr_t)completion_trb, comp_code,
            report[0], report[1], report[2], report[3], report[4],
            report[5], report[6], report[7], count, ep_ring->enqueue_idx,
            ep_ring->dequeue_idx, rearmed);
    }
#endif
}
