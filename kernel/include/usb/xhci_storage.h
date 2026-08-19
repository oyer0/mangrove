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

typedef enum {
    XHCI_STORAGE_STAGE_NONE = 0,
    XHCI_STORAGE_STAGE_DMA,
    XHCI_STORAGE_STAGE_INQUIRY,
    XHCI_STORAGE_STAGE_CAPACITY,
    XHCI_STORAGE_STAGE_GEOMETRY,
    XHCI_STORAGE_STAGE_BLOCK_REGISTER,
    XHCI_STORAGE_STAGE_READY
} xhci_storage_stage_t;

typedef struct {
    xhci_storage_stage_t stage;
    bool block_registered;
    bool gpt_scan_ran;
    bool gpt_found;
} xhci_storage_probe_result_t;

bool xhci_storage_init_device(xhci_controller_t *xhc, u8 slot_id,
                              u8 bulk_in_ep, u8 bulk_out_ep,
                              xhci_storage_probe_result_t *out_result);
u32 xhci_storage_device_count(void);
