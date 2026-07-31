#pragma once


/* ==============================================================================
 * xHCI Capability Registers
 * Base Address: MMIO Base
 * ============================================================================== */

typedef struct __attribute__((packed)) {
    volatile u8  caplength;    /* Capability Register Length (Offset to OpRegs) */
    volatile u8  rsvd;
    volatile u16 hciversion;   /* Interface Version Number (BCD, e.g., 0x0100) */
    volatile u32 hcsparams1;   /* Structural Parameters 1 */
    volatile u32 hcsparams2;   /* Structural Parameters 2 */
    volatile u32 hcsparams3;   /* Structural Parameters 3 */
    volatile u32 hccparams1;   /* Capability Parameters 1 */
    volatile u32 dboff;        /* Doorbell Offset */
    volatile u32 rtsoff;       /* Runtime Register Space Offset */
    volatile u32 hccparams2;   /* Capability Parameters 2 */
} xhci_cap_regs_t;

/* HCSPARAMS1 Bitmasks */
#define XHCI_HCSPARAMS1_MAX_SLOTS(x)     ((x) & 0xFF)
#define XHCI_HCSPARAMS1_MAX_INTRS(x)     (((x) >> 8) & 0x7FF)
#define XHCI_HCSPARAMS1_MAX_PORTS(x)     (((x) >> 24) & 0xFF)

/* HCSPARAMS2 Bitmasks */
#define XHCI_HCSPARAMS2_ERST_MAX(x)      (((x) >> 4) & 0xF)
#define XHCI_HCSPARAMS2_MAX_SCRATCH(x)   ((((x) >> 21) & 0x1F) | (((x) >> 21) & 0x3E0)) 

/* HCCPARAMS1 Bitmasks */
#define XHCI_HCCPARAMS1_AC64             (1 << 0)  /* 64-bit Addressing Capability */
#define XHCI_HCCPARAMS1_CSZ              (1 << 2)  /* Context Size: 0 = 32-byte, 1 = 64-byte */
#define XHCI_HCCPARAMS1_XECP(x)          (((x) >> 16) & 0xFFFF) /* xHCI Extended Capabilities Pointer */


/* ==============================================================================
 * xHCI Operational Registers
 * Base Address: MMIO Base + CAPLENGTH
 * ============================================================================== */

typedef struct __attribute__((packed)) {
    volatile u32 usbcmd;       /* USB Command Register */
    volatile u32 usbsts;       /* USB Status Register */
    volatile u32 pagesize;     /* Page Size Register */
    volatile u8  rsvd1[8];
    volatile u32 dnctrl;       /* Device Notification Control */
    volatile u64 crcr;         /* Command Ring Control Register */
    volatile u8  rsvd2[16];
    volatile u64 dcbaap;       /* Device Context Base Address Array Pointer */
    volatile u32 config;       /* Configure Register */
} xhci_op_regs_t;

/* USBCMD Bitmasks */
#define XHCI_USBCMD_RS                   (1 << 0)  /* Run/Stop */
#define XHCI_USBCMD_HCRST                (1 << 1)  /* Host Controller Reset */
#define XHCI_USBCMD_INTE                 (1 << 2)  /* Interrupter Enable */
#define XHCI_USBCMD_HSEE                 (1 << 3)  /* Host System Error Enable */
#define XHCI_USBCMD_LHCRST               (1 << 7)  /* Light Host Controller Reset */
#define XHCI_USBCMD_EU3S                 (1 << 11) /* Enable U3 MFINDEX Stop */

/* USBSTS Bitmasks */
#define XHCI_USBSTS_HCH                  (1 << 0)  /* Halted */
#define XHCI_USBSTS_HSE                  (1 << 2)  /* Host System Error */
#define XHCI_USBSTS_EINT                 (1 << 3)  /* Event Interrupt */
#define XHCI_USBSTS_PCD                  (1 << 4)  /* Port Change Detect */
#define XHCI_USBSTS_SSS                  (1 << 8)  /* Save State Status */
#define XHCI_USBSTS_RSS                  (1 << 9)  /* Restore State Status */
#define XHCI_USBSTS_SRE                  (1 << 10) /* Save/Restore Error */
#define XHCI_USBSTS_CNR                  (1 << 11) /* Controller Not Ready */
#define XHCI_USBSTS_HCE                  (1 << 12) /* Host Controller Error */

/* CRCR (Command Ring Control) Bitmasks */
#define XHCI_CRCR_RCS                    (1 << 0)  /* Ring Cycle State */
#define XHCI_CRCR_CS                     (1 << 1)  /* Command Stop */
#define XHCI_CRCR_CA                     (1 << 2)  /* Command Abort */
#define XHCI_CRCR_CRR                    (1 << 3)  /* Command Ring Running */

/* CONFIG Bitmasks */
#define XHCI_CONFIG_MAX_SLOTS_EN_MASK    0xFF


/* ==============================================================================
 * xHCI Port Register Set
 * Base Address: Operational Base + 0x400 + (0x10 * (Port Num - 1))
 * ============================================================================== */

typedef struct __attribute__((packed)) {
    volatile u32 portsc;       /* Port Status and Control */
    volatile u32 portpmsc;     /* Port Power Management Status and Control */
    volatile u32 portli;       /* Port Link Info */
    volatile u32 porthlpmc;    /* Port Hardware LPM Control */
} xhci_port_regs_t;

