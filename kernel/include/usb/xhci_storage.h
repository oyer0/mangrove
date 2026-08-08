#pragma once

#include <types.h>
#include <block.h>
#include <xhci.h>

typedef struct {
    xhci_controller_t *xhc;
    u8 slot_id;
    u8 bulk_in_dci;
    u8 bulk_out_dci;
    u32 block_size;
    u64 block_count;
    u32 tag;
    block_device_t block;
} usb_mass_storage_device_t;

bool xhci_storage_init_device(xhci_controller_t *xhc, u8 slot_id,
                              u8 bulk_in_ep, u8 bulk_out_ep);
