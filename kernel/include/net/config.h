#pragma once

#include <types.h>

typedef struct {
    u8 octet[4];
} net_ipv4_t;

typedef enum {
    NET_CONFIG_MODE_NONE = 0,
    NET_CONFIG_MODE_DHCP = 1,
    NET_CONFIG_MODE_MANUAL = 2,
} net_config_mode_t;

typedef enum {
    NET_DHCP_STATE_NONE = 0,
    NET_DHCP_STATE_ACQUIRING,
    NET_DHCP_STATE_BOUND,
    NET_DHCP_STATE_RENEWING,
    NET_DHCP_STATE_REBINDING,
} net_dhcp_state_t;

typedef struct {
    net_ipv4_t address;
    net_ipv4_t netmask;
    net_ipv4_t gateway;
    net_ipv4_t dns;
    net_ipv4_t dhcp_server;
    u32 lease_seconds;
    u8 prefix_length;
    net_config_mode_t mode;
    bool configured;
    bool has_gateway;
    bool has_dns;
    net_dhcp_state_t dhcp_state;
    u32 renewal_seconds;
    u32 rebinding_seconds;
    u64 lease_acquired_ms;
    u64 renewal_deadline_ms;
    u64 rebinding_deadline_ms;
    u64 expiry_deadline_ms;
    u64 retry_deadline_ms;
    u32 retry_delay_ms;
    char interface_name[16];
} net_config_t;

typedef struct {
    net_config_mode_t mode;
    char interface_name[16];
    net_ipv4_t address;
    u8 prefix_length;
    net_ipv4_t gateway;
    net_ipv4_t dns;
} net_persistent_config_t;

void net_config_init(void);
const net_config_t *net_config(void);
bool net_config_apply_dhcp(const net_ipv4_t *address,
                           const net_ipv4_t *netmask,
                           const net_ipv4_t *gateway, bool has_gateway,
                           const net_ipv4_t *dns, bool has_dns,
                           const net_ipv4_t *server, u32 lease_seconds,
                           u32 renewal_seconds, u32 rebinding_seconds);
void net_config_begin_dhcp(void);
void net_config_mark_dhcp_attempt(bool rebinding);
void net_config_mark_dhcp_failure(void);
bool net_config_apply_manual(const net_ipv4_t *address, u8 prefix_length,
                             const net_ipv4_t *gateway,
                             const net_ipv4_t *dns);
void net_config_clear(void);
bool net_config_load_persistent(net_persistent_config_t *output,
                                const char **reason);
bool net_network_configured(void);
const net_ipv4_t *net_local_ipv4(void);
const net_ipv4_t *net_netmask_ipv4(void);
const net_ipv4_t *net_gateway_ipv4(void);
bool net_ipv4_equal(net_ipv4_t left, net_ipv4_t right);
