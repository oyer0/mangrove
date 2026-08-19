#pragma once

#include <net/net.h>

#define ETHERNET_ETHERTYPE_IPV4 0x0800U
#define ETHERNET_ETHERTYPE_ARP  0x0806U
#define ETHERNET_PAYLOAD_MAX    1500U

typedef u8 net_mac_t[6];

bool net_mac_equal(const u8 left[6], const u8 right[6]);
void net_mac_copy(u8 destination[6], const u8 source[6]);
bool net_mac_is_broadcast(const u8 mac[6]);

void ethernet_init(void);
bool ethernet_transmit(net_device_t *device, const u8 destination[6],
                       u16 ethertype, const void *payload, usize length);
void ethernet_receive(net_device_t *device, const u8 *frame, usize length);
