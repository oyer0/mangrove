#include <xhci.h>
#include <xhci_ring.h>
#include <xhci_trb.h>
#include <xhci_regs.h>
#include <stddef.h>

#include <kprint.h>

/* ==============================================================================
 * External Dependencies
 * ============================================================================== */

/* Extracted from Mangrove OS timekeeping subsystem (Stage 6) */
extern void timer_sleep(u64 ms);

/* Controller state accessors (implemented in xhci.c) */
extern xhci_ring_t* xhci_get_event_ring(xhci_controller_t *xhc);
extern xhci_intr_regs_t* xhci_get_intr_regs(xhci_controller_t *xhc, u8 interrupter_idx);

/* Asynchronous event routers (implemented in xhci_port.c and xhci_hid.c) */
extern void xhci_handle_port_status_change(xhci_controller_t *xhc, xhci_trb_t *event);
extern void xhci_handle_transfer_event(xhci_controller_t *xhc, xhci_trb_t *event);


/* ==============================================================================
 * Internal Helper Functions
 * ============================================================================== */

/*
 * Acknowledges event processing to the hardware by updating the Event Ring 
 * Dequeue Pointer (ERDP) and clearing the Event Handler Busy (EHB) flag.
 */
static void xhci_update_erdp(xhci_controller_t *xhc, xhci_ring_t *event_ring) {
    xhci_intr_regs_t *intr_regs = xhci_get_intr_regs(xhc, 0);
    if (!intr_regs || !event_ring) {
        return;
    }

    /* Calculate the exact physical address of the current dequeue index */
    u64 current_dequeue_phys = event_ring->phys_base + (event_ring->dequeue_idx * sizeof(xhci_trb_t));

    /* Write the physical pointer and write 1 to EHB (bit 3) to clear the busy state */
    intr_regs->erdp = current_dequeue_phys | XHCI_ERDP_EHB;
}

/*
 * Maps a hardware TRB completion code to the OS xhci_status_t enumeration.
 */
static xhci_status_t xhci_map_completion_code(u32 comp_code) {
    switch (comp_code) {
        case XHCI_COMP_SUCCESS:
        case XHCI_COMP_SHORT_PACKET: /* Often valid for descriptor reads and HID reports */
            return XHCI_SUCCESS;
        case XHCI_COMP_STALL_ERR:
            return XHCI_ERR_TRANSACTION;
        case XHCI_COMP_RESOURCE_ERR:
        case XHCI_COMP_NO_SLOTS_ERR:
            return XHCI_ERR_NO_MEMORY;
        case XHCI_COMP_USB_TRANSACTION_ERR:
        case XHCI_COMP_BABBLE_ERR:
            return XHCI_ERR_CONTROLLER_BAD;
        default:
            return XHCI_ERR_INVALID_PARAM; /* Generic fault for unmapped codes */
    }
}


/* ==============================================================================
 * Synchronous Polling API
 * Used during controller initialization, enumeration, and device setup where 
 * blocking the thread is acceptable and required.
 * ============================================================================== */

/*
 * Blocks and waits for a Command Completion Event TRB.
 * Any other events (like Port Status Changes) that fire during this wait 
 * are safely routed to their respective asynchronous handlers.
 * * @param xhc               The controller instance.
 * @param expected_cmd_type The TRB Type of the command we are waiting for.
 * @param out_event         Pointer to output the resulting Event TRB for caller extraction.
 * @return                  XHCI_SUCCESS or timeout/hardware failure.
 */
