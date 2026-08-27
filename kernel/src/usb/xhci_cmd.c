#include <xhci.h>
#include <xhci_ring.h>
#include <xhci_trb.h>
#include <xhci_regs.h>
#include <stddef.h>

extern void kprint(const char *fmt, ...);

/* ==============================================================================
 * External Dependencies
 * These will be defined in xhci.c (where the controller state lives) and 
 * xhci_event.c (which handles waiting for completion events).
 * ============================================================================== */
extern volatile u32* xhci_get_doorbell_ptr(xhci_controller_t *xhc, u8 target_idx);
extern xhci_ring_t* xhci_get_cmd_ring(xhci_controller_t *xhc);
extern xhci_status_t      xhci_wait_for_cmd_completion(xhci_controller_t *xhc, u8 trb_type, xhci_trb_t *out_event);


/* ==============================================================================
 * Internal Helper Functions
 * ============================================================================== */

/*
 * Rings a specific doorbell register.
 * Doorbell 0 is strictly for the Command Ring.
 * Doorbells 1-N correspond to Slot IDs.
 */
static void xhci_ring_doorbell(xhci_controller_t *xhc, u8 db_idx, u32 target) {
    volatile u32 *db = xhci_get_doorbell_ptr(xhc, db_idx);
    if (db) {
        *db = target;
    }
}


/* ==============================================================================
 * Standard xHCI Command Implementations
 * Each function formats a specific TRB Type, queues it to the Command Ring,
 * rings Doorbell 0, and synchronously blocks waiting for the Event Ring.
 * ============================================================================== */

/*
 * Issues the Enable Slot Command.
 * Requests a new Slot ID from the host controller.
 * * @param xhc         The controller instance.
 * @param out_slot_id Pointer to store the successfully returned Slot ID.
 * @return            XHCI_SUCCESS or standard error code.
 */
xhci_status_t xhci_cmd_enable_slot(xhci_controller_t *xhc, u8 *out_slot_id) {
    if (!xhc || !out_slot_id) return XHCI_ERR_INVALID_PARAM;

    xhci_ring_t *cmd_ring = xhci_get_cmd_ring(xhc);
    
    /* Command TRBs require no param/status fields for Enable Slot */
    u32 param1 = 0;
    u32 param2 = 0;
    u32 status = 0;
    u32 control = XHCI_TRB_CTRL_TYPE_SET(XHCI_TRB_TYPE_ENABLE_SLOT);

    uintptr_t command_trb_phys = cmd_ring->phys_base +
        cmd_ring->enqueue_idx * sizeof(xhci_trb_t);
    if (!xhci_arm_command_wait(xhc, XHCI_TRB_TYPE_ENABLE_SLOT,
                               command_trb_phys, 0))
        return XHCI_ERR_INVALID_PARAM;
    xhci_status_t err = xhci_ring_enqueue(cmd_ring, param1, param2, status, control);
    if (err != XHCI_SUCCESS) {
        xhci_cancel_command_wait(xhc);
        return err;
    }

    /* Ring Doorbell 0 (Host Controller Command Ring) */
    xhci_ring_doorbell(xhc, 0, 0);

    /* Synchronously wait for the Command Completion Event */
    xhci_trb_t event_trb = {0};
    err = xhci_wait_for_cmd_completion(xhc, XHCI_TRB_TYPE_ENABLE_SLOT, &event_trb);
    xhci_diag_command_result(XHCI_TRB_TYPE_ENABLE_SLOT, 0, err, &event_trb);
    xhci_diag_timeline_port(xhc, "enable-slot-done");
    if (err != XHCI_SUCCESS) return err;

    /* The assigned Slot ID is returned in bits [31:24] of param1 */
    *out_slot_id = (u8)(XHCI_TRB_CTRL_SLOT_ID_GET(event_trb.control));

    return XHCI_SUCCESS;
}

