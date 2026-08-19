#pragma once

#include <xhci.h>

typedef struct {
    u8 endpoint_address;
    u8 interval;
    u8 config_value;
    u8 interface_number;
    u8 interface_protocol;
    u16 max_packet_size;
    u8 max_burst;
    u8 mult;
    u16 bytes_per_interval;
    bool has_ss_companion;
} xhci_hub_endpoint_info_t;

typedef struct {
    u8 descriptor_type;
    u8 num_ports;
    u16 characteristics;
    u8 power_on_delay_2ms;
    u8 controller_current;
    u8 header_decode_latency;
    u16 hub_delay;
} xhci_hub_descriptor_info_t;

xhci_status_t xhci_get_hub_endpoint_info(
    xhci_controller_t *xhc, u8 slot_id, xhci_speed_t speed,
    xhci_hub_endpoint_info_t *out_info);

xhci_status_t xhci_hub_read_descriptor(
    xhci_controller_t *xhc, u8 slot_id, xhci_speed_t speed,
    xhci_hub_descriptor_info_t *out_descriptor);

xhci_status_t xhci_hub_enumerate_children(
    xhci_controller_t *xhc, u8 hub_slot_id, u8 root_port,
    u32 hub_route_string, u8 hub_depth, xhci_speed_t hub_speed,
    const xhci_hub_descriptor_info_t *descriptor);

/* Called only while the controller setup critical section is already held. */
xhci_status_t xhci_setup_child_device_locked(
    xhci_controller_t *xhc, u8 root_port, xhci_speed_t speed,
    u32 route_string, u8 topology_depth, u8 parent_hub_slot,
    u8 parent_port, xhci_speed_t parent_speed);

