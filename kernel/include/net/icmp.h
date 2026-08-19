#pragma once

#include <net/ipv4.h>

#define ICMP_TYPE_ECHO_REPLY   0U
#define ICMP_TYPE_ECHO_REQUEST 8U

void icmp_init(void);
void icmp_receive(net_device_t *device, net_ipv4_t source,
                  net_ipv4_t destination, const u8 *packet, usize length);
bool icmp_echo_request(net_device_t *device, net_ipv4_t destination,
                       u16 identifier, u16 sequence,
                       const void *payload, usize length);
bool icmp_echo_reply_received(u16 identifier, u16 sequence);
bool icmp_echo_reply_info(u16 identifier, u16 sequence, net_ipv4_t *source,
                          usize *reply_length, u64 *received_at_ms);