xhci_status_t xhci_cmd_disable_slot(xhci_controller_t *xhc, u8 slot_id)
{
    if (!xhc || slot_id == 0)
        return XHCI_ERR_INVALID_PARAM;

    xhci_ring_t *cmd_ring = xhci_get_cmd_ring(xhc);
    uintptr_t command_trb_phys = cmd_ring->phys_base +
        cmd_ring->enqueue_idx * sizeof(xhci_trb_t);
    u32 control = XHCI_TRB_CTRL_TYPE_SET(XHCI_TRB_TYPE_DISABLE_SLOT) |
                  XHCI_TRB_CTRL_SLOT_ID_SET(slot_id);
    if (!xhci_arm_command_wait(xhc, XHCI_TRB_TYPE_DISABLE_SLOT,
                               command_trb_phys, slot_id))
        return XHCI_ERR_INVALID_PARAM;

    xhci_status_t err = xhci_ring_enqueue(cmd_ring, 0, 0, 0, control);
    if (err != XHCI_SUCCESS) {
        xhci_cancel_command_wait(xhc);
        return err;
    }

    xhci_ring_doorbell(xhc, 0, 0);
    xhci_trb_t event_trb = {0};
    err = xhci_wait_for_cmd_completion(xhc, XHCI_TRB_TYPE_DISABLE_SLOT,
                                       &event_trb);
    xhci_diag_command_result(XHCI_TRB_TYPE_DISABLE_SLOT, slot_id, err,
                             &event_trb);
    return err;
}

/*
 * Issues the Address Device Command.
 * Transitions a Slot from 'Enabled' to 'Addressed' and parses the Input Context.
 * * @param xhc               The controller instance.
 * @param slot_id           The Slot ID acquired from Enable Slot.
 * @param input_ctx_phys    Physical address of the initialized Input Context.
 * @param block_set_address If true, controller evaluates context but does not send SET_ADDRESS to USB wire.
 * @return                  XHCI_SUCCESS or standard error code.
 */
xhci_status_t xhci_cmd_address_device(xhci_controller_t *xhc, u8 slot_id, uintptr_t input_ctx_phys, bool block_set_address) {
    if (!xhc || slot_id == 0) return XHCI_ERR_INVALID_PARAM;

    xhci_ring_t *cmd_ring = xhci_get_cmd_ring(xhc);
    u32 cmd_idx = cmd_ring->enqueue_idx;
    xhci_diag_address_dw1(xhc, slot_id, "before-trb");
    xhci_diag_timeline("address-before-trb", 0);

    u32 param1 = XHCI_TRB_PARAM1_PTR(input_ctx_phys);
    u32 param2 = XHCI_TRB_PARAM2_PTR(input_ctx_phys);
    u32 status = 0;
    
    u32 control = XHCI_TRB_CTRL_TYPE_SET(XHCI_TRB_TYPE_ADDRESS_DEVICE) |
                       XHCI_TRB_CTRL_SLOT_ID_SET(slot_id);
                       
    if (block_set_address) {
        control |= XHCI_TRB_CTRL_BSR;
    }

    uintptr_t command_trb_phys = cmd_ring->phys_base +
        cmd_ring->enqueue_idx * sizeof(xhci_trb_t);
    if (!xhci_arm_command_wait(xhc, XHCI_TRB_TYPE_ADDRESS_DEVICE,
                               command_trb_phys, slot_id))
        return XHCI_ERR_INVALID_PARAM;
    xhci_status_t err = xhci_ring_enqueue(cmd_ring, param1, param2, status, control);
    if (err != XHCI_SUCCESS) {
        xhci_cancel_command_wait(xhc);
        return err;
    }

    xhci_trb_t *cmd = &cmd_ring->trbs[cmd_idx];
    XHCI_DEBUG_LOG("[xHCI-ADDR-CMD] p=%08x/%08x st=%08x ctl=%08x bsr=%u idx=%u>%u\n",
                   cmd->param1, cmd->param2, cmd->status, cmd->control,
                   (cmd->control & XHCI_TRB_CTRL_BSR) != 0,
                   cmd_idx, cmd_ring->enqueue_idx);
    xhci_diag_address_dw1(xhc, slot_id, "after-enqueue");

    xhci_diag_address_dw1(xhc, slot_id, "before-doorbell");
    xhci_diag_timeline("address-before-doorbell", 0);
    xhci_ring_doorbell(xhc, 0, 0);
    xhci_diag_timeline("address-after-doorbell", 0);
    xhci_diag_address_dw1(xhc, slot_id, "after-doorbell");

    xhci_trb_t event_trb = {0};
    err = xhci_wait_for_cmd_completion(xhc, XHCI_TRB_TYPE_ADDRESS_DEVICE, &event_trb);
    xhci_diag_timeline("address-after-completion", 0);
    xhci_diag_address_dw1(xhc, slot_id, "after-completion");
    xhci_diag_command_result(XHCI_TRB_TYPE_ADDRESS_DEVICE, slot_id, err, &event_trb);
    return err;
}

