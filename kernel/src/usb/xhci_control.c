#include <xhci.h>
#include <xhci_ring.h>
#include <xhci_trb.h>
#include <xhci_regs.h>
#include <stddef.h>

/* ==============================================================================
 * External Dependencies
 * Implemented in xhci.c (State/Accessors) and xhci_event.c (Waiting logic).
 * ============================================================================== */
extern volatile u32* xhci_get_doorbell_ptr(xhci_controller_t *xhc, u8 target_idx);
extern xhci_ring_t* xhci_get_ep_ring(xhci_controller_t *xhc, u8 slot_id, u8 dci);
extern xhci_status_t      xhci_wait_for_transfer_completion(xhci_controller_t *xhc, xhci_trb_t *out_event);


/* ==============================================================================
 * Standard USB Control Request Constants
 * ============================================================================== */
#define USB_REQ_TYPE_DIR_OUT         0x00
#define USB_REQ_TYPE_DIR_IN          0x80
#define USB_REQ_TYPE_TYPE_STANDARD   0x00
#define USB_REQ_TYPE_TYPE_CLASS      0x20
#define USB_REQ_TYPE_RECIP_DEVICE    0x00
#define USB_REQ_TYPE_RECIP_INTERFACE 0x01

#define USB_REQ_GET_DESCRIPTOR       6
#define USB_REQ_SET_CONFIGURATION    9
#define USB_REQ_SET_PROTOCOL         11

#define USB_DESC_TYPE_DEVICE         0x01
#define USB_DESC_TYPE_CONFIGURATION  0x02
#define USB_DESC_TYPE_HID_REPORT     0x22


/* ==============================================================================
 * Internal Helper Functions
 * ============================================================================== */

/*
 * Rings the doorbell for a specific Slot and Endpoint.
 * For EP0 (Control), the target DCI is always 1.
 */
static void xhci_ring_ep_doorbell(xhci_controller_t *xhc, u8 slot_id, u8 dci) {
    volatile u32 *db = xhci_get_doorbell_ptr(xhc, slot_id);
    if (db) {
        /* Write the DCI to the lower 8 bits (DB Target) */
        *db = XHCI_DB_TARGET(dci);
    }
}


/* ==============================================================================
 * Core Control Transfer Execution
 * Handles Setup, optional Data, and Status stages over the EP0 Transfer Ring.
 * ============================================================================== */

/*
 * Dispatches a standard USB Control Transfer using the strictly defined xHCI
 * 3-stage (or 2-stage) TRB sequence. Blocks until completion.
 *
 * @param xhc              The controller instance.
 * @param slot_id          The assigned Slot ID of the device.
 * @param bmRequestType    USB bmRequestType byte.
 * @param bRequest         USB bRequest byte.
 * @param wValue           USB wValue word.
 * @param wIndex           USB wIndex word.
 * @param wLength          Length of the data stage (0 for no data).
 * @param data_buffer_phys Physically contiguous DMA buffer for the data stage.
 * @return                 XHCI_SUCCESS or standard hardware error code.
 */
