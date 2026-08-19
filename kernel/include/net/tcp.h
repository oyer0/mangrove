#pragma once

#include <net/config.h>
#include <net/net.h>

#define TCP_PROTOCOL 6U
#define TCP_HEADER_MIN_SIZE 20U
#define TCP_MSS 1460U

#define TCP_FLAG_FIN 0x01U
#define TCP_FLAG_SYN 0x02U
#define TCP_FLAG_RST 0x04U
#define TCP_FLAG_PSH 0x08U
#define TCP_FLAG_ACK 0x10U

typedef enum {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_SYN_SENT,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT_1,
    TCP_STATE_FIN_WAIT_2,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_LAST_ACK,
    TCP_STATE_TIME_WAIT
} tcp_state_t;

typedef enum {
    TCP_STATUS_SUCCESS = 0,
    TCP_STATUS_TIMEOUT,
    TCP_STATUS_RESET,
    TCP_STATUS_UNREACHABLE,
    TCP_STATUS_INVALID_STATE,
    TCP_STATUS_BUFFER_FULL,
    TCP_STATUS_CLOSED,
    TCP_STATUS_BUSY,
    TCP_STATUS_INVALID
} tcp_status_t;

typedef struct {
    u16 source_port;
    u16 destination_port;
    u32 sequence;
    u32 acknowledgment;
    u8 flags;
    u16 window;
    const u8 *payload;
    usize payload_length;
} tcp_segment_t;

typedef struct tcp_connection tcp_connection_t;

bool tcp_seq_before(u32 left, u32 right);
bool tcp_seq_between(u32 value, u32 first, u32 last);
u16 tcp_checksum(net_ipv4_t source, net_ipv4_t destination,
                 const void *segment, usize length);
bool tcp_parse_segment(const u8 *packet, usize length, tcp_segment_t *segment);

void tcp_init(void);
void tcp_receive(net_device_t *device, net_ipv4_t source, net_ipv4_t destination,
                 const u8 *packet, usize length);
bool tcp_connect(net_device_t *device, net_ipv4_t remote, u16 remote_port,
                 tcp_connection_t **connection_out, tcp_status_t *status_out);
bool tcp_send(tcp_connection_t *connection, const void *data, usize length,
              tcp_status_t *status_out);
usize tcp_receive_bytes(tcp_connection_t *connection, void *output, usize capacity);
bool tcp_close(tcp_connection_t *connection, tcp_status_t *status_out);
/* Release the singleton connection immediately.  This is for object/process
 * teardown after a best-effort close; it intentionally does not wait or emit
 * more packets from a destruction path. */
void tcp_abort(tcp_connection_t *connection);
tcp_state_t tcp_connection_state(const tcp_connection_t *connection);
