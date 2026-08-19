#include <net/ipv4.h>
#include <net/tcp.h>
#include <scheduler.h>
#include <timer.h>

#define TCP_RX_CAPACITY 4096U
#define TCP_RTO_TICKS 1000U
#define TCP_TIME_WAIT_TICKS 1000U
#define TCP_MAX_RETRIES 3U
#define TCP_ACTION_ACK 0x1U

struct tcp_connection {
    bool in_use;
    net_device_t *device;
    net_ipv4_t local;
    net_ipv4_t remote;
    u16 local_port;
    u16 remote_port;
    volatile tcp_state_t state;
    volatile tcp_status_t status;
    u32 iss;
    u32 snd_una;
    u32 snd_nxt;
    u32 rcv_nxt;
    u16 remote_window;
    u8 receive_buffer[TCP_RX_CAPACITY];
    usize receive_length;
    u8 tx_buffer[TCP_MSS];
    usize tx_length;
    u32 tx_sequence;
    u8 tx_flags;
    bool tx_outstanding;
    u64 retransmit_deadline;
    u32 retries;
    bool peer_fin_seen;
    u64 time_wait_deadline;
    volatile u32 actions;
};

static struct tcp_connection connection;

static u16 tcp_read16(const u8 *p)
{
    return ((u16)p[0] << 8) | p[1];
}

static void tcp_write16(u8 *p, u16 value)
{
    p[0] = (u8)(value >> 8);
    p[1] = (u8)value;
}

static void tcp_write32(u8 *p, u32 value)
{
    p[0] = (u8)(value >> 24);
    p[1] = (u8)(value >> 16);
    p[2] = (u8)(value >> 8);
    p[3] = (u8)value;
}

static u32 tcp_sequence_space(u8 flags, usize length)
{
    return (u32)length + ((flags & TCP_FLAG_SYN) ? 1U : 0U) +
           ((flags & TCP_FLAG_FIN) ? 1U : 0U);
}

static bool tcp_send_raw(struct tcp_connection *c, u32 sequence, u32 acknowledgment,
                         u8 flags, const u8 *payload, usize length)
{
    u8 packet[TCP_HEADER_MIN_SIZE + TCP_MSS];
    u16 checksum;
    if (!c || !c->device || length > TCP_MSS) return false;
    tcp_write16(packet + 0, c->local_port);
    tcp_write16(packet + 2, c->remote_port);
    tcp_write32(packet + 4, sequence);
    tcp_write32(packet + 8, acknowledgment);
    packet[12] = 5U << 4;
    packet[13] = flags;
    tcp_write16(packet + 14, TCP_RX_CAPACITY);
    packet[16] = packet[17] = 0;
    packet[18] = packet[19] = 0;
    for (usize i = 0; i < length; i++) packet[TCP_HEADER_MIN_SIZE + i] = payload[i];
    checksum = tcp_checksum(c->local, c->remote, packet, TCP_HEADER_MIN_SIZE + length);
    tcp_write16(packet + 16, checksum);
    return ipv4_transmit_from(c->device, c->local, c->remote, TCP_PROTOCOL,
                              packet, TCP_HEADER_MIN_SIZE + length);
}

static void tcp_track(struct tcp_connection *c, u32 sequence, u8 flags,
                      const u8 *payload, usize length)
{
    c->tx_sequence = sequence;
    c->tx_flags = flags;
    c->tx_length = length;
    for (usize i = 0; i < length; i++) c->tx_buffer[i] = payload[i];
    c->tx_outstanding = true;
    c->retries = 0;
    c->retransmit_deadline = timer_ticks() + TCP_RTO_TICKS;
}

static bool tcp_send_tracked(struct tcp_connection *c, u8 flags,
                             const u8 *payload, usize length)
{
    u32 sequence;
    if (!c || c->tx_outstanding || length > TCP_MSS) return false;
    sequence = c->snd_nxt;
    tcp_track(c, sequence, flags, payload, length);
    c->snd_nxt += tcp_sequence_space(flags, length);
    return tcp_send_raw(c, sequence, c->rcv_nxt, flags, payload, length);
}

static void tcp_fail(struct tcp_connection *c, tcp_status_t status)
{
    c->status = status;
    c->state = TCP_STATE_CLOSED;
    c->tx_outstanding = false;
    c->in_use = false;
}

