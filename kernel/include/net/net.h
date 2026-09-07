/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

#define NET_ETHERNET_HEADER_SIZE 14U
#define NET_ETHERNET_MIN_FRAME    60U
#define NET_ETHERNET_MAX_FRAME  1518U
#define NET_DEVICE_ID_BASE      0x3000000000000000ULL
#define NET_MAX_DEVICES         4U

typedef struct net_device net_device_t;
/* Invoked synchronously by a driver while its receive descriptor is owned by
 * the kernel.  A handler must copy data it needs after returning. */
typedef void (*net_receive_handler_t)(net_device_t *device,
                                      const u8 *frame, usize length);

struct net_device {
    u64 id;
    u64 generation;
    const char *name;
    u8 mac[6];
    usize mtu;
    bool (*transmit)(net_device_t *device, const void *frame, usize length);
    void *driver_data;
    bool administrative_enabled;
    bool link_known;
    bool link_up;
};

void net_init(void);
bool net_register_device(net_device_t *device);
net_device_t *net_primary_device(void);
u32 net_device_count(void);
net_device_t *net_device_at(u32 index);
bool net_unregister_device(net_device_t *device);
bool net_device_current(const net_device_t *device);
bool net_device_instance_current(const net_device_t *device, u64 generation);
bool net_device_enabled(const net_device_t *device);
bool net_set_device_enabled(net_device_t *device, bool enabled);
bool net_device_set_link(net_device_t *device, bool known, bool up);
bool netdev_transmit(net_device_t *device, const void *frame, usize length);
void net_set_receive_handler(net_receive_handler_t handler);
void net_receive_frame(net_device_t *device, const u8 *frame, usize length);
u64 net_received_frames(void);
u64 net_received_bytes(void);
u64 net_transmitted_frames(void);
bool net_fill_interface(void *output, usize capacity, usize *count);
bool net_fill_routes(void *output, usize capacity, usize *count);
