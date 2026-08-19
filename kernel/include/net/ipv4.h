#pragma once

#include <net/net.h>
#include <net/config.h>

#define IPV4_PROTOCOL_ICMP 1U
#define IPV4_PROTOCOL_UDP 17U
#define IPV4_TTL_DEFAULT   64U

void ipv4_init(void);
void ipv4_receive(net_device_t *device, const u8 *packet, usize length);
bool ipv4_transmit(net_device_t *device, net_ipv4_t destination,
                   u8 protocol, const void *payload, usize length);
bool ipv4_transmit_from(net_device_t *device, net_ipv4_t source,
                        net_ipv4_t destination, u8 protocol,
                        const void *payload, usize length);