static void tcp_process_ack(struct tcp_connection *c, u32 acknowledgment)
{
    u32 segment_end;
    if (tcp_seq_before(acknowledgment, c->snd_una) ||
        tcp_seq_before(c->snd_nxt, acknowledgment)) return;
    if (acknowledgment == c->snd_una) return;
    c->snd_una = acknowledgment;
    if (c->tx_outstanding) {
        segment_end = c->tx_sequence + tcp_sequence_space(c->tx_flags, c->tx_length);
        if (!tcp_seq_before(acknowledgment, segment_end)) {
            c->tx_outstanding = false;
            c->retries = 0;
        } else if (!(c->tx_flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) &&
                   !tcp_seq_before(acknowledgment, c->tx_sequence)) {
            usize acknowledged = (usize)(acknowledgment - c->tx_sequence);
            if (acknowledged <= c->tx_length) {
                for (usize i = acknowledged; i < c->tx_length; i++)
                    c->tx_buffer[i - acknowledged] = c->tx_buffer[i];
                c->tx_length -= acknowledged;
                c->tx_sequence = acknowledgment;
                c->retransmit_deadline = timer_ticks() + TCP_RTO_TICKS;
            }
        }
    }
    if (c->state == TCP_STATE_FIN_WAIT_1 && !c->tx_outstanding) {
        if (c->peer_fin_seen) {
            c->state = TCP_STATE_TIME_WAIT;
            c->time_wait_deadline = timer_ticks() + TCP_TIME_WAIT_TICKS;
        } else {
            c->state = TCP_STATE_FIN_WAIT_2;
        }
    } else if (c->state == TCP_STATE_LAST_ACK && !c->tx_outstanding) {
        c->state = TCP_STATE_CLOSED;
        c->in_use = false;
    }
}

static void tcp_queue_ack(struct tcp_connection *c)
{
    __atomic_fetch_or(&c->actions, TCP_ACTION_ACK, __ATOMIC_RELEASE);
}

static void tcp_service(struct tcp_connection *c)
{
    u32 actions;
    if (!c) return;
    actions = __atomic_exchange_n(&c->actions, 0, __ATOMIC_ACQUIRE);
    if (actions & TCP_ACTION_ACK) {
        if (!tcp_send_raw(c, c->snd_nxt, c->rcv_nxt, TCP_FLAG_ACK, 0, 0))
            tcp_queue_ack(c);
    }
    if (c->state == TCP_STATE_TIME_WAIT && timer_ticks() >= c->time_wait_deadline) {
        c->state = TCP_STATE_CLOSED;
        c->in_use = false;
        return;
    }
    if (c->tx_outstanding && timer_ticks() >= c->retransmit_deadline) {
        if (c->retries >= TCP_MAX_RETRIES) {
            tcp_fail(c, TCP_STATUS_TIMEOUT);
            return;
        }
        (void)tcp_send_raw(c, c->tx_sequence, c->rcv_nxt, c->tx_flags,
                           c->tx_buffer, c->tx_length);
        c->retries++;
        c->retransmit_deadline = timer_ticks() + TCP_RTO_TICKS;
    }
}

static bool tcp_wait_for(struct tcp_connection *c, tcp_state_t wanted,
                         u64 timeout, tcp_status_t *status_out)
{
    u64 start = timer_ticks();
    while (timer_ticks() - start < timeout) {
        tcp_service(c);
        if (c->state == wanted) {
            if (status_out) *status_out = TCP_STATUS_SUCCESS;
            return true;
        }
        if (c->status != TCP_STATUS_SUCCESS) break;
        /* This core is also reached through a Ring-3 syscall, where the
         * architecture masks IF.  Yield instead of executing hlt with an
         * interrupt state inherited from the caller. */
        (void)scheduler_sleep(1);
    }
    if (c->status == TCP_STATUS_SUCCESS) tcp_fail(c, TCP_STATUS_TIMEOUT);
    if (status_out) *status_out = c->status;
    return false;
}

static bool tcp_matches(const struct tcp_connection *c, net_device_t *device,
                        net_ipv4_t source, net_ipv4_t destination,
                        const tcp_segment_t *segment)
{
    return c->in_use && c->device == device &&
           net_ipv4_equal(c->remote, source) && net_ipv4_equal(c->local, destination) &&
           c->remote_port == segment->source_port && c->local_port == segment->destination_port;
}

