#pragma once

#include <types.h>

typedef struct {
    u8 bus;
    u8 device;
    u8 function;

    u16 vendor_id;
    u16 device_id;

    u8 class_code;
    u8 subclass;
    u8 prog_if;
    u8 revision;

    u8 header_type;
} pci_device_t;

void pci_init(void);

u32 pci_get_device_count(void);
const pci_device_t *pci_get_device(u32 index);
