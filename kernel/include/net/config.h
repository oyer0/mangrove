#pragma once

#include <types.h>

typedef struct {
    u8 octet[4];
} net_ipv4_t;

typedef struct {
    net_ipv4_t address;
    net_ipv4_t netmask;
    net_ipv4_t gateway;
    net_ipv4_t dns;
    net_ipv4_t dhcp_server;
    u32 lease_seconds;
    bool configured;
    bool has_gateway;
    bool has_dns;
} net_config_t;

void net_config_init(void);
const net_config_t *net_config(void);
bool net_config_apply_dhcp(const net_ipv4_t *address,
                           const net_ipv4_t *netmask,
                           const net_ipv4_t *gateway, bool has_gateway,
                           const net_ipv4_t *dns, bool has_dns,
                           const net_ipv4_t *server, u32 lease_seconds);
bool net_network_configured(void);
const net_ipv4_t *net_local_ipv4(void);
const net_ipv4_t *net_netmask_ipv4(void);
const net_ipv4_t *net_gateway_ipv4(void);
bool net_ipv4_equal(net_ipv4_t left, net_ipv4_t right);
