#include <net/config.h>

static net_config_t configuration;

void net_config_init(void)
{
    configuration = (net_config_t){0};
}

const net_config_t *net_config(void)
{
    return &configuration;
}

bool net_config_apply_dhcp(const net_ipv4_t *address,
                           const net_ipv4_t *netmask,
                           const net_ipv4_t *gateway, bool has_gateway,
                           const net_ipv4_t *dns, bool has_dns,
                           const net_ipv4_t *server, u32 lease_seconds)
{
    if (!address || !netmask || !server) return false;
    configuration.address = *address;
    configuration.netmask = *netmask;
    configuration.gateway = gateway ? *gateway : (net_ipv4_t){{0}};
    configuration.dns = dns ? *dns : (net_ipv4_t){{0}};
    configuration.dhcp_server = *server;
    configuration.has_gateway = has_gateway && gateway != 0;
    configuration.has_dns = has_dns && dns != 0;
    configuration.lease_seconds = lease_seconds;
    configuration.configured = true;
    return true;
}

bool net_network_configured(void)
{
    return configuration.configured;
}

const net_ipv4_t *net_local_ipv4(void)
{
    return &configuration.address;
}

const net_ipv4_t *net_gateway_ipv4(void)
{
    return &configuration.gateway;
}

const net_ipv4_t *net_netmask_ipv4(void)
{
    return &configuration.netmask;
}

bool net_ipv4_equal(net_ipv4_t left, net_ipv4_t right)
{
    return left.octet[0] == right.octet[0] &&
           left.octet[1] == right.octet[1] &&
           left.octet[2] == right.octet[2] &&
           left.octet[3] == right.octet[3];
}
