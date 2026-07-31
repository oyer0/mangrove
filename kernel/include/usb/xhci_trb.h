#pragma once


/* ==============================================================================
 * xHCI Transfer Request Block (TRB)
 * * TRBs are the fundamental 16-byte data structures used to communicate with 
 * the xHCI controller across the Command Ring, Event Ring, and Transfer Rings.
 * * As per the xHCI specification, all TRBs follow a standard 4-DWORD layout.
 * The precise meaning of param1, param2, status, and control depends strictly
 * on the TRB Type defined in the control field.
 * ============================================================================== */

typedef struct __attribute__((packed)) {
    volatile u32 param1;  /* Parameter 1 / Lower 32 bits of 64-bit address */
    volatile u32 param2;  /* Parameter 2 / Upper 32 bits of 64-bit address */
    volatile u32 status;  /* Status (Length, Completion Codes, Interrupter target) */
    volatile u32 control; /* Control (Cycle bit, TRB Type, IOC, ISP, Flags) */
} xhci_trb_t;


/* ==============================================================================
 * TRB Types (xHCI Spec 6.4.6)
 * Assigned to Control DWORD bits [15:10]
 * ============================================================================== */

typedef enum {
    /* Transfer Ring TRBs */
    XHCI_TRB_TYPE_NORMAL             = 1,
    XHCI_TRB_TYPE_SETUP_STAGE        = 2,
    XHCI_TRB_TYPE_DATA_STAGE         = 3,
    XHCI_TRB_TYPE_STATUS_STAGE       = 4,
    XHCI_TRB_TYPE_ISOCH              = 5,
    XHCI_TRB_TYPE_LINK               = 6,
    XHCI_TRB_TYPE_EVENT_DATA         = 7,
    XHCI_TRB_TYPE_NO_OP              = 8,

    /* Command Ring TRBs */
    XHCI_TRB_TYPE_ENABLE_SLOT        = 9,
    XHCI_TRB_TYPE_DISABLE_SLOT       = 10,
    XHCI_TRB_TYPE_ADDRESS_DEVICE     = 11,
    XHCI_TRB_TYPE_CONFIG_ENDPOINT    = 12,
    XHCI_TRB_TYPE_EVAL_CONTEXT       = 13,
    XHCI_TRB_TYPE_RESET_ENDPOINT     = 14,
    XHCI_TRB_TYPE_STOP_ENDPOINT      = 15,
    XHCI_TRB_TYPE_SET_TR_DEQUEUE     = 16,
    XHCI_TRB_TYPE_RESET_DEVICE       = 17,
    XHCI_TRB_TYPE_NO_OP_CMD          = 23,

    /* Event Ring TRBs */
    XHCI_TRB_TYPE_TRANSFER_EVENT     = 32,
    XHCI_TRB_TYPE_CMD_COMPLETION     = 33,
    XHCI_TRB_TYPE_PORT_STATUS_CHANGE = 34,
    XHCI_TRB_TYPE_HOST_CTRL_EVENT    = 37
} xhci_trb_type_t;


/* ==============================================================================
 * TRB Completion Codes (xHCI Spec 6.4.5)
 * Extracted from Event TRB Status DWORD bits [31:24]
 * ============================================================================== */

typedef enum {
    XHCI_COMP_INVALID                = 0,
    XHCI_COMP_SUCCESS                = 1,
    XHCI_COMP_DATA_BUFFER_ERR        = 2,
    XHCI_COMP_BABBLE_ERR             = 3,
    XHCI_COMP_USB_TRANSACTION_ERR    = 4,
    XHCI_COMP_TRB_ERR                = 5,
    XHCI_COMP_STALL_ERR              = 6,
    XHCI_COMP_RESOURCE_ERR           = 7,
    XHCI_COMP_BANDWIDTH_ERR          = 8,
    XHCI_COMP_NO_SLOTS_ERR           = 9,
    XHCI_COMP_INVALID_STREAM_ERR     = 10,
    XHCI_COMP_SLOT_NOT_ENABLED_ERR   = 11,
    XHCI_COMP_EP_NOT_ENABLED_ERR     = 12,
    XHCI_COMP_SHORT_PACKET           = 13,
    XHCI_COMP_RING_UNDERRUN          = 14,
    XHCI_COMP_RING_OVERRUN           = 15,
    XHCI_COMP_VF_EVENT_RING_FULL     = 16,
    XHCI_COMP_PARAMETER_ERR          = 17,
    XHCI_COMP_CONTEXT_STATE_ERR      = 19,
    XHCI_COMP_EVENT_RING_FULL        = 21,
    XHCI_COMP_INCOMPATIBLE_DEVICE    = 22,
    XHCI_COMP_MISSED_SERVICE_ERR     = 23,
    XHCI_COMP_CMD_RING_STOPPED       = 24,
    XHCI_COMP_CMD_ABORTED            = 25,
    XHCI_COMP_STOPPED                = 26,
    XHCI_COMP_STOPPED_LENGTH_INVALID = 27,
    XHCI_COMP_ISOCH_BUFFER_OVERRUN   = 31,
    XHCI_COMP_EVENT_LOST_ERR         = 32
} xhci_trb_comp_code_t;


