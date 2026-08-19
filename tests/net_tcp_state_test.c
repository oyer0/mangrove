#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long long usize;
typedef _Bool bool;
enum { false = 0, true = 1 };

typedef struct { u8 octet[4]; } net_ipv4_t;
typedef struct net_device net_device_t;
struct net_device {
    const char *name;
    u8 mac[6];
    usize mtu;
    bool (*transmit)(net_device_t *, const void *, usize);
    void *driver_data;
};
typedef struct {
    net_ipv4_t address, netmask, gateway, dns, dhcp_server;
    u32 lease_seconds;
    bool configured, has_gateway, has_dns;
} net_config_t;
typedef enum {
    TCP_STATE_CLOSED, TCP_STATE_SYN_SENT, TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT_1, TCP_STATE_FIN_WAIT_2, TCP_STATE_CLOSE_WAIT,
    TCP_STATE_LAST_ACK, TCP_STATE_TIME_WAIT
} tcp_state_t;
typedef enum {
    TCP_STATUS_SUCCESS, TCP_STATUS_TIMEOUT, TCP_STATUS_RESET,
    TCP_STATUS_UNREACHABLE, TCP_STATUS_INVALID_STATE, TCP_STATUS_BUFFER_FULL,
    TCP_STATUS_CLOSED, TCP_STATUS_BUSY, TCP_STATUS_INVALID
} tcp_status_t;
typedef struct tcp_connection tcp_connection_t;

void tcp_init(void);
bool tcp_connect(net_device_t *, net_ipv4_t, u16, tcp_connection_t **, tcp_status_t *);
bool tcp_send(tcp_connection_t *, const void *, usize, tcp_status_t *);
usize tcp_receive_bytes(tcp_connection_t *, void *, usize);
tcp_state_t tcp_connection_state(const tcp_connection_t *);
void tcp_receive(net_device_t *, net_ipv4_t, net_ipv4_t, const u8 *, usize);
u16 tcp_checksum(net_ipv4_t, net_ipv4_t, const void *, usize);

#define TCP_SYN 0x02U
#define TCP_ACK 0x10U
#define TCP_PSH 0x08U
#define TCP_RST 0x04U

static net_config_t config = {{{10,0,2,15}}, {{255,255,255,0}}, {{10,0,2,2}}, {{0}}, {{0}}, 0, true, true, false};
static net_device_t device = {"test", {0}, 1500, 0, 0};
static net_ipv4_t local = {{10,0,2,15}};
static net_ipv4_t remote = {{10,0,2,2}};
static u32 client_next;
static u32 server_next = 5001;
static u16 client_port;
static u64 ticks;
static u32 syn_count;
static u32 first_syn_sequence;

static u16 be16(const u8 *p) { return ((u16)p[0] << 8) | p[1]; }
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

static void inject(u8 flags, u32 sequence, u32 acknowledgment, const u8 *data, usize length)
{
    u8 packet[64] = {0};
    put16(packet, 12345); put16(packet + 2, client_port);
    put32(packet + 4, sequence); put32(packet + 8, acknowledgment);
    packet[12] = 5U << 4; packet[13] = flags; put16(packet + 14, 4096);
    for (usize i = 0; i < length; i++) packet[20 + i] = data[i];
    put16(packet + 16, tcp_checksum(remote, local, packet, 20 + length));
    tcp_receive(&device, remote, local, packet, 20 + length);
}

/* Advance directly beyond the fixed RTO so the first deliberately dropped SYN
 * exercises retransmission without executing host-mode hlt. */
u64 timer_ticks(void) { ticks += 2000; return ticks; }
bool scheduler_sleep(u64 duration) { (void)duration; return false; }
const net_config_t *net_config(void) { return &config; }
bool net_ipv4_equal(net_ipv4_t a, net_ipv4_t b) {
    for (u32 i = 0; i < 4; i++) if (a.octet[i] != b.octet[i]) return false;
    return true;
}

bool ipv4_transmit_from(net_device_t *dev, net_ipv4_t source, net_ipv4_t destination,
                        u8 protocol, const void *payload, usize length)
{
    const u8 *tcp = payload;
    u8 flags;
    (void)dev; (void)source; (void)destination; (void)protocol;
    assert(length >= 20);
    flags = tcp[13];
    client_port = be16(tcp);
    if (flags == TCP_SYN) {
        client_next = be32(tcp + 4) + 1;
        if (++syn_count == 1) {
            first_syn_sequence = be32(tcp + 4);
            return true; /* drop the first SYN */
        }
        assert(be32(tcp + 4) == first_syn_sequence);
        inject(TCP_SYN | TCP_ACK, 5000, client_next, 0, 0);
    } else if ((flags & (TCP_ACK | TCP_PSH)) == (TCP_ACK | TCP_PSH)) {
        static const u8 echo[] = {'o', 'k'};
        client_next = be32(tcp + 4) + (u32)(length - 20);
        inject(TCP_ACK, server_next, client_next, 0, 0);
        inject(TCP_ACK | TCP_PSH, server_next, client_next, echo, sizeof(echo));
        inject(TCP_ACK | TCP_PSH, server_next, client_next, echo, sizeof(echo)); /* duplicate */
        inject(TCP_ACK | TCP_PSH, server_next + 8, client_next, echo, sizeof(echo)); /* gap */
        server_next += sizeof(echo);
    }
    return true;
}

int main(void)
{
    tcp_connection_t *connection;
    tcp_status_t status;
    u8 received[8] = {0};
    tcp_init();
    assert(tcp_connect(&device, remote, 12345, &connection, &status));
    assert(syn_count == 2);
    assert(status == TCP_STATUS_SUCCESS && tcp_connection_state(connection) == TCP_STATE_ESTABLISHED);
    assert(tcp_send(connection, "go", 2, &status));
    assert(status == TCP_STATUS_SUCCESS);
    assert(tcp_receive_bytes(connection, received, sizeof(received)) == 2);
    assert(received[0] == 'o' && received[1] == 'k');
    inject(TCP_RST, server_next, client_next, 0, 0);
    assert(tcp_connection_state(connection) == TCP_STATE_CLOSED);
    puts("TCP state tests passed");
    return 0;
}
