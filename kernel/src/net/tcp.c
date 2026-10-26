/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <net/ipv4.h>
#include <net/tcp.h>
#include <scheduler.h>
#include <stddef.h>
#include <spinlock.h>
#include <timer.h>

#define TCP_RX_CAPACITY 4096U
#define TCP_RTO_MS 1000U
#define TCP_TIME_WAIT_MS 1000U
#define TCP_MAX_RETRIES 3U

typedef struct {
    net_device_t *device;
    u64 device_generation;
    net_ipv4_t local;
    net_ipv4_t remote;
    u16 local_port;
    u16 remote_port;
    u32 sequence;
    u32 acknowledgment;
    u16 window;
    u8 flags;
    usize payload_length;
    u8 payload[TCP_MSS];
} tcp_packet_t;

struct tcp_connection {
    bool in_use;
    bool send_in_progress;
    net_device_t *device;
    u64 device_generation;
    net_ipv4_t local;
    net_ipv4_t remote;
    u16 local_port;
    u16 remote_port;
    tcp_state_t state;
    tcp_status_t status;
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
    bool ack_pending;
    u64 ack_retry_deadline;
};

static struct tcp_connection connection;
static spinlock_t connection_lock;

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

static u64 tcp_lock(void)
{
    return spin_lock_irqsave(&connection_lock);
}

static void tcp_unlock(u64 flags)
{
    spin_unlock_irqrestore(&connection_lock, flags);
}

static void tcp_packet_from_connection_locked(const struct tcp_connection *c,
                                               tcp_packet_t *packet,
                                               u32 sequence, u8 flags,
                                               const u8 *payload, usize length)
{
    *packet = (tcp_packet_t){
        .device = c->device,
        .device_generation = c->device_generation,
        .local = c->local,
        .remote = c->remote,
        .local_port = c->local_port,
        .remote_port = c->remote_port,
        .sequence = sequence,
        .acknowledgment = c->rcv_nxt,
        .window = (u16)(TCP_RX_CAPACITY - c->receive_length),
        .flags = flags,
        .payload_length = length,
    };
    for (usize i = 0; i < length; i++) packet->payload[i] = payload[i];
}

static bool tcp_transmit_packet(const tcp_packet_t *snapshot)
{
    u8 packet[TCP_HEADER_MIN_SIZE + TCP_MSS];
    u16 checksum;
    if (!snapshot || !snapshot->device || snapshot->payload_length > TCP_MSS ||
        !net_device_instance_current(snapshot->device,
                                     snapshot->device_generation)) return false;
    tcp_write16(packet + 0, snapshot->local_port);
    tcp_write16(packet + 2, snapshot->remote_port);
    tcp_write32(packet + 4, snapshot->sequence);
    tcp_write32(packet + 8, snapshot->acknowledgment);
    packet[12] = 5U << 4;
    packet[13] = snapshot->flags;
    tcp_write16(packet + 14, snapshot->window);
    packet[16] = packet[17] = 0;
    packet[18] = packet[19] = 0;
    for (usize i = 0; i < snapshot->payload_length; i++)
        packet[TCP_HEADER_MIN_SIZE + i] = snapshot->payload[i];
    checksum = tcp_checksum(snapshot->local, snapshot->remote, packet,
                            TCP_HEADER_MIN_SIZE + snapshot->payload_length);
    tcp_write16(packet + 16, checksum);
    return ipv4_transmit_from(snapshot->device, snapshot->local,
                              snapshot->remote, TCP_PROTOCOL, packet,
                              TCP_HEADER_MIN_SIZE + snapshot->payload_length);
}

static void tcp_track_locked(struct tcp_connection *c, u32 sequence, u8 flags,
                             const u8 *payload, usize length)
{
    c->tx_sequence = sequence;
    c->tx_flags = flags;
    c->tx_length = length;
    for (usize i = 0; i < length; i++) c->tx_buffer[i] = payload[i];
    c->tx_outstanding = true;
    c->retries = 0;
    c->retransmit_deadline = timer_uptime_ms() + TCP_RTO_MS;
}

