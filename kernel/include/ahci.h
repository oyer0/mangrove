#pragma once

#include <types.h>

// Limits
#define AHCI_MAX_PORTS 32U

// Global Host Control
#define AHCI_GHC_HR (1U << 0)
#define AHCI_GHC_AE (1U << 31)

// SATA Status Register
#define AHCI_SSTS_DET_PRESENT 3U
#define AHCI_SSTS_IPM_ACTIVE  1U

// Device Signatures
#define AHCI_SIG_ATA     0x00000101
#define AHCI_SIG_ATAPI   0xEB140101
#define AHCI_SIG_SEMB    0xC33C0101
#define AHCI_SIG_PM      0x96690101

// Port Command Register
#define AHCI_PORT_CMD_ST   (1U << 0)
#define AHCI_PORT_CMD_FRE  (1U << 4)
#define AHCI_PORT_CMD_FR   (1U << 14)
#define AHCI_PORT_CMD_CR   (1U << 15)

// AHCI Port Registers
typedef struct
{
    u32 clb;
    u32 clbu;
    u32 fb;
    u32 fbu;
    u32 is;
    u32 ie;
    u32 cmd;
    u32 reserved0;
    u32 tfd;
    u32 sig;
    u32 ssts;
    u32 sctl;
    u32 serr;
    u32 sact;
    u32 ci;
    u32 sntf;
    u32 fbs;
    u32 reserved1[11];
    u32 vendor[4];
} ahci_port_registers_t;

// AHCI Host Bus Adapter Registers
typedef struct
{
    u32 cap;
    u32 ghc;
    u32 is;
    u32 pi;
    u32 vs;

    u32 ccc_ctl;
    u32 ccc_pts;
    u32 em_loc;
    u32 em_ctl;
    u32 cap2;
    u32 bohc;

    // 0x2C - 0x9F: Reserved
    u8 reserved[0xA0 - 0x2C];
    
    // 0xA0 - 0xFF: Vendor specific registers
    u8 vendor_regs[0x100 - 0xA0];

    // 0x100 onwards: Port control registers
    ahci_port_registers_t ports[32];
} ahci_hba_registers_t;

// Public API
void ahci_init(void);
void ahci_port_init(u8 port_number);
bool ahci_present(void);

u64 ahci_base(void);
u32 ahci_capabilities(void);
u32 ahci_version(void);
u32 ahci_ports_implemented(void);

bool ahci_port_present(u8 port);
bool ahci_port_implemented(u8 port);
