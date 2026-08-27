#include <xhci.h>
#include <xhci_ring.h>
#include <xhci_trb.h>
#include <xhci_regs.h>
#include <stddef.h>
#include <scheduler.h>

/* ==============================================================================
 * External Dependencies
 * ============================================================================== */

/* Extracted from Mangrove OS timekeeping subsystem (Stage 6) */
extern void timer_sleep(u64 ms);
extern u64 timer_uptime_ms(void);
extern void kprint(const char *fmt, ...);

/* Controller state accessors (implemented in xhci.c) */
extern xhci_ring_t* xhci_get_event_ring(xhci_controller_t *xhc);
extern xhci_intr_regs_t* xhci_get_intr_regs(xhci_controller_t *xhc, u8 interrupter_idx);
extern bool xhci_is_service_owner(xhci_controller_t *xhc);
extern void xhci_process_events(xhci_controller_t *xhc);
extern void xhci_mark_event_work_pending(xhci_controller_t *xhc);

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
        case XHCI_COMP_USB_TRANSACTION_ERR:
            return XHCI_ERR_TRANSACTION;
        case XHCI_COMP_RESOURCE_ERR:
        case XHCI_COMP_NO_SLOTS_ERR:
            return XHCI_ERR_NO_MEMORY;
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
 * Synchronous callers never inspect the Event Ring.  The xHCI service owner
 * captures events into the appropriate mailbox and these functions wait only
 * for that published completion.
 */
xhci_status_t xhci_wait_for_cmd_completion(xhci_controller_t *xhc,
                                            u8 expected_cmd_type,
                                            xhci_trb_t *out_event)
{
    u64 deadline;
    xhci_trb_t captured;
    (void)expected_cmd_type; /* The armed command record performs the match. */
    if (!xhc)
        return XHCI_ERR_INVALID_PARAM;
    deadline = timer_uptime_ms() + 1000;
    while (timer_uptime_ms() < deadline) {
        if (xhci_take_command_completion(xhc, &captured)) {
            if (out_event)
                *out_event = captured;
            return xhci_map_completion_code(
                XHCI_TRB_STS_COMP_CODE_GET(captured.status));
        }
        if (xhci_is_service_owner(xhc)) {
            xhci_process_events(xhc);
            timer_sleep(1);
        } else {
            /* A non-owner may be inside a syscall, where SYSCALL masks IF
               and timer_sleep() would strand the service owner.  Yield the
               caller to the scheduler so the owner can consume the event;
               completion remains the only condition that ends this wait. */
            (void)xhci_start_deferred_worker(xhc);
            (void)scheduler_sleep(1);
        }
    }
    xhci_cancel_command_wait(xhc);
    return XHCI_ERR_TIMEOUT;
}

/*
 * Blocks and waits for a Transfer Event TRB.
 * Used strictly for synchronous Control Transfers (EP0) like GET_DESCRIPTOR.
 * * @param xhc               The controller instance.
 * @param out_event         Pointer to output the resulting Transfer Event TRB.
 * @return                  XHCI_SUCCESS or timeout/hardware failure.
 */
xhci_status_t xhci_wait_for_transfer_completion(xhci_controller_t *xhc,
                                                xhci_trb_t *out_event)
{
    u64 deadline;
    xhci_trb_t captured;
    if (!xhc)
        return XHCI_ERR_INVALID_PARAM;
    deadline = timer_uptime_ms() + 2500;
    while (timer_uptime_ms() < deadline) {
        if (xhci_take_transfer_completion(xhc, &captured)) {
            if (out_event)
                *out_event = captured;
            return xhci_map_completion_code(
                XHCI_TRB_STS_COMP_CODE_GET(captured.status));
        }
        if (xhci_is_service_owner(xhc)) {
            xhci_process_events(xhc);
            timer_sleep(1);
        } else {
            /* See the command wait above: this is a scheduler wait, not a
               timing assumption about USB completion. */
            (void)xhci_start_deferred_worker(xhc);
            (void)scheduler_sleep(1);
        }
    }
    xhci_cancel_transfer_wait(xhc);
    return XHCI_ERR_TIMEOUT;
}

xhci_status_t xhci_wait_for_transfer_completion_for(xhci_controller_t *xhc,
                                                    u8 slot_id, u8 dci,
                                                    xhci_trb_t *out_event)
{
    (void)slot_id;
    (void)dci;
    return xhci_wait_for_transfer_completion(xhc, out_event);
}
/* ==============================================================================
 * Asynchronous Event Dispatcher
 * Called strictly from the xHCI service owner in normal kernel context.
 * ============================================================================== */

/*
 * Drains the Event Ring completely, routing all pending events, and updates
 * the hardware Dequeue Pointer once at the end.  No other path may dequeue
 * events or update ERDP.
 */
#define XHCI_EVENT_BATCH_LIMIT 32U

void xhci_process_events(xhci_controller_t *xhc) {
    xhci_ring_t *event_ring = xhci_get_event_ring(xhc);
    u32 processed_count = 0;
    if (!event_ring) return;

    bool processed_any = false;

    while (processed_count < XHCI_EVENT_BATCH_LIMIT) {
        xhci_trb_t *event = xhci_event_ring_get_next(event_ring);
        if (!event) {
            break; /* Ring is drained */
        }

        u32 event_type = XHCI_TRB_CTRL_TYPE_GET(event->control);
        xhci_trb_t captured_event = *event;

        /* Advance software tracker */
        xhci_event_ring_advance(event_ring);
        processed_any = true;
        processed_count++;

        if (event_type == XHCI_TRB_TYPE_CMD_COMPLETION) {
            if (!xhci_route_command_completion(xhc, &captured_event))
                XHCI_DEBUG_LOG(
                    "[xHCI-COMP] stale command event ptr=%p slot=%u cc=%u\n",
                    (void *)XHCI_TRB_PTR_GET(captured_event.param1,
                                             captured_event.param2),
                    XHCI_TRB_CTRL_SLOT_ID_GET(captured_event.control),
                    XHCI_TRB_STS_COMP_CODE_GET(captured_event.status));
        } else if (event_type == XHCI_TRB_TYPE_TRANSFER_EVENT) {
            xhci_transfer_event_route_t route =
                xhci_route_transfer_event(xhc, &captured_event);
            if (route == XHCI_TRANSFER_EVENT_ASYNC) {
                xhci_handle_transfer_event(xhc, &captured_event);
            } else if (route == XHCI_TRANSFER_EVENT_STALE) {
                XHCI_DEBUG_LOG(
                    "[xHCI-COMP] stale transfer event s%u d%u ptr=%p cc=%u\n",
                    XHCI_TRB_CTRL_SLOT_ID_GET(captured_event.control),
                    XHCI_TRB_CTRL_EP_ID_GET(captured_event.control),
                    (void *)XHCI_TRB_PTR_GET(captured_event.param1,
                                             captured_event.param2),
                    XHCI_TRB_STS_COMP_CODE_GET(captured_event.status));
            }
        } else if (event_type == XHCI_TRB_TYPE_PORT_STATUS_CHANGE) {
            xhci_handle_port_status_change(xhc, &captured_event);
        }
    }

    /* Batch update the ERDP if we processed anything */
    if (processed_any) {
        xhci_update_erdp(xhc, event_ring);
    }
    if (processed_count == XHCI_EVENT_BATCH_LIMIT) {
        bool more = xhci_event_ring_get_next(event_ring) != NULL;
        if (more)
            xhci_mark_event_work_pending(xhc);
    }
}