static void tcp_accept_payload(struct tcp_connection *c, const tcp_segment_t *segment)
{
    if (segment->sequence == c->rcv_nxt) {
        if (segment->payload_length > TCP_RX_CAPACITY - c->receive_length) {
            c->status = TCP_STATUS_BUFFER_FULL;
            tcp_queue_ack(c);
            return;
        }
        for (usize i = 0; i < segment->payload_length; i++)
            c->receive_buffer[c->receive_length + i] = segment->payload[i];
        c->receive_length += segment->payload_length;
        c->rcv_nxt += (u32)segment->payload_length;
        if (segment->payload_length) tcp_queue_ack(c);
        if (segment->flags & TCP_FLAG_FIN) {
            c->rcv_nxt++;
            c->peer_fin_seen = true;
            if (c->state == TCP_STATE_ESTABLISHED) {
                c->state = TCP_STATE_CLOSE_WAIT;
            } else if (c->state == TCP_STATE_FIN_WAIT_2) {
                c->state = TCP_STATE_TIME_WAIT;
                c->time_wait_deadline = timer_ticks() + TCP_TIME_WAIT_TICKS;
            }
            tcp_queue_ack(c);
        }
    } else {
        /* Old duplicates and future gaps are never delivered twice.  The
         * current cumulative ACK tells the peer exactly what is missing. */
        tcp_queue_ack(c);
    }
}

void tcp_init(void)
{
    connection = (struct tcp_connection){0};
}

void tcp_receive(net_device_t *device, net_ipv4_t source, net_ipv4_t destination,
                 const u8 *packet, usize length)
{
    tcp_segment_t segment;
    u16 received_checksum;
    struct tcp_connection *c = &connection;
    if (!packet || length < TCP_HEADER_MIN_SIZE || !tcp_parse_segment(packet, length, &segment)) return;
    received_checksum = tcp_read16(packet + 16);
    if (tcp_checksum(source, destination, packet, length) != received_checksum) return;
    if (!tcp_matches(c, device, source, destination, &segment)) return;
    c->remote_window = segment.window;
    if (segment.flags & TCP_FLAG_RST) {
        tcp_fail(c, TCP_STATUS_RESET);
        return;
    }
    if (c->state == TCP_STATE_SYN_SENT) {
        if ((segment.flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) !=
            (TCP_FLAG_SYN | TCP_FLAG_ACK) || segment.acknowledgment != c->snd_nxt) {
            return;
        }
        tcp_process_ack(c, segment.acknowledgment);
        c->rcv_nxt = segment.sequence + 1U;
        c->state = TCP_STATE_ESTABLISHED;
        tcp_queue_ack(c);
        return;
    }
    if (segment.flags & TCP_FLAG_ACK) tcp_process_ack(c, segment.acknowledgment);
    if (c->state == TCP_STATE_CLOSED) return;
    if (segment.payload_length || (segment.flags & TCP_FLAG_FIN)) tcp_accept_payload(c, &segment);
}

bool tcp_connect(net_device_t *device, net_ipv4_t remote, u16 remote_port,
                 tcp_connection_t **connection_out, tcp_status_t *status_out)
{
    const net_config_t *configuration = net_config();
    static u16 port_sequence;
    static u32 isn_sequence;
    struct tcp_connection *c = &connection;
    if (status_out) *status_out = TCP_STATUS_INVALID;
    if (!device || !remote_port || !connection_out || !configuration->configured) return false;
    if (c->in_use) { if (status_out) *status_out = TCP_STATUS_BUSY; return false; }
    *c = (struct tcp_connection){0};
    c->in_use = true;
    c->device = device;
    c->local = configuration->address;
    c->remote = remote;
    c->local_port = (u16)(49152U + (++port_sequence % (65535U - 49152U)));
    c->remote_port = remote_port;
    c->iss = (u32)timer_ticks() ^ (++isn_sequence * 0x9e3779b9U) ^
             ((u32)c->local_port << 16);
    c->snd_una = c->iss;
    c->snd_nxt = c->iss;
    c->status = TCP_STATUS_SUCCESS;
    c->state = TCP_STATE_SYN_SENT;
    (void)tcp_send_tracked(c, TCP_FLAG_SYN, 0, 0);
    if (!tcp_wait_for(c, TCP_STATE_ESTABLISHED, TCP_RTO_TICKS * (TCP_MAX_RETRIES + 1U), status_out))
        return false;
    *connection_out = c;
    return true;
}