xhci_status_t xhci_wait_for_cmd_completion(xhci_controller_t *xhc, u8 expected_cmd_type, xhci_trb_t *out_event) {
    xhci_ring_t *event_ring = xhci_get_event_ring(xhc);
    if (!event_ring) return XHCI_ERR_INVALID_PARAM;

    /* A 1000ms timeout is standard for USB host controller commands */
    u32 timeout_ms = 1000;

    while (timeout_ms > 0) {
        xhci_trb_t *event = xhci_event_ring_get_next(event_ring);
        
        if (event) {
            u32 event_type = XHCI_TRB_CTRL_TYPE_GET(event->control);
            u32 comp_code = XHCI_TRB_STS_COMP_CODE_GET(event->status);

            /* Copy out the event *before* we advance the ring and overwrite memory */
            xhci_trb_t captured_event = *event;

            /* Advance software tracker and acknowledge to hardware immediately */
            xhci_event_ring_advance(event_ring);
            xhci_update_erdp(xhc, event_ring);

            if (event_type == XHCI_TRB_TYPE_CMD_COMPLETION) {
                kprint("Completion code: %u -> status %d\n",
                    comp_code,
                    xhci_map_completion_code(comp_code));

                if (out_event) {
                    *out_event = captured_event;
                }
                
                /* In a fully multithreaded driver, we would verify captured_event.param1 
                   against our specific command's physical address. For synchronous 
                   bootstrapping, catching the completion is sufficient. */
                return xhci_map_completion_code(comp_code);
            } 
            else if (event_type == XHCI_TRB_TYPE_PORT_STATUS_CHANGE) {
                xhci_handle_port_status_change(xhc, &captured_event);
            }
            else if (event_type == XHCI_TRB_TYPE_TRANSFER_EVENT) {
                xhci_handle_transfer_event(xhc, &captured_event);
            }
        } else {
            /* Yield CPU time via Mangrove OS PIT subsystem */
            timer_sleep(1);
            timeout_ms--;
        }
    }

    return XHCI_ERR_TIMEOUT;
}

/*
 * Blocks and waits for a Transfer Event TRB.
 * Used strictly for synchronous Control Transfers (EP0) like GET_DESCRIPTOR.
 * * @param xhc               The controller instance.
 * @param out_event         Pointer to output the resulting Transfer Event TRB.
 * @return                  XHCI_SUCCESS or timeout/hardware failure.
 */
xhci_status_t xhci_wait_for_transfer_completion(xhci_controller_t *xhc, xhci_trb_t *out_event) {
    xhci_ring_t *event_ring = xhci_get_event_ring(xhc);
    if (!event_ring) return XHCI_ERR_INVALID_PARAM;

    u32 timeout_ms = 2500;

    while (timeout_ms > 0) {
        xhci_trb_t *event = xhci_event_ring_get_next(event_ring);
        
        if (event) {
            u32 event_type = XHCI_TRB_CTRL_TYPE_GET(event->control);
            u32 comp_code = XHCI_TRB_STS_COMP_CODE_GET(event->status);

            xhci_trb_t captured_event = *event;

            xhci_event_ring_advance(event_ring);
            xhci_update_erdp(xhc, event_ring);

            if (event_type == XHCI_TRB_TYPE_TRANSFER_EVENT) {
                if (out_event) {
                    *out_event = captured_event;
                }
                return xhci_map_completion_code(comp_code);
            }
            else if (event_type == XHCI_TRB_TYPE_PORT_STATUS_CHANGE) {
                xhci_handle_port_status_change(xhc, &captured_event);
            }
        } else {
            // Spin briefly and force decrement so it's guaranteed to time out
            for (volatile int i = 0; i < 10000; i++) {
                __asm__ volatile("pause");
            }
            timeout_ms--;
        }
    }

    return XHCI_ERR_TIMEOUT;
}
/* ==============================================================================
 * Asynchronous Event Dispatcher
 * Called strictly from the xHCI hardware interrupt handler (ISR context) after 
 * Phase 6 is complete and the OS is relying on Interrupt IN TRBs.
 * ============================================================================== */

/*
 * Drains the Event Ring completely, routing all pending events, and updates 
 * the hardware Dequeue Pointer once at the end to minimize MMIO writes during 
 * high interrupt pressure.
 */
void xhci_process_events(xhci_controller_t *xhc) {
    xhci_ring_t *event_ring = xhci_get_event_ring(xhc);
    if (!event_ring) return;

    bool processed_any = false;

    while (true) {
        xhci_trb_t *event = xhci_event_ring_get_next(event_ring);
        if (!event) {
            break; /* Ring is drained */
        }

        u32 event_type = XHCI_TRB_CTRL_TYPE_GET(event->control);
        xhci_trb_t captured_event = *event;

        /* Advance software tracker */
        xhci_event_ring_advance(event_ring);
        processed_any = true;

        /* Dispatch to corresponding subsystem handler */
        if (event_type == XHCI_TRB_TYPE_TRANSFER_EVENT) {
            /* Transfer Events on async endpoints usually contain HID Key Reports */
            xhci_handle_transfer_event(xhc, &captured_event);
        } 
        else if (event_type == XHCI_TRB_TYPE_PORT_STATUS_CHANGE) {
            /* Hotplug / Unplug events */
            xhci_handle_port_status_change(xhc, &captured_event);
        }
    }

    /* Batch update the ERDP if we processed anything */
    if (processed_any) {
        xhci_update_erdp(xhc, event_ring);
    }
}