static void tcp_fail_locked(struct tcp_connection *c, tcp_status_t status)
{
    c->status = status;
    c->state = TCP_STATE_CLOSED;
    c->tx_outstanding = false;
    /* Keep the singleton reserved while its stream object owns it.  Otherwise
     * a reset can make an old handle alias a newly-created connection. */
}

static void tcp_process_ack_locked(struct tcp_connection *c, u32 acknowledgment)
{
    u32 segment_end;
    if (tcp_seq_before(acknowledgment, c->snd_una) ||
        tcp_seq_before(c->snd_nxt, acknowledgment)) return;
    if (acknowledgment == c->snd_una) return;
    c->snd_una = acknowledgment;
    if (c->tx_outstanding) {
        segment_end = c->tx_sequence +
            tcp_sequence_space(c->tx_flags, c->tx_length);
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
                c->retransmit_deadline = timer_uptime_ms() + TCP_RTO_MS;
            }
        }
    }
    if (c->state == TCP_STATE_FIN_WAIT_1 && !c->tx_outstanding) {
        if (c->peer_fin_seen) {
            c->state = TCP_STATE_TIME_WAIT;
            c->time_wait_deadline = timer_uptime_ms() + TCP_TIME_WAIT_MS;
        } else {
            c->state = TCP_STATE_FIN_WAIT_2;
        }
    } else if (c->state == TCP_STATE_LAST_ACK && !c->tx_outstanding) {
        c->state = TCP_STATE_CLOSED;
    }
}

static bool tcp_matches_locked(const struct tcp_connection *c,
                               net_device_t *device, net_ipv4_t source,
                               net_ipv4_t destination,
                               const tcp_segment_t *segment)
{
    return c->in_use && c->device == device &&
           c->device_generation == device->generation &&
           net_ipv4_equal(c->remote, source) &&
           net_ipv4_equal(c->local, destination) &&
           c->remote_port == segment->source_port &&
           c->local_port == segment->destination_port;
}

static void tcp_accept_payload_locked(struct tcp_connection *c,
                                      const tcp_segment_t *segment)
{
    if (segment->sequence == c->rcv_nxt) {
        if (segment->payload_length > TCP_RX_CAPACITY - c->receive_length) {
            /* Do not consume data that cannot be retained.  ACK the current
             * receive edge with a zero/smaller advertised window. */
            c->ack_pending = true;
            return;
        }
        for (usize i = 0; i < segment->payload_length; i++)
            c->receive_buffer[c->receive_length + i] = segment->payload[i];
        c->receive_length += segment->payload_length;
        c->rcv_nxt += (u32)segment->payload_length;
        if (segment->payload_length) c->ack_pending = true;
        if (segment->flags & TCP_FLAG_FIN) {
            c->rcv_nxt++;
            c->peer_fin_seen = true;
            if (c->state == TCP_STATE_ESTABLISHED) {
                c->state = TCP_STATE_CLOSE_WAIT;
            } else if (c->state == TCP_STATE_FIN_WAIT_2) {
                c->state = TCP_STATE_TIME_WAIT;
                c->time_wait_deadline = timer_uptime_ms() + TCP_TIME_WAIT_MS;
            }
            c->ack_pending = true;
        }
    } else {
        /* Repeated or out-of-order bytes are never delivered twice. */
        c->ack_pending = true;
    }
}

void tcp_init(void)
{
    spinlock_init(&connection_lock);
    u64 flags = tcp_lock();
    connection = (struct tcp_connection){0};
    tcp_unlock(flags);
}

