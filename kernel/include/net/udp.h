#pragma once

#include <net/config.h>
#include <net/ethernet.h>
#include <net/net.h>

#define UDP_PROTOCOL 17U
#define UDP_HEADER_SIZE 8U

typedef void (*udp_handler_t)(net_device_t *device, net_ipv4_t source,
                              u16 source_port, net_ipv4_t destination,
                              u16 destination_port, const u8 *payload,
                              usize length);

void udp_init(void);
bool udp_register_handler(u16 port, udp_handler_t handler);
bool udp_unregister_handler(u16 port, udp_handler_t handler);
void udp_receive(net_device_t *device, net_ipv4_t source,
                 net_ipv4_t destination, const u8 *packet, usize length);
bool udp_transmit(net_device_t *device, net_ipv4_t source,
                  net_ipv4_t destination, u16 source_port, u16 destination_port,
                  const void *payload, usize length);
u16 udp_checksum(net_ipv4_t source, net_ipv4_t destination,
                 const void *udp_packet, usize length);
