/* SPDX-License-Identifier: GPL-3.0-or-later */
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
    TCP_STATUS_INVALID,
    TCP_STATUS_INTERRUPTED,
    TCP_STATUS_DEVICE_GONE,
    TCP_STATUS_WOULD_BLOCK
} tcp_status_t;

typedef bool (*tcp_interrupt_check_t)(void *context);

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
                 u64 timeout_ms, tcp_interrupt_check_t interrupted,
                 void *interrupt_context, tcp_connection_t **connection_out,
                 tcp_status_t *status_out);
/* Returns payload bytes accepted into the bounded retransmission queue.
 * A positive partial count can accompany a timeout/interruption while the
 * final accepted segment remains owned by TCP until ACK or connection failure. */
usize tcp_send(tcp_connection_t *connection, const void *data, usize length,
               u64 timeout_ms, tcp_interrupt_check_t interrupted,
               void *interrupt_context, tcp_status_t *status_out);
usize tcp_receive_bytes(tcp_connection_t *connection, void *output, usize capacity,
                        tcp_status_t *status_out);
bool tcp_close(tcp_connection_t *connection, u64 timeout_ms,
               tcp_interrupt_check_t interrupted, void *interrupt_context,
               tcp_status_t *status_out);
/* Release the singleton connection immediately.  This is for object/process
 * teardown after a best-effort close; it intentionally does not wait or emit
 * more packets from a destruction path. */
void tcp_abort(tcp_connection_t *connection);
tcp_state_t tcp_connection_state(const tcp_connection_t *connection);