void tcp_receive(net_device_t *device, net_ipv4_t source, net_ipv4_t destination,
                 const u8 *packet, usize length)
{
    tcp_segment_t segment;
    struct tcp_connection *c = &connection;
    if (!device || !packet || length < TCP_HEADER_MIN_SIZE ||
        !tcp_parse_segment(packet, length, &segment) ||
        tcp_checksum(source, destination, packet, length) !=
            tcp_read16(packet + 16)) return;
    if (!net_device_instance_current(device, device->generation)) return;
    u64 flags = tcp_lock();
    if (!tcp_matches_locked(c, device, source, destination, &segment)) {
        tcp_unlock(flags);
        return;
    }
    c->remote_window = segment.window;
    if (segment.flags & TCP_FLAG_RST) {
        tcp_fail_locked(c, TCP_STATUS_RESET);
        tcp_unlock(flags);
        return;
    }
    if (c->state == TCP_STATE_SYN_SENT) {
        if ((segment.flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) !=
                (TCP_FLAG_SYN | TCP_FLAG_ACK) ||
            segment.acknowledgment != c->snd_nxt) {
            tcp_unlock(flags);
            return;
        }
        tcp_process_ack_locked(c, segment.acknowledgment);
        c->rcv_nxt = segment.sequence + 1U;
        c->state = TCP_STATE_ESTABLISHED;
        c->ack_pending = true;
        tcp_unlock(flags);
        return;
    }
    if (segment.flags & TCP_FLAG_ACK)
        tcp_process_ack_locked(c, segment.acknowledgment);
    if (c->state != TCP_STATE_CLOSED &&
        (segment.payload_length || (segment.flags & TCP_FLAG_FIN)))
        tcp_accept_payload_locked(c, &segment);
    tcp_unlock(flags);
}

/* Service timers and deferred ACKs. State is snapshotted under the lock; no
 * device transmit or scheduler operation occurs while the TCP lock is held. */
static void tcp_service(struct tcp_connection *c)
{
    tcp_packet_t packet;
    bool transmit = false;
    bool retransmission = false;
    u64 now = timer_uptime_ms();
    if (!c) return;
    u64 flags = tcp_lock();
    if (!c->in_use) {
        tcp_unlock(flags);
        return;
    }
    net_device_t *device = c->device;
    u64 device_generation = c->device_generation;
    tcp_unlock(flags);
    bool current = device &&
        net_device_instance_current(device, device_generation);
    flags = tcp_lock();
    if (!c->in_use || c->device != device ||
        c->device_generation != device_generation) {
        tcp_unlock(flags);
        return;
    }
    if (!current) {
        tcp_fail_locked(c, TCP_STATUS_DEVICE_GONE);
        tcp_unlock(flags);
        return;
    }
    if (c->state == TCP_STATE_TIME_WAIT && now >= c->time_wait_deadline) {
        c->state = TCP_STATE_CLOSED;
        tcp_unlock(flags);
        return;
    }
    if (c->tx_outstanding && now >= c->retransmit_deadline) {
        if (c->retries >= TCP_MAX_RETRIES) {
            tcp_fail_locked(c, TCP_STATUS_TIMEOUT);
            tcp_unlock(flags);
            return;
        }
        tcp_packet_from_connection_locked(c, &packet, c->tx_sequence,
                                          c->tx_flags, c->tx_buffer,
                                          c->tx_length);
        c->retries++;
        c->retransmit_deadline = now + TCP_RTO_MS;
        transmit = retransmission = true;
    } else if (c->ack_pending && now >= c->ack_retry_deadline) {
        c->ack_pending = false;
        tcp_packet_from_connection_locked(c, &packet, c->snd_nxt,
                                          TCP_FLAG_ACK, NULL, 0);
        transmit = true;
    }
    tcp_unlock(flags);
    if (transmit && !tcp_transmit_packet(&packet)) {
        flags = tcp_lock();
        if (c->in_use && c->device == packet.device &&
            c->device_generation == packet.device_generation) {
            if (!retransmission) {
                c->ack_pending = true;
                c->ack_retry_deadline = timer_uptime_ms() + 100U;
            }
        }
        tcp_unlock(flags);
    }
}