bool tcp_send(tcp_connection_t *connection_ptr, const void *data, usize length,
              tcp_status_t *status_out)
{
    struct tcp_connection *c = connection_ptr;
    const u8 *bytes = (const u8 *)data;
    usize offset = 0;
    if (status_out) *status_out = TCP_STATUS_INVALID_STATE;
    if (!c || !data || c != &connection || c->state != TCP_STATE_ESTABLISHED) return false;
    while (offset < length) {
        usize chunk = length - offset;
        if (c->remote_window == 0) {
            if (status_out) *status_out = TCP_STATUS_TIMEOUT;
            return false;
        }
        if (chunk > TCP_MSS) chunk = TCP_MSS;
        if (chunk > c->remote_window) chunk = c->remote_window;
        if (!chunk || !tcp_send_tracked(c, TCP_FLAG_ACK | TCP_FLAG_PSH, bytes + offset, chunk)) {
            if (status_out) *status_out = TCP_STATUS_INVALID_STATE;
            return false;
        }
        if (!tcp_wait_for(c, TCP_STATE_ESTABLISHED,
                          TCP_RTO_TICKS * (TCP_MAX_RETRIES + 1U), status_out)) return false;
        offset += chunk;
    }
    if (status_out) *status_out = TCP_STATUS_SUCCESS;
    return true;
}

usize tcp_receive_bytes(tcp_connection_t *connection_ptr, void *output, usize capacity)
{
    struct tcp_connection *c = connection_ptr;
    u8 *bytes = (u8 *)output;
    usize count;
    if (!c || c != &connection || !output) return 0;
    tcp_service(c);
    count = c->receive_length < capacity ? c->receive_length : capacity;
    for (usize i = 0; i < count; i++) bytes[i] = c->receive_buffer[i];
    for (usize i = count; i < c->receive_length; i++) c->receive_buffer[i - count] = c->receive_buffer[i];
    c->receive_length -= count;
    return count;
}

bool tcp_close(tcp_connection_t *connection_ptr, tcp_status_t *status_out)
{
    struct tcp_connection *c = connection_ptr;
    u64 start;
    if (status_out) *status_out = TCP_STATUS_INVALID_STATE;
    if (!c || c != &connection) return false;
    tcp_service(c);
    if (c->state == TCP_STATE_ESTABLISHED) {
        c->state = TCP_STATE_FIN_WAIT_1;
        (void)tcp_send_tracked(c, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
    } else if (c->state == TCP_STATE_CLOSE_WAIT) {
        c->state = TCP_STATE_LAST_ACK;
        (void)tcp_send_tracked(c, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
    } else if (c->state == TCP_STATE_CLOSED) {
        c->in_use = false;
        if (status_out) *status_out = c->status;
        return c->status == TCP_STATUS_SUCCESS;
    } else if (c->state != TCP_STATE_FIN_WAIT_1 &&
               c->state != TCP_STATE_FIN_WAIT_2 &&
               c->state != TCP_STATE_LAST_ACK &&
               c->state != TCP_STATE_TIME_WAIT) {
        return false;
    }

    /* A close may be retried after the first call has already entered a
     * FIN state.  Continue servicing that state instead of returning while
     * leaving the singleton connection permanently reserved. */
    start = timer_ticks();
    while (timer_ticks() - start < TCP_RTO_TICKS * (TCP_MAX_RETRIES + 2U)) {
        tcp_service(c);
        if (c->state == TCP_STATE_CLOSED) {
            c->in_use = false;
            if (status_out) *status_out = c->status;
            return c->status == TCP_STATUS_SUCCESS;
        }
        (void)scheduler_sleep(1);
    }
    if (c->status == TCP_STATUS_SUCCESS) tcp_fail(c, TCP_STATUS_TIMEOUT);
    if (status_out) *status_out = c->status;
    return false;
}

void tcp_abort(tcp_connection_t *connection_ptr)
{
    if (connection_ptr != &connection) return;
    /* Late packets are ignored because in_use is cleared before the storage
     * is reset.  No user object may retain this pointer after its destructor
     * returns. */
    connection.in_use = false;
    connection = (struct tcp_connection){0};
}

tcp_state_t tcp_connection_state(const tcp_connection_t *connection_ptr)
{
    return connection_ptr == &connection ? connection.state : TCP_STATE_CLOSED;
}