/* PORTSC Bitmasks */
#define XHCI_PORTSC_CCS                  (1 << 0)  /* Current Connect Status */
#define XHCI_PORTSC_PED                  (1 << 1)  /* Port Enabled/Disabled */
#define XHCI_PORTSC_OCA                  (1 << 3)  /* Over-Current Active */
#define XHCI_PORTSC_PR                   (1 << 4)  /* Port Reset */
#define XHCI_PORTSC_PLS_MASK             (0xF << 5) /* Port Link State */
#define XHCI_PORTSC_PP                   (1 << 9)  /* Port Power */
#define XHCI_PORTSC_SPEED(x)             (((x) >> 10) & 0xF) /* Port Speed */
#define XHCI_PORTSC_LWS                  (1 << 16) /* Link State Write Strobe */
#define XHCI_PORTSC_CSC                  (1 << 17) /* Connect Status Change (RW1C) */
#define XHCI_PORTSC_PEC                  (1 << 18) /* Port Enabled/Disabled Change (RW1C) */
#define XHCI_PORTSC_WRC                  (1 << 19) /* Warm Port Reset Change (RW1C) */
#define XHCI_PORTSC_OCC                  (1 << 20) /* Over-Current Change (RW1C) */
#define XHCI_PORTSC_PRC                  (1 << 21) /* Port Reset Change (RW1C) */
#define XHCI_PORTSC_PLC                  (1 << 22) /* Port Link State Change (RW1C) */
#define XHCI_PORTSC_CEC                  (1 << 23) /* Port Config Error Change (RW1C) */
#define XHCI_PORTSC_CAS                  (1 << 24) /* Cold Attach Status (RW1C) */
#define XHCI_PORTSC_WCE                  (1 << 25) /* Wake on Connect Enable */
#define XHCI_PORTSC_WDE                  (1 << 26) /* Wake on Disconnect Enable */
#define XHCI_PORTSC_WOE                  (1 << 27) /* Wake on Over-Current Enable */
#define XHCI_PORTSC_DR                   (1 << 30) /* Device Removable */
#define XHCI_PORTSC_WPR                  (1 << 31) /* Warm Port Reset */

/* * CRITICAL: Read-Modify-Write Protection Mask for PORTSC.
 * These bits are Read-Write-1-to-Clear. If you read PORTSC and write the same 
 * value back without masking these, you will accidentally clear pending interrupts.
 */
#define XHCI_PORTSC_RW1C_MASK (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | \
                               XHCI_PORTSC_WRC | XHCI_PORTSC_OCC | \
                               XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | \
                               XHCI_PORTSC_CEC | XHCI_PORTSC_CAS)


/* ==============================================================================
 * xHCI Interrupter Register Set (Part of Runtime Registers)
 * Base Address: MMIO Base + RTSOFF + 0x20 + (0x20 * Interrupter Index)
 * ============================================================================== */

typedef struct __attribute__((packed)) {
    volatile u32 iman;         /* Interrupter Management */
    volatile u32 imod;         /* Interrupter Moderation */
    volatile u32 erstsz;       /* Event Ring Segment Table Size */
    volatile u32 rsvd;
    volatile u64 erstba;       /* Event Ring Segment Table Base Address */
    volatile u64 erdp;         /* Event Ring Dequeue Pointer */
} xhci_intr_regs_t;

/* IMAN Bitmasks */
#define XHCI_IMAN_IP                     (1 << 0)  /* Interrupt Pending (RW1C) */
#define XHCI_IMAN_IE                     (1 << 1)  /* Interrupt Enable */

/* ERDP Bitmasks */
#define XHCI_ERDP_DESI(x)                ((x) & 0x7) /* Dequeue ERST Segment Index */
#define XHCI_ERDP_EHB                    (1 << 3)  /* Event Handler Busy (RW1C) */


/* ==============================================================================
 * xHCI Runtime Registers Top Level
 * Base Address: MMIO Base + RTSOFF
 * ============================================================================== */

typedef struct __attribute__((packed)) {
    volatile u32 mfindex;      /* Microframe Index */
    volatile u8  rsvd[28];
    xhci_intr_regs_t  irs[1024];    /* Interrupter Register Sets (0 to MaxIntrs-1) */
} xhci_run_regs_t;


/* ==============================================================================
 * xHCI Doorbell Registers
 * Base Address: MMIO Base + DBOFF
 * Access dynamically via db_array[Target_Slot_ID]
 * ============================================================================== */

/* * Doorbell Registers are an array of 32-bit registers.
 * Index 0   = Host Controller Command Ring Doorbell.
 * Index 1-N = Device Context Doorbell (Slot ID 1 to MaxSlots).
 */
#define XHCI_DB_TARGET(x)                ((x) & 0xFF) /* DB Target (EP ID or Stream ID) */
#define XHCI_DB_STREAM_ID(x)             (((x) << 16) & 0xFFFF0000)

/* Doorbell Target Constants */
#define XHCI_DB_TARGET_HOST_CMD          0
#define XHCI_DB_TARGET_CONTROL_EP0       1
