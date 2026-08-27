#pragma once

#include <net/config.h>
#include <net/net.h>

typedef struct {
    net_ipv4_t address;
    net_ipv4_t netmask;
    net_ipv4_t gateway;
    net_ipv4_t dns;
    net_ipv4_t server;
    u32 lease_seconds;
    bool has_gateway;
    bool has_dns;
} dhcp_lease_t;

void dhcp_init(void);
void dhcp_reset(void);
bool dhcp_acquire(net_device_t *device, dhcp_lease_t *lease);
