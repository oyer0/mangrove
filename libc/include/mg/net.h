#pragma once

#include <mg/error.h>

/* IPv4 octets are stored in wire/display order, never host-endian integers. */
typedef struct {
    u8 octet[4];
} mg_ipv4_addr_t;

typedef struct {
    mg_ipv4_addr_t address;
    u16 port;
    u16 reserved;
} mg_net_endpoint_t;

typedef struct {
    bool configured;
    u8 mode;
    u8 prefix_length;
    u8 reserved[5];
    mg_ipv4_addr_t address;
    mg_ipv4_addr_t netmask;
    mg_ipv4_addr_t gateway;
    mg_ipv4_addr_t dns;
} mg_net_info_t;

enum {
    MG_NET_MODE_NONE = 0,
    MG_NET_MODE_DHCP = 1,
    MG_NET_MODE_MANUAL = 2,
};

typedef struct {
    mg_ipv4_addr_t address;
    u8 prefix_length;
    u8 reserved[3];
    mg_ipv4_addr_t gateway;
    mg_ipv4_addr_t dns;
} mg_net_manual_config_t;

typedef struct {
    mg_ipv4_addr_t source;
    u16 sequence;
    u16 reply_length;
    u64 rtt_ms;
} mg_icmp_echo_result_t;

typedef struct {
    mg_net_endpoint_t source;
    usize length;
} mg_datagram_result_t;

#define MG_NET_NAME_MAX 16U
#define MG_NET_TYPE_MAX 16U
#define MG_NET_CONNECTION_STATE_MAX 16U

typedef struct {
    char name[MG_NET_NAME_MAX];
    char type[MG_NET_TYPE_MAX];
    bool link_up;
    u8 mac[6];
    u8 reserved[1];
    u32 mtu;
    mg_ipv4_addr_t address;
    mg_ipv4_addr_t netmask;
    u64 rx_packets;
    u64 tx_packets;
} mg_net_interface_info_t;

typedef struct {
    mg_ipv4_addr_t destination;
    mg_ipv4_addr_t netmask;
    mg_ipv4_addr_t gateway;
    char interface_name[MG_NET_NAME_MAX];
    bool is_default;
    u8 reserved[3];
} mg_net_route_info_t;

typedef struct {
    mg_ipv4_addr_t address;
    u8 mac[6];
    char interface_name[MG_NET_NAME_MAX];
    u8 state;
    u8 reserved[3];
} mg_net_neighbor_info_t;

typedef struct {
    u8 protocol; /* 6 = TCP, 17 = UDP */
    u8 state;
    u16 local_port;
    u16 remote_port;
    u16 reserved;
    mg_ipv4_addr_t local_address;
    mg_ipv4_addr_t remote_address;
    u32 process_id;
    char process_name[MG_NET_NAME_MAX];
} mg_net_connection_info_t;

/* All blocking network calls use milliseconds.  Zero asks for an immediate
 * result; DEFAULT selects the protocol's bounded default timeout. */
#define MG_NET_TIMEOUT_IMMEDIATE 0U
#define MG_NET_TIMEOUT_DEFAULT   (~(u32)0)

enum mg_net_operation {
    MG_NET_OP_INFO = 1,
    MG_NET_OP_RESOLVE_A,
    MG_NET_OP_ICMP_OPEN,
    MG_NET_OP_ICMP_ECHO,
    MG_NET_OP_DATAGRAM_OPEN,
    MG_NET_OP_DATAGRAM_SEND,
    MG_NET_OP_DATAGRAM_RECEIVE,
    MG_NET_OP_STREAM_CONNECT,
    MG_NET_OP_STREAM_SEND,
    MG_NET_OP_STREAM_RECEIVE,
    MG_NET_OP_STREAM_CLOSE,
    MG_NET_OP_ICMP_CLOSE,
    MG_NET_OP_DATAGRAM_CLOSE,
    MG_NET_OP_INTERFACES,
    MG_NET_OP_ROUTES,
    MG_NET_OP_NEIGHBORS,
    MG_NET_OP_CONNECTIONS,
    MG_NET_OP_RENEW,
    MG_NET_OP_SET_MANUAL,
    MG_NET_OP_SET_AUTOMATIC,
    MG_NET_OP_RELOAD,
};

/* Stable request ABI for the Mangrove-native network-object syscall.  Buffer
 * pointers are copied/validated by the kernel and are never retained. */
typedef struct {
    u32 operation;
    mg_handle_t handle;
    u32 timeout_ms; /* 0 = immediate; UINT32_MAX = bounded kernel default. */
    u32 flags;
    mg_net_endpoint_t endpoint;
    const void *buffer;
    usize buffer_length;
    void *result;
    usize result_capacity;
} mg_net_request_t;

bool mg_ipv4_parse(const char *text, mg_ipv4_addr_t *address);
bool mg_ipv4_format(const mg_ipv4_addr_t *address, char *text, usize capacity);

mg_result_t mg_net_info(mg_net_info_t *info);
mg_result_t mg_net_interfaces(mg_net_interface_info_t *entries, usize capacity);
mg_result_t mg_net_routes(mg_net_route_info_t *entries, usize capacity);
mg_result_t mg_net_neighbors(mg_net_neighbor_info_t *entries, usize capacity);
mg_result_t mg_net_connections(mg_net_connection_info_t *entries, usize capacity);
mg_result_t mg_net_renew(u32 timeout_ms);
mg_result_t mg_net_set_manual(const mg_net_manual_config_t *configuration);
mg_result_t mg_net_set_automatic(void);
mg_result_t mg_net_reload(void);
mg_result_t mg_net_resolve_a(const char *hostname, mg_ipv4_addr_t *address,
                             u32 timeout_ms);

mg_result_t mg_icmp_open(mg_handle_t *handle);
mg_result_t mg_icmp_echo(mg_handle_t handle, const mg_ipv4_addr_t *destination,
                         const void *payload, usize payload_length,
                         u32 timeout_ms, mg_icmp_echo_result_t *result);
mg_result_t mg_icmp_close(mg_handle_t handle);

mg_result_t mg_datagram_open(u16 local_port, mg_handle_t *handle);
mg_result_t mg_datagram_send(mg_handle_t handle, const mg_net_endpoint_t *destination,
                             const void *data, usize length, u32 timeout_ms);
mg_result_t mg_datagram_receive(mg_handle_t handle, void *data, usize capacity,
                                u32 timeout_ms, mg_datagram_result_t *result);
mg_result_t mg_datagram_close(mg_handle_t handle);

mg_result_t mg_stream_connect(const mg_net_endpoint_t *remote, u32 timeout_ms,
                              mg_handle_t *handle);
mg_result_t mg_stream_send(mg_handle_t handle, const void *data, usize length,
                           u32 timeout_ms);
mg_result_t mg_stream_receive(mg_handle_t handle, void *data, usize capacity,
                              u32 timeout_ms);
mg_result_t mg_stream_close(mg_handle_t handle);