xhci_status_t xhci_control_transfer(xhci_controller_t *xhc, u8 slot_id,
                                    u8 bmRequestType, u8 bRequest,
                                    u16 wValue, u16 wIndex, u16 wLength,
                                    uintptr_t data_buffer_phys)
{
    if (!xhc || slot_id == 0) return XHCI_ERR_INVALID_PARAM;

    /* DCI 1 is always the Control Endpoint 0 (EP0) */
    xhci_ring_t *ep0_ring = xhci_get_ep_ring(xhc, slot_id, 1);
    if (!ep0_ring) return XHCI_ERR_INVALID_PARAM;
    u32 ring_before_enq = ep0_ring->enqueue_idx;
    u32 ring_before_deq = ep0_ring->dequeue_idx;

    u32 trt;
    u32 status_dir;

    /* Determine Transfer Type (TRT) and Status Stage Direction based on bmRequestType */
    if (wLength == 0) {
        trt = XHCI_TRB_CTRL_TRT_NO_DATA;
        status_dir = XHCI_TRB_CTRL_DIR_IN; /* No Data -> Status is IN */
    } else if (bmRequestType & USB_REQ_TYPE_DIR_IN) {
        trt = XHCI_TRB_CTRL_TRT_IN;
        status_dir = 0; /* IN Data -> Status is OUT */
    } else {
        trt = XHCI_TRB_CTRL_TRT_OUT;
        status_dir = XHCI_TRB_CTRL_DIR_IN; /* OUT Data -> Status is IN */
    }

    /* --------------------------------------------------------------------------
     * 1. Setup Stage TRB
     * -------------------------------------------------------------------------- */
    u32 setup_p1 = XHCI_SETUP_PARAM1(bmRequestType, bRequest, wValue);
    u32 setup_p2 = XHCI_SETUP_PARAM2(wIndex, wLength);
    u32 setup_sts = XHCI_TRB_STS_XFER_LEN_SET(8); /* Setup packet is always 8 bytes */
    
    /* IDT (Immediate Data) must be set since the setup packet is embedded in
       param1/2.  A Setup Stage is a complete, single-TRB TD, so CH is clear. */
    u32 setup_ctrl = XHCI_TRB_CTRL_TYPE_SET(XHCI_TRB_TYPE_SETUP_STAGE) | 
                          trt | XHCI_TRB_CTRL_IDT;

    xhci_status_t err = xhci_ring_enqueue(ep0_ring, setup_p1, setup_p2, setup_sts, setup_ctrl);
    if (err != XHCI_SUCCESS) return err;

    /* --------------------------------------------------------------------------
     * 2. Data Stage TRB (If required)
     * -------------------------------------------------------------------------- */
    if (wLength > 0) {
        u32 data_p1 = XHCI_TRB_PARAM1_PTR(data_buffer_phys);
        u32 data_p2 = XHCI_TRB_PARAM2_PTR(data_buffer_phys);
        u32 data_sts = XHCI_TRB_STS_XFER_LEN_SET(wLength);
        
        /* This buffer completes the Data Stage TD, so CH is clear.  ISP allows
           graceful completion if the device returns a shorter descriptor. */
        u32 data_ctrl = XHCI_TRB_CTRL_TYPE_SET(XHCI_TRB_TYPE_DATA_STAGE) | 
                             XHCI_TRB_CTRL_ISP;

        if (bmRequestType & USB_REQ_TYPE_DIR_IN) {
            data_ctrl |= XHCI_TRB_CTRL_DIR_IN;
        }

        err = xhci_ring_enqueue(ep0_ring, data_p1, data_p2, data_sts, data_ctrl);
        if (err != XHCI_SUCCESS) return err;
    }

    /* --------------------------------------------------------------------------
     * 3. Status Stage TRB
     * -------------------------------------------------------------------------- */
    u32 status_p1 = 0;
    u32 status_p2 = 0;
    u32 status_sts = 0;
    
    /* IOC (Interrupt On Completion) ensures the Event Ring fires when the sequence is done.
       CH (Chain) is 0 because this terminates the command chain. */
    u32 status_ctrl = XHCI_TRB_CTRL_TYPE_SET(XHCI_TRB_TYPE_STATUS_STAGE) | 
                           status_dir | XHCI_TRB_CTRL_IOC;

    err = xhci_ring_enqueue(ep0_ring, status_p1, status_p2, status_sts, status_ctrl);
    if (err != XHCI_SUCCESS) return err;

    /* Strike the doorbell for EP0 (Target DCI = 1) */
    xhci_ring_ep_doorbell(xhc, slot_id, 1);

    /* Synchronously await the Transfer Event TRB */
    xhci_trb_t event_trb = {0};
    xhci_status_t result = xhci_wait_for_transfer_completion(xhc, &event_trb);
    xhci_diag_control_result(xhc, slot_id, bmRequestType, bRequest, wValue,
                             wIndex, wLength, setup_ctrl, status_ctrl,
                             ring_before_enq, ring_before_deq,
                             ep0_ring->enqueue_idx, ep0_ring->dequeue_idx,
                             result, &event_trb);
    return result;
}


/* ==============================================================================
 * High-Level USB Control Operations (Phase 5 & 6)
 * ============================================================================== */

/*
 * Retrieves a descriptor from the device (Device, Configuration, String, etc).
 * Used during Phase 5 to parse max packet sizes and HID endpoint parameters.
 */
xhci_status_t xhci_control_get_descriptor(xhci_controller_t *xhc, u8 slot_id, 
                                          u8 desc_type, u8 desc_index, 
                                          u16 length, uintptr_t buffer_phys) 
{
    u8 req_type = USB_REQ_TYPE_DIR_IN | USB_REQ_TYPE_TYPE_STANDARD | USB_REQ_TYPE_RECIP_DEVICE;
    u16 wValue = (u16)((desc_type << 8) | desc_index);
    
    return xhci_control_transfer(xhc, slot_id, req_type, USB_REQ_GET_DESCRIPTOR, 
                                 wValue, 0, length, buffer_phys);
}

/*
 * Sets the active configuration of the device.
 * Used during Phase 6 to move the device into the configured state.
 */
xhci_status_t xhci_control_set_configuration(xhci_controller_t *xhc, u8 slot_id, u8 config_value) 
{
    u8 req_type = USB_REQ_TYPE_DIR_OUT | USB_REQ_TYPE_TYPE_STANDARD | USB_REQ_TYPE_RECIP_DEVICE;
    
    return xhci_control_transfer(xhc, slot_id, req_type, USB_REQ_SET_CONFIGURATION, 
                                 config_value, 0, 0, 0);
}

/*
 * Forces a HID device into the Boot Protocol.
 * Mandatory for Phase 6 to ensure the keyboard outputs fixed 8-byte reports 
 * instead of complex variable-length HID reports, removing the need for a full HID parser.
 */
xhci_status_t xhci_control_set_protocol(xhci_controller_t *xhc, u8 slot_id, u8 interface_index, u8 protocol) 
{
    /* 0x21: OUT, Class, Interface */
    u8 req_type = USB_REQ_TYPE_DIR_OUT | USB_REQ_TYPE_TYPE_CLASS | USB_REQ_TYPE_RECIP_INTERFACE;
    
    return xhci_control_transfer(xhc, slot_id, req_type, USB_REQ_SET_PROTOCOL, 
                                 protocol, interface_index, 0, 0);
}
