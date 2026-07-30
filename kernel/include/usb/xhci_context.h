#pragma once


/* ==============================================================================
 * xHCI Context Data Structures
 * * Contexts are heavily structured DMA buffers used by the xHC to maintain 
 * the state of Slots (Devices) and Endpoints. 
 * * IMPORTANT (CSZ Note): The xHCI spec supports both 32-byte and 64-byte 
 * context structures depending on the HCCPARAMS1.CSZ bit. These structures 
 * represent the foundational 32-byte layout. If CSZ=1, each of these blocks 
 * must be padded with an additional 32 bytes of reserved space when 
 * constructed in memory.
 * ============================================================================== */

/* ==============================================================================
 * Slot Context (xHCI Spec 6.2.2)
 * Maintains the state of a logical device (Slot) attached to the host.
 * ============================================================================== */

typedef struct __attribute__((packed)) {
    volatile u32 info1;    /* Route String, Speed, MTT, Hub, Context Entries */
    volatile u32 info2;    /* Max Exit Latency, Root Hub Port Number, Number of Ports */
    volatile u32 info3;    /* Parent Hub Slot ID, Parent Port Number, TTT, Interrupter Target */
    volatile u32 info4;    /* USB Device Address, Slot State */
    volatile u32 rsvd[4];  /* Reserved for 32-byte alignment */
} xhci_slot_context_t;

/* Slot Context info1 masks */
#define XHCI_SLOT_CTX_ROUTE_STRING_SET(x)  ((u32)(x) & 0xFFFFF)
#define XHCI_SLOT_CTX_SPEED_SET(x)         (((u32)(x) & 0xF) << 20)
#define XHCI_SLOT_CTX_SPEED_GET(x)         (((u32)(x) >> 20) & 0xF)
#define XHCI_SLOT_CTX_MTT                  (1 << 25) /* Multi-TT */
#define XHCI_SLOT_CTX_HUB                  (1 << 26) /* Is a Hub */
#define XHCI_SLOT_CTX_ENTRIES_SET(x)       (((u32)(x) & 0x1F) << 27) /* Index of highest active EP */

/* Slot Context info2 masks */
#define XHCI_SLOT_CTX_MAX_EXIT_LAT_SET(x)  ((u32)(x) & 0xFFFF)
#define XHCI_SLOT_CTX_ROOT_HUB_PORT_SET(x) (((u32)(x) & 0xFF) << 16)
#define XHCI_SLOT_CTX_NUM_PORTS_SET(x)     (((u32)(x) & 0xFF) << 24)

/* Slot Context info3 masks */
#define XHCI_SLOT_CTX_PARENT_HUB_ID_SET(x) ((u32)(x) & 0xFF)
#define XHCI_SLOT_CTX_PARENT_PORT_SET(x)   (((u32)(x) & 0xFF) << 8)
#define XHCI_SLOT_CTX_TTT_SET(x)           (((u32)(x) & 0x3) << 16) /* TT Think Time */
#define XHCI_SLOT_CTX_INTR_TARGET_SET(x)   (((u32)(x) & 0x3FF) << 22)

/* Slot Context info4 masks */
#define XHCI_SLOT_CTX_DEV_ADDR_GET(x)      ((u32)(x) & 0xFF)
#define XHCI_SLOT_CTX_SLOT_STATE_GET(x)    (((u32)(x) >> 27) & 0x1F)

/* Slot States */
typedef enum {
    XHCI_SLOT_STATE_DISABLED_ENABLED = 0, /* Disabled / Enabled State */
    XHCI_SLOT_STATE_DEFAULT          = 1,
    XHCI_SLOT_STATE_ADDRESSED        = 2,
    XHCI_SLOT_STATE_CONFIGURED       = 3
} xhci_slot_state_t;


/* ==============================================================================
 * Endpoint Context (xHCI Spec 6.2.3)
 * Maintains the state of a single unidirectional USB Endpoint.
 * Endpoint 0 is a special bidirectional Control Endpoint.
 * ============================================================================== */

typedef struct __attribute__((packed)) {
    volatile u32 info1;    /* EP State, Mult, Max PStreams, Interval, Max ESIT Payload High */
    volatile u32 info2;    /* Error Count (CErr), EP Type, HID, Max Burst Size, Max Packet Size */
    volatile u32 tr_dq_lo; /* Transfer Ring Dequeue Pointer Lo (Bits 3:1 Rsvd, Bit 0 DCS) */
    volatile u32 tr_dq_hi; /* Transfer Ring Dequeue Pointer Hi */
    volatile u32 info3;    /* Average TRB Length, Max ESIT Payload Lo */
    volatile u32 rsvd[3];  /* Reserved for 32-byte alignment */
} xhci_ep_context_t;