static bool tcp_wait_condition(struct tcp_connection *c, u64 deadline,
                               bool wait_for_tx_clear, bool wait_for_window,
                               bool nonblocking,
                               tcp_interrupt_check_t interrupted,
                               void *interrupt_context,
                               tcp_status_t *status_out)
{
    for (;;) {
        tcp_service(c);
        u64 flags = tcp_lock();
        tcp_status_t status = c->status;
        tcp_state_t state = c->state;
        bool done = (!wait_for_tx_clear || !c->tx_outstanding) &&
                    (!wait_for_window || c->remote_window != 0);
        tcp_unlock(flags);
        if (status != TCP_STATUS_SUCCESS) {
            if (status_out) *status_out = status;
            return false;
        }
        if (state != TCP_STATE_ESTABLISHED && state != TCP_STATE_CLOSE_WAIT) {
            if (status_out) *status_out = TCP_STATUS_CLOSED;
            return false;
        }
        if (done) return true;
        if (interrupted && interrupted(interrupt_context)) {
            if (status_out) *status_out = TCP_STATUS_INTERRUPTED;
            return false;
        }
        if (timer_uptime_ms() >= deadline) {
            if (status_out) *status_out = nonblocking ?
                TCP_STATUS_WOULD_BLOCK : TCP_STATUS_TIMEOUT;
            return false;
        }
        (void)scheduler_sleep(1);
    }
}

static bool tcp_wait_connect(struct tcp_connection *c, u64 deadline,
                             tcp_interrupt_check_t interrupted,
                             void *interrupt_context, tcp_status_t *status_out)
{
    for (;;) {
        tcp_service(c);
        u64 flags = tcp_lock();
        tcp_status_t status = c->status;
        tcp_state_t state = c->state;
        tcp_unlock(flags);
        if (status != TCP_STATUS_SUCCESS) {
            if (status_out) *status_out = status;
            return false;
        }
        if (state == TCP_STATE_ESTABLISHED) {
            if (status_out) *status_out = TCP_STATUS_SUCCESS;
            return true;
        }
        if (interrupted && interrupted(interrupt_context)) {
            if (status_out) *status_out = TCP_STATUS_INTERRUPTED;
            return false;
        }
        if (timer_uptime_ms() >= deadline) {
            if (status_out) *status_out = TCP_STATUS_TIMEOUT;
            return false;
        }
        (void)scheduler_sleep(1);
    }
}

bool tcp_connect(net_device_t *device, net_ipv4_t remote, u16 remote_port,
                 u64 timeout_ms, tcp_interrupt_check_t interrupted,
                 void *interrupt_context, tcp_connection_t **connection_out,
                 tcp_status_t *status_out)
{
    const net_config_t *configuration = net_config();
    static u16 port_sequence;
    static u32 isn_sequence;
    struct tcp_connection *c = &connection;
    tcp_packet_t packet;
    bool sent;
    u64 deadline;
    if (status_out) *status_out = TCP_STATUS_INVALID;
    if (!device || !remote_port || !connection_out || !configuration ||
        !configuration->configured) return false;
    *connection_out = NULL;
    if (timeout_ms == 0) {
        if (status_out) *status_out = TCP_STATUS_WOULD_BLOCK;
        return false;
    }
    deadline = timer_uptime_ms() + timeout_ms;
    u64 flags = tcp_lock();
    if (c->in_use) {
        tcp_unlock(flags);
        if (status_out) *status_out = TCP_STATUS_BUSY;
        return false;
    }
    *c = (struct tcp_connection){0};
    c->in_use = true;
    c->device = device;
    c->device_generation = device->generation;
    c->local = configuration->address;
    c->remote = remote;
    c->local_port = (u16)(49152U + (++port_sequence % (65535U - 49152U)));
    c->remote_port = remote_port;
    c->iss = (u32)timer_uptime_ms() ^ (++isn_sequence * 0x9e3779b9U) ^
             ((u32)c->local_port << 16);
    c->snd_una = c->iss;
    c->snd_nxt = c->iss;
    c->status = TCP_STATUS_SUCCESS;
    c->state = TCP_STATE_SYN_SENT;
    tcp_track_locked(c, c->snd_nxt, TCP_FLAG_SYN, NULL, 0);
    c->snd_nxt++;
    tcp_packet_from_connection_locked(c, &packet, c->iss, TCP_FLAG_SYN,
                                      NULL, 0);
    tcp_unlock(flags);
    sent = tcp_transmit_packet(&packet);
    (void)sent; /* Retransmission service also handles initial SYN failure. */
    if (!tcp_wait_connect(c, deadline, interrupted, interrupt_context,
                          status_out)) {
        tcp_abort(c);
        return false;
    }
    *connection_out = c;
    return true;
}

