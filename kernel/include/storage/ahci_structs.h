#pragma once

#include <types.h>

// Frame Information Structure (FIS) Types
#define FIS_TYPE_REG_H2D 0x27

// Register Host-to-Device FIS
typedef struct PACKED
{
    u8 fis_type;

    u8 pmport : 4;
    u8 reserved0 : 3;
    u8 command : 1;

    u8 command_register;
    u8 feature_low;

    u8 lba0;
    u8 lba1;
    u8 lba2;
    u8 device;

    u8 lba3;
    u8 lba4;
    u8 lba5;
    u8 feature_high;

    u8 count_low;
    u8 count_high;

    u8 icc;
    u8 control;

    u8 reserved1[4];
} fis_reg_h2d_t;

// AHCI Command Header
typedef struct PACKED
{
    u16 cfl : 5;
    u16 atapi : 1;
    u16 write : 1;
    u16 prefetchable : 1;
    u16 reset : 1;
    u16 bist : 1;
    u16 clear_busy : 1;
    u16 reserved0 : 1;
    u16 pmp : 4;

    u16 prdtl;

    volatile u32 prdbc;

    u32 ctba;
    u32 ctbau;

    u32 reserved1[4];
} ahci_command_header_t;

// Physical Region Descriptor Table Entry
typedef struct PACKED
{
    u32 dba;
    u32 dbau;

    u32 reserved0;

    u32 dbc : 22;
    u32 reserved1 : 9;
    u32 interrupt : 1;
} ahci_prdt_entry_t;

// AHCI Command Table
typedef struct PACKED
{
    u8 command_fis[64];

    u8 atapi_command[16];

    u8 reserved[48];

    ahci_prdt_entry_t prdt_entries[1];
} ahci_command_table_t;