/* EP Context info1 masks */
#define XHCI_EP_CTX_STATE_GET(x)           ((u32)(x) & 0x7)
#define XHCI_EP_CTX_MULT_SET(x)            (((u32)(x) & 0x3) << 8)
#define XHCI_EP_CTX_MAX_PSTREAMS_SET(x)    (((u32)(x) & 0x1F) << 10)
#define XHCI_EP_CTX_LSA                    (1 << 15) /* Linear Stream Array */
#define XHCI_EP_CTX_INTERVAL_SET(x)        (((u32)(x) & 0xFF) << 16)
#define XHCI_EP_CTX_MAX_ESIT_HI_SET(x)     (((u32)(x) & 0xFF) << 24)

/* EP Context info2 masks */
#define XHCI_EP_CTX_CERR_SET(x)            (((u32)(x) & 0x3) << 1)
#define XHCI_EP_CTX_TYPE_SET(x)            (((u32)(x) & 0x7) << 3)
#define XHCI_EP_CTX_TYPE_GET(x)            (((u32)(x) >> 3) & 0x7)
#define XHCI_EP_CTX_HID                    (1 << 7)  /* Host Initiate Disable */
#define XHCI_EP_CTX_MAX_BURST_SET(x)       (((u32)(x) & 0xFF) << 8)
#define XHCI_EP_CTX_MAX_PACKET_SET(x)      (((u32)(x) & 0xFFFF) << 16)
#define XHCI_EP_CTX_MAX_PACKET_GET(x)      (((u32)(x) >> 16) & 0xFFFF)

/* EP Context tr_dq_lo masks */
#define XHCI_EP_CTX_TR_DQ_DCS              (1 << 0)  /* Dequeue Cycle State */

/* EP Context info3 masks */
#define XHCI_EP_CTX_AVG_TRB_LEN_SET(x)     ((u32)(x) & 0xFFFF)
#define XHCI_EP_CTX_MAX_ESIT_LO_SET(x)     (((u32)(x) & 0xFFFF) << 16)

/* Endpoint States */
typedef enum {
    XHCI_EP_STATE_DISABLED           = 0,
    XHCI_EP_STATE_RUNNING            = 1,
    XHCI_EP_STATE_HALTED             = 2,
    XHCI_EP_STATE_STOPPED            = 3,
    XHCI_EP_STATE_ERROR              = 4
} xhci_ep_state_t;

/* Endpoint Types */
typedef enum {
    XHCI_EP_TYPE_INVALID             = 0,
    XHCI_EP_TYPE_ISOCH_OUT           = 1,
    XHCI_EP_TYPE_BULK_OUT            = 2,
    XHCI_EP_TYPE_INTR_OUT            = 3,
    XHCI_EP_TYPE_CONTROL             = 4,
    XHCI_EP_TYPE_ISOCH_IN            = 5,
    XHCI_EP_TYPE_BULK_IN             = 6,
    XHCI_EP_TYPE_INTR_IN             = 7
} xhci_ep_type_t;


/* ==============================================================================
 * Input Control Context (xHCI Spec 6.2.5.1)
 * Used as the first 32 bytes of the Input Context array. Dictates which 
 * subsequent contexts (Slot or EPs) should be evaluated by commands like 
 * Address Device, Configure Endpoint, or Evaluate Context.
 * ============================================================================== */

typedef struct __attribute__((packed)) {
    volatile u32 drop_flags; /* Drop Context Flags (EP0 is bit 1, EP1 is bit 2, etc.) */
    volatile u32 add_flags;  /* Add Context Flags (Slot is bit 0, EP0 is bit 1, etc.) */
    volatile u32 rsvd[6];    /* Reserved for 32-byte alignment */
} xhci_input_control_context_t;

/*
 * Bit mapping for Input Control Context Drop/Add Flags.
 * - Bit 0 applies to the Slot Context.
 * - Bits 1 to 31 apply to Endpoints 0 to 30.
 * * Formula to calculate EP bit:
 * DCI (Device Context Index) = (Endpoint Number * 2) + Direction
 * Direction = 0 for OUT/Control, 1 for IN
 * * e.g., EP 1 IN -> (1 * 2) + 1 = 3 (Bit 3).
 */
#define XHCI_CTX_FLAG_SLOT                 (1 << 0)
#define XHCI_CTX_FLAG_EP(dci)              (1 << (dci))