usize tcp_send(tcp_connection_t *connection_ptr, const void *data, usize length,
               u64 timeout_ms, tcp_interrupt_check_t interrupted,
               void *interrupt_context, tcp_status_t *status_out)
{
    struct tcp_connection *c = connection_ptr;
    const u8 *bytes = (const u8 *)data;
    usize accepted = 0;
    u64 deadline = timer_uptime_ms() + timeout_ms;
    tcp_status_t result_status = TCP_STATUS_SUCCESS;
    if (status_out) *status_out = TCP_STATUS_INVALID_STATE;
    if (!c || c != &connection || (!data && length)) {
        if (status_out) *status_out = TCP_STATUS_INVALID;
        return 0;
    }
    if (!length) {
        if (status_out) *status_out = TCP_STATUS_SUCCESS;
        return 0;
    }
    u64 flags = tcp_lock();
    if (!c->in_use ||
        (c->state != TCP_STATE_ESTABLISHED &&
         c->state != TCP_STATE_CLOSE_WAIT) ||
        c->send_in_progress) {
        tcp_status_t status = c->status != TCP_STATUS_SUCCESS ? c->status :
                              c->send_in_progress ? TCP_STATUS_BUSY : TCP_STATUS_CLOSED;
        tcp_unlock(flags);
        if (status_out) *status_out = status;
        return 0;
    }
    c->send_in_progress = true;
    tcp_unlock(flags);

    while (accepted < length) {
        tcp_packet_t packet;
        usize chunk;
        u32 sequence;
        flags = tcp_lock();
        bool can_send = c->in_use &&
                        (c->state == TCP_STATE_ESTABLISHED ||
                         c->state == TCP_STATE_CLOSE_WAIT) &&
                        c->status == TCP_STATUS_SUCCESS && !c->tx_outstanding &&
                        c->remote_window != 0;
        if (!can_send) {
            tcp_status_t status = c->status != TCP_STATUS_SUCCESS ? c->status :
                                  c->state != TCP_STATE_ESTABLISHED ? TCP_STATUS_CLOSED :
                                  TCP_STATUS_SUCCESS;
            tcp_unlock(flags);
            if (status != TCP_STATUS_SUCCESS) {
                result_status = status;
                break;
            }
            if (!tcp_wait_condition(c, deadline, true, true, timeout_ms == 0,
                                    interrupted, interrupt_context,
                                    &result_status)) break;
            continue;
        }
        chunk = length - accepted;
        if (chunk > TCP_MSS) chunk = TCP_MSS;
        if (chunk > c->remote_window) chunk = c->remote_window;
        sequence = c->snd_nxt;
        tcp_track_locked(c, sequence, TCP_FLAG_ACK | TCP_FLAG_PSH,
                         bytes + accepted, chunk);
        c->snd_nxt += (u32)chunk;
        tcp_packet_from_connection_locked(c, &packet, sequence,
                                          TCP_FLAG_ACK | TCP_FLAG_PSH,
                                          bytes + accepted, chunk);
        tcp_unlock(flags);

        if (!tcp_transmit_packet(&packet)) {
            flags = tcp_lock();
            bool acknowledged = !c->tx_outstanding &&
                !tcp_seq_before(c->snd_una, sequence + (u32)chunk);
            if (!acknowledged && c->tx_outstanding && c->tx_sequence == sequence) {
                c->tx_outstanding = false;
                c->tx_length = 0;
                c->snd_nxt = sequence;
            }
            tcp_unlock(flags);
            if (!acknowledged) {
                result_status =
                    net_device_instance_current(packet.device,
                                                packet.device_generation) ?
                    TCP_STATUS_UNREACHABLE : TCP_STATUS_DEVICE_GONE;
                break;
            }
        }
        accepted += chunk;
        if (accepted < length &&
            !tcp_wait_condition(c, deadline, true, false, timeout_ms == 0,
                                interrupted, interrupt_context,
                                &result_status)) break;
    }

    flags = tcp_lock();
    c->send_in_progress = false;
    if (accepted == length) result_status = TCP_STATUS_SUCCESS;
    if (result_status == TCP_STATUS_INTERRUPTED && accepted != 0)
        tcp_fail_locked(c, TCP_STATUS_INTERRUPTED);
    tcp_unlock(flags);
    if (status_out) *status_out = result_status;
    return accepted;
}

