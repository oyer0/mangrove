#pragma once

#include <net/config.h>
#include <net/net.h>

#define ARP_CACHE_CAPACITY 16U

void arp_init(void);
void arp_receive(net_device_t *device, const u8 *packet, usize length);
bool arp_lookup(const net_ipv4_t *address, u8 mac_out[6]);
bool arp_request(net_device_t *device, net_ipv4_t address);
typedef struct { net_ipv4_t address; u8 mac[6]; bool valid; } arp_snapshot_entry_t;
usize arp_snapshot(arp_snapshot_entry_t *entries, usize capacity);
