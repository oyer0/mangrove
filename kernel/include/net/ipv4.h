/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <net/net.h>
#include <net/config.h>

#define IPV4_PROTOCOL_ICMP 1U
#define IPV4_PROTOCOL_UDP 17U
#define IPV4_TTL_DEFAULT   64U

typedef enum {
    IPV4_SEND_FAILED = 0,
    IPV4_SEND_SUBMITTED,
    IPV4_SEND_NEIGHBOR_PENDING,
} ipv4_send_result_t;

void ipv4_init(void);
void ipv4_receive(net_device_t *device, const u8 *packet, usize length);
ipv4_send_result_t ipv4_send(net_device_t *device, net_ipv4_t destination,
                             u8 protocol, const void *payload, usize length);
ipv4_send_result_t ipv4_send_from(net_device_t *device, net_ipv4_t source,
                                  net_ipv4_t destination, u8 protocol,
                                  const void *payload, usize length);
bool ipv4_neighbor_resolved(net_ipv4_t destination);
bool ipv4_transmit(net_device_t *device, net_ipv4_t destination,
                   u8 protocol, const void *payload, usize length);
bool ipv4_transmit_from(net_device_t *device, net_ipv4_t source,
                        net_ipv4_t destination, u8 protocol,
                        const void *payload, usize length);
