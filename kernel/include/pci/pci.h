/* SPDX-License-Identifier: GPL-3.0-or-later */
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

    /* The registry keeps physical-function slots stable across a runtime
     * rescan.  A generation changes when a function disappears and a new
     * function occupies the same BDF. */
    bool present;
    u64 generation;
} pci_device_t;

typedef struct {
    u64 address;

    bool io;
    bool prefetchable;
    bool is_64bit;
} pci_bar_t;

typedef struct {
    u64 bar_address;
    u64 table_address;
    u32 table_offset;
    u16 table_size;
    u8 capability_offset;
    u8 bir;
    void *table_virt;
} pci_msix_info_t;

void pci_init(void);

/* Reconcile the bounded PCI registry with hardware without moving existing
 * entries.  Existing pointers remain valid; removed entries are returned as
 * NULL by pci_get_device(). */
bool pci_rescan(void);

u32 pci_get_device_count(void);
const pci_device_t *pci_get_device(u32 index);

u16 pci_read_config16(const pci_device_t *device, u8 offset);
u32 pci_read_config32(const pci_device_t *device, u8 offset);
u8 pci_read_config8(const pci_device_t *device, u8 offset);
void pci_write_config8(const pci_device_t *device, u8 offset, u8 value);
void pci_write_config16(const pci_device_t *device, u8 offset, u16 value);
void pci_write_config32(const pci_device_t *device, u8 offset, u32 value);
bool pci_enable_memory_busmaster(const pci_device_t *device);

/* Service native PCIe slot-change notifications from the bounded runtime
 * rescan path.  This is intentionally limited to acknowledging a physical
 * slot removal; device drivers still own their endpoint teardown. */
void pci_process_hotplug(void);

bool pci_get_msix_info(const pci_device_t *device, pci_msix_info_t *info);
bool pci_map_msix_table(pci_msix_info_t *info);
bool pci_prepare_msix_vector(const pci_device_t *device,
                             const pci_msix_info_t *info,
                             u16 entry, u8 apic_id, u8 vector);
bool pci_unmask_msix_vector(const pci_device_t *device,
                            const pci_msix_info_t *info, u16 entry);
void pci_disable_msix(const pci_device_t *device,
                      const pci_msix_info_t *info, u16 entry);

pci_bar_t pci_get_bar(const pci_device_t *device, u8 bar);