usize tcp_receive_bytes(tcp_connection_t *connection_ptr, void *output,
                        usize capacity, tcp_status_t *status_out)
{
    struct tcp_connection *c = connection_ptr;
    u8 *bytes = (u8 *)output;
    usize count;
    bool send_window_update = false;
    if (status_out) *status_out = TCP_STATUS_INVALID_STATE;
    if (!c || c != &connection || !output || !capacity) {
        if (status_out) *status_out = TCP_STATUS_INVALID;
        return 0;
    }
    tcp_service(c);
    u64 flags = tcp_lock();
    count = c->receive_length < capacity ? c->receive_length : capacity;
    for (usize i = 0; i < count; i++) bytes[i] = c->receive_buffer[i];
    for (usize i = count; i < c->receive_length; i++)
        c->receive_buffer[i - count] = c->receive_buffer[i];
    c->receive_length -= count;
    if (count) {
        c->ack_pending = true;
        send_window_update = true;
    }
    tcp_status_t status = c->status;
    tcp_state_t state = c->state;
    tcp_unlock(flags);
    if (send_window_update) tcp_service(c);
    if (status_out) {
        if (count) *status_out = TCP_STATUS_SUCCESS;
        else if (status != TCP_STATUS_SUCCESS) *status_out = status;
        else if (state == TCP_STATE_CLOSED || state == TCP_STATE_CLOSE_WAIT)
            *status_out = TCP_STATUS_CLOSED;
        else *status_out = TCP_STATUS_SUCCESS;
    }
    return count;
}

