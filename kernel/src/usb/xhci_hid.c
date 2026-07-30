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

/* Retrieves the OS-registered keyboard callback function */
extern xhci_hid_keyboard_callback_t xhci_get_keyboard_callback(xhci_controller_t *xhc);

/* Logging subsystem */
extern void kprint(const char *fmt, ...);


/* ==============================================================================
 * Internal Helper Functions
 * ============================================================================== */

/*
 * Rings the doorbell for a specific Slot and Endpoint Context Index (DCI).
 */
static void xhci_ring_ep_doorbell(xhci_controller_t *xhc, u8 slot_id, u8 dci) {
    volatile u32 *db = xhci_get_doorbell_ptr(xhc, slot_id);
    if (db) {
        *db = XHCI_DB_TARGET(dci);
    }
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
void xhci_hid_queue_read(xhci_controller_t *xhc, u8 slot_id, u8 dci) {
    if (!xhc || slot_id == 0 || dci == 0) return;

    xhci_ring_t *ep_ring = xhci_get_ep_ring(xhc, slot_id, dci);
    uintptr_t buffer_phys = xhci_get_ep_dma_phys(xhc, slot_id, dci);

    if (!ep_ring || buffer_phys == 0) {
        kprint("[xHCI] HID Queue Error: Missing ring or buffer for Slot %d, EP %d\n", slot_id, dci);
        return;
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

    xhci_status_t err = xhci_ring_enqueue(ep_ring, param1, param2, status, control);
    if (err == XHCI_SUCCESS) {
        xhci_ring_ep_doorbell(xhc, slot_id, dci);
    } else {
        kprint("[xHCI] HID Queue Error: Failed to enqueue TRB.\n");
    }
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

    /* EP0 (DCI 1) transfers are synchronous and handled in xhci_control.c.
       We only asynchronously parse endpoints with DCI >= 2 (Interrupt/Bulk/Isoch). */
    if (dci < 2) {
        return;
    }

    /* Valid completions for HID are SUCCESS and SHORT_PACKET */
    if (comp_code == XHCI_COMP_SUCCESS || comp_code == XHCI_COMP_SHORT_PACKET) {
        
        u8 *buffer = xhci_get_ep_dma_buffer(xhc, slot_id, dci);
        if (buffer) {
            
            /* * Standard USB HID Boot Protocol Keyboard Report Layout:
             * Byte 0: Modifier keys (Bit 0: LCtrl, 1: LShift, 2: LAlt, 3: LGUI, 4: RCtrl, 5: RShift, 6: RAlt, 7: RGUI)
             * Byte 1: Reserved (Usually 0x00)
             * Byte 2 to 7: Array of currently pressed key scancodes (Usage Page 0x07)
             */
            u8 modifier_mask = buffer[0];
            u8 active_keys[6];
            u8 count = 0;

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
    xhci_hid_queue_read(xhc, slot_id, dci);
}
