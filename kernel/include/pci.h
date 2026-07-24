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

typedef struct {
    u64 address;

    bool io;
    bool prefetchable;
    bool is_64bit;
} pci_bar_t;

void pci_init(void);

u32 pci_get_device_count(void);
const pci_device_t *pci_get_device(u32 index);

u16 pci_read_config16(const pci_device_t *device, u8 offset);

pci_bar_t pci_get_bar(const pci_device_t *device, u8 bar);