bool tcp_close(tcp_connection_t *connection_ptr, u64 timeout_ms,
               tcp_interrupt_check_t interrupted, void *interrupt_context,
               tcp_status_t *status_out)
{
    struct tcp_connection *c = connection_ptr;
    u64 deadline = timer_uptime_ms() + timeout_ms;
    if (status_out) *status_out = TCP_STATUS_INVALID_STATE;
    if (!c || c != &connection) return false;

    u64 flags = tcp_lock();
    if (!c->in_use) {
        tcp_unlock(flags);
        if (status_out) *status_out = TCP_STATUS_CLOSED;
        return false;
    }
    tcp_state_t state = c->state;
    tcp_status_t status = c->status;
    tcp_unlock(flags);
    if (status != TCP_STATUS_SUCCESS || state == TCP_STATE_CLOSED) {
        tcp_abort(c);
        if (status_out) *status_out = status;
        return status == TCP_STATUS_SUCCESS;
    }
    if (timeout_ms == 0) {
        if (status_out) *status_out = TCP_STATUS_WOULD_BLOCK;
        return false;
    }
    if ((state == TCP_STATE_ESTABLISHED || state == TCP_STATE_CLOSE_WAIT) &&
        !tcp_wait_condition(c, deadline, true, false, false,
                            interrupted, interrupt_context, status_out)) {
        tcp_abort(c);
        return false;
    }

    tcp_packet_t packet;
    flags = tcp_lock();
    if (!c->in_use || c->tx_outstanding || c->status != TCP_STATUS_SUCCESS) {
        tcp_status_t failed = c->status != TCP_STATUS_SUCCESS ? c->status :
                              TCP_STATUS_INVALID_STATE;
        tcp_unlock(flags);
        if (status_out) *status_out = failed;
        tcp_abort(c);
        return false;
    }
    state = c->state;
    if (state == TCP_STATE_ESTABLISHED) c->state = TCP_STATE_FIN_WAIT_1;
    else if (state == TCP_STATE_CLOSE_WAIT) c->state = TCP_STATE_LAST_ACK;
    else if (state != TCP_STATE_FIN_WAIT_1 && state != TCP_STATE_FIN_WAIT_2 &&
             state != TCP_STATE_LAST_ACK && state != TCP_STATE_TIME_WAIT) {
        tcp_unlock(flags);
        if (status_out) *status_out = TCP_STATUS_INVALID_STATE;
        tcp_abort(c);
        return false;
    }
    if (state == TCP_STATE_ESTABLISHED || state == TCP_STATE_CLOSE_WAIT) {
        u32 sequence = c->snd_nxt;
        tcp_track_locked(c, sequence, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
        c->snd_nxt++;
        tcp_packet_from_connection_locked(c, &packet, sequence,
                                          TCP_FLAG_FIN | TCP_FLAG_ACK,
                                          NULL, 0);
    }
    tcp_unlock(flags);
    if (state == TCP_STATE_ESTABLISHED || state == TCP_STATE_CLOSE_WAIT)
        (void)tcp_transmit_packet(&packet);

    for (;;) {
        tcp_service(c);
        flags = tcp_lock();
        status = c->status;
        state = c->state;
        tcp_unlock(flags);
        if (status != TCP_STATUS_SUCCESS || state == TCP_STATE_CLOSED) break;
        if (interrupted && interrupted(interrupt_context)) {
            if (status_out) *status_out = TCP_STATUS_INTERRUPTED;
            tcp_abort(c);
            return false;
        }
        if (timer_uptime_ms() >= deadline) {
            if (status_out) *status_out = TCP_STATUS_TIMEOUT;
            tcp_abort(c);
            return false;
        }
        (void)scheduler_sleep(1);
    }
    if (state == TCP_STATE_TIME_WAIT) {
        while (timer_uptime_ms() < deadline) {
            tcp_service(c);
            flags = tcp_lock();
            state = c->state;
            status = c->status;
            tcp_unlock(flags);
            if (state == TCP_STATE_CLOSED || status != TCP_STATUS_SUCCESS) break;
            if (interrupted && interrupted(interrupt_context)) {
                if (status_out) *status_out = TCP_STATUS_INTERRUPTED;
                tcp_abort(c);
                return false;
            }
            (void)scheduler_sleep(1);
        }
    }
    flags = tcp_lock();
    status = c->status;
    state = c->state;
    if (state == TCP_STATE_CLOSED) c->in_use = false;
    tcp_unlock(flags);
    if (status_out) *status_out = status;
    if (state != TCP_STATE_CLOSED || status != TCP_STATUS_SUCCESS) {
        tcp_abort(c);
        return false;
    }
    return true;
}

void tcp_abort(tcp_connection_t *connection_ptr)
{
    if (connection_ptr != &connection) return;
    u64 flags = tcp_lock();
    connection = (struct tcp_connection){0};
    tcp_unlock(flags);
}

tcp_state_t tcp_connection_state(const tcp_connection_t *connection_ptr)
{
    if (connection_ptr != &connection) return TCP_STATE_CLOSED;
    u64 flags = tcp_lock();
    tcp_state_t state = connection.state;
    tcp_unlock(flags);
    return state;
}
