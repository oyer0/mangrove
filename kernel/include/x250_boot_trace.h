/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <bootinfo.h>

#ifndef X250_BOOT_TRACE
#define X250_BOOT_TRACE 0
#endif

#define X250_TRACE_GPT_USB_SCAN_REPORTED (-2147483647)

/* Waits and filesystem phases outside the USB backend's measured callback.
 * Bounded snapshots are used only by opt-in boot-log diagnostics. */
typedef enum {
    X250_BOOT_TRACE_MEASURE_BLOCK_IO_GATE_ACQUIRE = 0,
    X250_BOOT_TRACE_MEASURE_VFS_OPERATION_GUARD_ACQUIRE,
    X250_BOOT_TRACE_MEASURE_VFS_OPERATION_LOCK_ACQUIRE,
    X250_BOOT_TRACE_MEASURE_RECOVERY_PIN_HANDOFF,
    X250_BOOT_TRACE_MEASURE_MGFS_ORDINARY_BEGIN,
    X250_BOOT_TRACE_MEASURE_MGFS_MUTATION,
    X250_BOOT_TRACE_MEASURE_MGFS_ORDINARY_COMPLETE,
    X250_BOOT_TRACE_MEASURE_COUNT
} x250_boot_trace_measurement_kind_t;

typedef struct {
    u64 count;
    u64 elapsed_ms;
    u64 maximum_ms;
} x250_boot_trace_measurement_stats_t;

typedef struct {
    x250_boot_trace_measurement_stats_t kinds[X250_BOOT_TRACE_MEASURE_COUNT];
} x250_boot_trace_measurement_snapshot_t;

struct cpu_registers;
struct xhci_storage_trace_snapshot;

#if X250_BOOT_TRACE
#define X250_BOOT_TRACE_HISTORY_CAPACITY 128U
#define X250_BOOT_TRACE_TEXT_CAPACITY 192U
#define X250_BOOT_TRACE_EXPORT_CAPACITY \
    (X250_BOOT_TRACE_HISTORY_CAPACITY * X250_BOOT_TRACE_TEXT_CAPACITY)
void x250_boot_trace_begin(void);
void x250_boot_trace_refresh(void);
usize x250_boot_trace_export(char *buffer, usize capacity, u32 *line_count,
                             bool *complete);
void x250_boot_trace_measurements_begin(void);
void x250_boot_trace_measurement_note(
    x250_boot_trace_measurement_kind_t kind, u64 elapsed_ms);
void x250_boot_trace_measurements_end(
    x250_boot_trace_measurement_snapshot_t *snapshot);
void x250_boot_trace_ports_stage(const char *stage, u64 ms, u32 command,
                                  u32 status, u32 connected, u32 powered);
void x250_boot_trace_ports_raw(u32 caplength, u64 first_physical,
                                const u32 *routed, const u32 *started,
                                const u32 *final, u32 count);
void x250_boot_trace_protocol(u32 major, u32 minor, u32 first,
                               u32 count, u32 slot_type);
void x250_boot_trace_routing(bool passed, u32 usb2_mask, u32 usb2_before,
                              u32 usb2_after, u32 usb3_mask,
                              u32 usb3_before, u32 usb3_after);
void x250_boot_trace_pci(u32 bus, u32 device, u32 function,
                          u32 vendor, u32 product, u32 interface, u32 command,
                          u32 d0, u32 d4, u32 d8, u32 dc);
void x250_boot_trace_event(BOOT_INFO *boot_info, u32 checkpoint,
                           bool passed, const char *description);
void x250_boot_trace_stage(u32 checkpoint, bool passed,
                           const char *description);
void x250_boot_trace_timing(u64 uptime_ms, const char *event, u32 port,
                            bool duration_valid, u64 duration_ms,
                            const char *state);
void x250_boot_trace_timing_value(u64 uptime_ms, const char *event,
                                  const char *value_name, u64 value,
                                  bool duration_valid, u64 duration_ms,
                                  const char *state);
void x250_boot_trace_storage_io(u64 uptime_ms, const char *phase,
                                const struct xhci_storage_trace_snapshot *io);
void x250_boot_trace_result(u32 checkpoint, bool passed,
                            const char *description, bool required,
                            bool status_available, int status,
                            const char *state, const char *reason);
void x250_boot_trace_storage_inventory(u32 block_devices,
                                       u32 usb_mass_storage_devices,
                                       bool xhci_ready);
void x250_boot_trace_xhci_summary(u32 ports, u32 connected,
                                  u32 enumerated, u32 failed,
                                  u32 usb_mass_storage);
void x250_boot_trace_xhci_port(u32 port, u32 portsc, const char *state,
                               bool result_available, int result,
                               const char *reason);
void x250_boot_trace_usb_device(u32 port, u32 slot, u32 speed,
                                u32 vendor_id, u32 product_id,
                                u32 class_code, u32 subclass,
                                u32 protocol, u8 class_flags,
                                bool mass_storage_endpoints_found,
                                const char *probe_stage, int probe_result);
void x250_boot_trace_msc_probe(u32 port, u32 slot, const char *stage,
                               int result, bool bot_ready,
                               bool capacity_known, bool block_registered,
                               bool gpt_scan_ran, bool gpt_scan_succeeded,
                               u32 gpt_partitions, u32 sector_size,
                               u64 sectors, const char *model);
void x250_boot_trace_root_inventory(u32 block_devices, u32 whole_disks,
                                    u32 partition_devices,
                                    u32 root_candidates);
void x250_boot_trace_block_device(u32 index, const char *type,
                                  u32 sector_size, u64 sectors,
                                  const char *model, int gpt_result,
                                  u32 gpt_type, u32 gpt_partitions,
                                  u32 registered_partitions,
                                  u32 root_candidates);
void x250_boot_trace_rebind(void *framebuffer, u32 width, u32 height,
                            u32 stride);
void x250_boot_trace_use_framebuffer_api(void);
void x250_boot_trace_panic(const char *message,
                           const struct cpu_registers *regs,
                           u64 cr2, u64 cr3);
#else
#define x250_boot_trace_begin(...) ((void)0)
#define x250_boot_trace_refresh(...) ((void)0)
#define x250_boot_trace_export(...) 0U
#define x250_boot_trace_measurements_begin(...) ((void)0)
#define x250_boot_trace_measurement_note(...) ((void)0)
#define x250_boot_trace_measurements_end(...) ((void)0)
#define x250_boot_trace_routing(...) ((void)0)
#define x250_boot_trace_pci(...) ((void)0)
#define x250_boot_trace_event(...) ((void)0)
#define x250_boot_trace_result(...) ((void)0)
#define x250_boot_trace_storage_inventory(...) ((void)0)
#define x250_boot_trace_xhci_summary(...) ((void)0)
#define x250_boot_trace_xhci_port(...) ((void)0)
#define x250_boot_trace_usb_device(...) ((void)0)
#define x250_boot_trace_msc_probe(...) ((void)0)
#define x250_boot_trace_root_inventory(...) ((void)0)
#define x250_boot_trace_block_device(...) ((void)0)
#define x250_boot_trace_stage(...) ((void)0)
#define x250_boot_trace_timing(...) ((void)0)
#define x250_boot_trace_timing_value(...) ((void)0)
#define x250_boot_trace_storage_io(...) ((void)0)
#define x250_boot_trace_rebind(...) ((void)0)
#define x250_boot_trace_use_framebuffer_api(...) ((void)0)
#define x250_boot_trace_panic(...) ((void)0)
#endif