/* ==============================================================================
 * TRB Control Field Bitmasks (DWORD 3)
 * ============================================================================== */

/* Common Control Flags */
#define XHCI_TRB_CTRL_CYCLE          (1 << 0)  /* Cycle Bit (C) - Ownership tracking */
#define XHCI_TRB_CTRL_ENT            (1 << 1)  /* Evaluate Next TRB */
#define XHCI_TRB_CTRL_ISP            (1 << 2)  /* Interrupt on Short Packet */
#define XHCI_TRB_CTRL_NS             (1 << 3)  /* No Snoop */
#define XHCI_TRB_CTRL_CH             (1 << 4)  /* Chain Bit */
#define XHCI_TRB_CTRL_IOC            (1 << 5)  /* Interrupt On Completion */
#define XHCI_TRB_CTRL_IDT            (1 << 6)  /* Immediate Data */

/* Address Device Command specific */
#define XHCI_TRB_CTRL_BSR            (1 << 9)  /* Block Set Address Request */

/* Link TRB specific */
#define XHCI_TRB_CTRL_TC             (1 << 1)  /* Toggle Cycle */

/* Setup/Data Stage specific */
#define XHCI_TRB_CTRL_DIR_IN         (1 << 16) /* Direction: 0 = OUT, 1 = IN */
#define XHCI_TRB_CTRL_TRT_NO_DATA    (0 << 16) /* Transfer Type: No Data Stage */
#define XHCI_TRB_CTRL_TRT_OUT        (2 << 16) /* Transfer Type: OUT Data Stage */
#define XHCI_TRB_CTRL_TRT_IN         (3 << 16) /* Transfer Type: IN Data Stage */

/* Shift/Mask Macros for Control Fields */
#define XHCI_TRB_CTRL_TYPE_SET(x)    (((u32)(x) & 0x3F) << 10)
#define XHCI_TRB_CTRL_TYPE_GET(x)    (((u32)(x) >> 10) & 0x3F)
#define XHCI_TRB_CTRL_EP_ID_SET(x)   (((u32)(x) & 0x1F) << 16)
#define XHCI_TRB_CTRL_EP_ID_GET(x)   (((u32)(x) >> 16) & 0x1F)
#define XHCI_TRB_CTRL_SLOT_ID_SET(x) (((u32)(x) & 0xFF) << 24)
#define XHCI_TRB_CTRL_SLOT_ID_GET(x) (((u32)(x) >> 24) & 0xFF)


/* ==============================================================================
 * TRB Status Field Bitmasks (DWORD 2)
 * ============================================================================== */

/* Transfer/Command Generation (Standard Rings) */
#define XHCI_TRB_STS_XFER_LEN_SET(x) ((u32)(x) & 0x1FFFF)
#define XHCI_TRB_STS_TD_SIZE_SET(x)  (((u32)(x) & 0x1F) << 17)
#define XHCI_TRB_STS_INTR_TAR_SET(x) (((u32)(x) & 0x3FF) << 22)

/* Event Parsing (Event Ring) */
#define XHCI_TRB_STS_COMP_CODE_GET(x) (((u32)(x) >> 24) & 0xFF)
#define XHCI_TRB_STS_XFER_LEN_GET(x)  ((u32)(x) & 0xFFFFFF) /* Events use 24 bits for length/param */


/* ==============================================================================
 * Helper Macros for Common TRB Construction
 * These map perfectly to physical layouts for USB Standard Requests.
 * ============================================================================== */

/* * Builds the param1 value for a Setup Stage TRB.
 * bmRequestType (8-bit), bRequest (8-bit), wValue (16-bit)
 */
#define XHCI_SETUP_PARAM1(req_type, req, wval) \
    (((u32)(req_type) & 0xFF) | (((u32)(req) & 0xFF) << 8) | (((u32)(wval) & 0xFFFF) << 16))

/* * Builds the param2 value for a Setup Stage TRB.
 * wIndex (16-bit), wLength (16-bit)
 */
#define XHCI_SETUP_PARAM2(widx, wlen) \
    (((u32)(widx) & 0xFFFF) | (((u32)(wlen) & 0xFFFF) << 16))

/*
 * Combines two 32-bit parameters into a 64-bit physical address.
 * Used when reading Event TRBs containing physical pointers.
 */
#define XHCI_TRB_PTR_GET(param1, param2) \
    ((u64)(param1) | ((u64)(param2) << 32))

/*
 * Splits a 64-bit physical address into param1 and param2.
 * Used when queuing Command or Transfer TRBs.
 */
#define XHCI_TRB_PARAM1_PTR(ptr) ((u32)((u64)(ptr) & 0xFFFFFFFF))
#define XHCI_TRB_PARAM2_PTR(ptr) ((u32)(((u64)(ptr) >> 32) & 0xFFFFFFFF))