/*
 * Issues the Evaluate Context Command.
 * Used to update Slot or Endpoint contexts without changing device state.
 * Specifically used in Phase 5 after reading the real EP0 Max Packet Size.
 * * @param xhc            The controller instance.
 * @param slot_id        The target Slot ID.
 * @param input_ctx_phys Physical address of the Input Context containing the updates.
 * @return               XHCI_SUCCESS or standard error code.
 */
xhci_status_t xhci_cmd_evaluate_context(xhci_controller_t *xhc, u8 slot_id, uintptr_t input_ctx_phys) {
    if (!xhc || slot_id == 0) return XHCI_ERR_INVALID_PARAM;

    xhci_ring_t *cmd_ring = xhci_get_cmd_ring(xhc);

    u32 param1 = XHCI_TRB_PARAM1_PTR(input_ctx_phys);
    u32 param2 = XHCI_TRB_PARAM2_PTR(input_ctx_phys);
    u32 status = 0;
    
    u32 control = XHCI_TRB_CTRL_TYPE_SET(XHCI_TRB_TYPE_EVAL_CONTEXT) |
                       XHCI_TRB_CTRL_SLOT_ID_SET(slot_id);

    uintptr_t command_trb_phys = cmd_ring->phys_base +
        cmd_ring->enqueue_idx * sizeof(xhci_trb_t);
    if (!xhci_arm_command_wait(xhc, XHCI_TRB_TYPE_EVAL_CONTEXT,
                               command_trb_phys, slot_id))
        return XHCI_ERR_INVALID_PARAM;
    xhci_status_t err = xhci_ring_enqueue(cmd_ring, param1, param2, status, control);
    if (err != XHCI_SUCCESS) {
        xhci_cancel_command_wait(xhc);
        return err;
    }

    xhci_ring_doorbell(xhc, 0, 0);

    xhci_trb_t event_trb = {0};
    err = xhci_wait_for_cmd_completion(xhc, XHCI_TRB_TYPE_EVAL_CONTEXT, &event_trb);
    xhci_diag_command_result(XHCI_TRB_TYPE_EVAL_CONTEXT, slot_id, err, &event_trb);
    return err;
}

/*
 * Issues the Configure Endpoint Command.
 * Activates new endpoints (like Interrupt IN for HID) based on the Input Context.
 * * @param xhc            The controller instance.
 * @param slot_id        The target Slot ID.
 * @param input_ctx_phys Physical address of the Input Context with Add/Drop flags set.
 * @return               XHCI_SUCCESS or standard error code.
 */
xhci_status_t xhci_cmd_configure_endpoint(xhci_controller_t *xhc, u8 slot_id, uintptr_t input_ctx_phys) {
    if (!xhc || slot_id == 0) return XHCI_ERR_INVALID_PARAM;

    xhci_ring_t *cmd_ring = xhci_get_cmd_ring(xhc);

    u32 param1 = XHCI_TRB_PARAM1_PTR(input_ctx_phys);
    u32 param2 = XHCI_TRB_PARAM2_PTR(input_ctx_phys);
    u32 status = 0;
    
    u32 control = XHCI_TRB_CTRL_TYPE_SET(XHCI_TRB_TYPE_CONFIG_ENDPOINT) |
                       XHCI_TRB_CTRL_SLOT_ID_SET(slot_id);

    uintptr_t command_trb_phys = cmd_ring->phys_base +
        cmd_ring->enqueue_idx * sizeof(xhci_trb_t);
    if (!xhci_arm_command_wait(xhc, XHCI_TRB_TYPE_CONFIG_ENDPOINT,
                               command_trb_phys, slot_id))
        return XHCI_ERR_INVALID_PARAM;
    xhci_status_t err = xhci_ring_enqueue(cmd_ring, param1, param2, status, control);
    if (err != XHCI_SUCCESS) {
        xhci_cancel_command_wait(xhc);
        return err;
    }

    xhci_ring_doorbell(xhc, 0, 0);

    xhci_trb_t event_trb = {0};
    err = xhci_wait_for_cmd_completion(xhc, XHCI_TRB_TYPE_CONFIG_ENDPOINT, &event_trb);
    xhci_diag_command_result(XHCI_TRB_TYPE_CONFIG_ENDPOINT, slot_id, err, &event_trb);
    return err;
}
