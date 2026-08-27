#pragma once

#include <net/config.h>
#include <net/net.h>

typedef enum {
    DNS_STATUS_SUCCESS = 0,
    DNS_STATUS_TIMEOUT,
    DNS_STATUS_NXDOMAIN,
    DNS_STATUS_SERVFAIL,
    DNS_STATUS_NO_ANSWER,
    DNS_STATUS_INVALID,
    DNS_STATUS_UNCONFIGURED
} dns_status_t;

void dns_init(void);
void dns_reset(void);
bool dns_encode_hostname(const char *hostname, u8 *output, usize capacity,
                         usize *encoded_length);
dns_status_t dns_parse_response(const u8 *packet, usize length, u16 query_id,
                                net_ipv4_t *address_out, u32 *answer_count);
bool dns_resolve_a(net_device_t *device, const char *hostname,
                   net_ipv4_t *address_out, dns_status_t *status_out);
