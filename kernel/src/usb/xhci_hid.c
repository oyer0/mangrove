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
extern u8* xhci_get_ep_dma_buffer(xhci_controller_t *xhc, u8 slot_id, u8 dci);

/* Retrieves the physical address of the DMA buffer for a given endpoint */
extern uintptr_t xhci_get_ep_dma_phys(xhci_controller_t *xhc, u8 slot_id, u8 dci);
extern bool xhci_is_hid_endpoint(xhci_controller_t *xhc, u8 slot_id, u8 dci);

/* Retrieves the OS-registered keyboard callback function */
extern xhci_hid_keyboard_callback_t xhci_get_keyboard_callback(xhci_controller_t *xhc);

/* Logging subsystem */
extern void kprint(const char *fmt, ...);

static uintptr_t hid_last_completion_trb;
static u32 hid_last_completion_code;
static bool hid_arm_logged[256];
static u8 hid_event_log_count[256];
static u8 hid_transfer_log_count;

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
    uintptr_t buffer_phys = xhci_get_ep_dma_phys(xhc, slot_id, dci);

    if (!ep_ring || buffer_phys == 0) {
        kprint("[xHCI] HID Queue Error: Missing ring or buffer for Slot %d, EP %d\n", slot_id, dci);
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

    u32 trb_index = ep_ring->enqueue_idx;
    xhci_status_t err = xhci_ring_enqueue(ep_ring, param1, param2, status, control);
    if (err == XHCI_SUCCESS) {
        bool doorbell_rung = xhci_ring_ep_doorbell(xhc, slot_id, dci);
        if (!hid_arm_logged[slot_id]) {
            XHCI_DEBUG_LOG("[HID-RT] arm s%u d%u trb=%u db=%u q=%u/%u\n",
                           slot_id, dci, trb_index, doorbell_rung,
                           ep_ring->enqueue_idx, ep_ring->dequeue_idx);
            hid_arm_logged[slot_id] = true;
        }
        return doorbell_rung;
    } else {
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
 * Called strictly from xhci_event.c -> xhci_process_events() during ISR context.
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

    u8 *buffer = NULL;
    u8 active_keys[6] = {0};
    u8 raw[4] = {0};
    u8 count = 0;
    bool callback_called = false;

    /* Valid completions for HID are SUCCESS and SHORT_PACKET */
    if (comp_code == XHCI_COMP_SUCCESS || comp_code == XHCI_COMP_SHORT_PACKET) {
        buffer = xhci_get_ep_dma_buffer(xhc, slot_id, dci);
        if (buffer) {
            for (u32 i = 0; i < sizeof(raw); i++) raw[i] = buffer[i];
            
            /* * Standard USB HID Boot Protocol Keyboard Report Layout:
             * Byte 0: Modifier keys (Bit 0: LCtrl, 1: LShift, 2: LAlt, 3: LGUI, 4: RCtrl, 5: RShift, 6: RAlt, 7: RGUI)
             * Byte 1: Reserved (Usually 0x00)
             * Byte 2 to 7: Array of currently pressed key scancodes (Usage Page 0x07)
             */
            u8 modifier_mask = buffer[0];

            for (int i = 0; i < 6; i++) {
                u8 key_code = buffer[2 + i];
                
                /* Filter empty slots (0x00) and rollover error states (0x01) */
                if (key_code != 0x00 && key_code != 0x01) {
                    active_keys[count++] = key_code;
                }
            }

            /* Dispatch to the OS higher-level keyboard interpreter */
            xhci_hid_keyboard_callback_t callback = xhci_get_keyboard_callback(xhc);
            if (callback) {
                callback(modifier_mask, active_keys, count);
                callback_called = true;
            }
        }
    } 
    else {
        kprint("[xHCI] Transfer Error on Slot %d EP %d (Code: %d)\n", slot_id, dci, comp_code);
        /* If we hit a STALL error or similar, endpoint reset logic would go here. */
    }

    /* * RE-ARMING PHASE 
     * Regardless of successful payload or minor errors (like babble/transaction error), 
     * we must queue a fresh TRB to catch the next keystroke/release event. 
     * Without this line, the keyboard works exactly once.
     */
    bool rearmed = xhci_hid_queue_read(xhc, slot_id, dci);
    if (hid_event_log_count[slot_id] < 4) {
        u32 residual = XHCI_TRB_STS_XFER_LEN_GET(event->status);
        XHCI_DEBUG_LOG("[HID-EV] s%u d%u cc=%u rem=%u raw=%02x/%02x/%02x/%02x keys=%u cb=%u re=%u\n",
                       slot_id, dci, comp_code, residual,
                       raw[0], raw[1], raw[2], raw[3], count,
                       callback_called, rearmed);
        hid_event_log_count[slot_id]++;
    }
}
