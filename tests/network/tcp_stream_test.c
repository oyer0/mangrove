/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include <net/config.h>
#include <net/ipv4.h>
#include <net/tcp.h>
#include <scheduler.h>
#include <spinlock.h>
#include <timer.h>

#define TEST_TCP_RETRIES 3U

static net_device_t device = {.generation = 1};
static net_config_t configuration = {
    .address = {{10, 0, 2, 15}}, .configured = true
};
static net_ipv4_t peer = {{10, 0, 2, 2}};
static u64 now_ms;
static unsigned transmissions;
static unsigned data_transmissions;
static unsigned sleeps_before_ack = 1;
static unsigned sleeps_since_data;
static bool auto_syn_ack = true;
static bool auto_fin_response = true;
static u8 last_packet[TCP_HEADER_MIN_SIZE + TCP_MSS];
static usize last_packet_length;
static u32 data_sequences[16];
static usize data_lengths[16];
static u32 data_history_count;

static u16 read_be16(const u8 *p) { return (u16)((u16)p[0] << 8 | p[1]); }
static u32 read_be32(const u8 *p)
{
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}
static void write_be16(u8 *p, u16 value)
{
    p[0] = (u8)(value >> 8); p[1] = (u8)value;
}
static void write_be32(u8 *p, u32 value)
{
    p[0] = (u8)(value >> 24); p[1] = (u8)(value >> 16);
    p[2] = (u8)(value >> 8); p[3] = (u8)value;
}

u64 timer_ticks(void) { return now_ms; }
u64 timer_uptime_ms(void) { return now_ms; }

void spinlock_init(spinlock_t *lock)
{
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
}
u64 spin_lock_irqsave(spinlock_t *lock)
{
    while (__atomic_exchange_n(&lock->value, 1U, __ATOMIC_ACQUIRE)) { }
    return 0;
}
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    (void)flags;
    __atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
}

const net_config_t *net_config(void) { return &configuration; }
bool net_device_instance_current(const net_device_t *candidate, u64 generation)
{
    return candidate == &device && generation == device.generation;
}
bool net_ipv4_equal(net_ipv4_t left, net_ipv4_t right)
{
    for (u32 i = 0; i < 4; i++)
        if (left.octet[i] != right.octet[i]) return false;
    return true;
}
u16 net_checksum(const void *bytes, usize length)
{
    (void)bytes; (void)length; return 0x4c2d;
}

static void incoming(net_ipv4_t source, net_ipv4_t destination, u32 sequence,
                     u32 acknowledgment, u8 flags, u16 window,
                     const u8 *payload, usize payload_length)
{
    u8 packet[TCP_HEADER_MIN_SIZE + TCP_MSS] = {0};
    assert(payload_length <= TCP_MSS);
    write_be16(packet + 0, read_be16(last_packet + 2));
    write_be16(packet + 2, read_be16(last_packet + 0));
    write_be32(packet + 4, sequence);
    write_be32(packet + 8, acknowledgment);
    packet[12] = 5U << 4;
    packet[13] = flags;
    write_be16(packet + 14, window);
    for (usize i = 0; i < payload_length; i++)
        packet[TCP_HEADER_MIN_SIZE + i] = payload[i];
    write_be16(packet + 16, tcp_checksum(source, destination, packet,
                                        TCP_HEADER_MIN_SIZE + payload_length));
    tcp_receive(&device, source, destination, packet,
                TCP_HEADER_MIN_SIZE + payload_length);
}

static void acknowledge_last_data(void)
{
    u8 response[TCP_HEADER_MIN_SIZE] = {0};
    write_be16(response + 0, read_be16(last_packet + 2));
    write_be16(response + 2, read_be16(last_packet + 0));
    write_be32(response + 4, 7000U);
    write_be32(response + 8, read_be32(last_packet + 4) +
               (u32)(last_packet_length - TCP_HEADER_MIN_SIZE));
    response[12] = 5U << 4;
    response[13] = TCP_FLAG_ACK;
    write_be16(response + 14, 4096U);
    write_be16(response + 16,
               tcp_checksum(peer, configuration.address, response,
                            sizeof(response)));
    tcp_receive(&device, peer, configuration.address, response,
                sizeof(response));
}

bool scheduler_sleep(u64 ticks)
{
    now_ms += ticks;
    if (last_packet_length > TCP_HEADER_MIN_SIZE &&
        (last_packet[13] & (TCP_FLAG_SYN | TCP_FLAG_FIN)) == 0 &&
        ++sleeps_since_data >= sleeps_before_ack) {
        acknowledge_last_data();
        sleeps_since_data = 0;
    }
    return true;
}

bool ipv4_transmit_from(net_device_t *sender, net_ipv4_t source,
                        net_ipv4_t destination, u8 protocol,
                        const void *payload, usize length)
{
    const u8 *packet = payload;
    assert(sender == &device && protocol == TCP_PROTOCOL);
    assert(length <= sizeof(last_packet));
    for (usize i = 0; i < length; i++) last_packet[i] = packet[i];
    last_packet_length = length;
    transmissions++;
    if (length > TCP_HEADER_MIN_SIZE) {
        assert(data_history_count <
               sizeof(data_sequences) / sizeof(data_sequences[0]));
        data_sequences[data_history_count] = read_be32(packet + 4);
        data_lengths[data_history_count] = length - TCP_HEADER_MIN_SIZE;
        data_history_count++;
        data_transmissions++;
    }
    if ((packet[13] & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == TCP_FLAG_SYN) {
        if (!auto_syn_ack) return true;
        u8 response[TCP_HEADER_MIN_SIZE] = {0};
        write_be16(response + 0, read_be16(packet + 2));
        write_be16(response + 2, read_be16(packet + 0));
        write_be32(response + 4, 7000U);
        write_be32(response + 8, read_be32(packet + 4) + 1U);
        response[12] = 5U << 4;
        response[13] = TCP_FLAG_SYN | TCP_FLAG_ACK;
        write_be16(response + 14, 4096U);
        write_be16(response + 16,
                   tcp_checksum(destination, source, response, sizeof(response)));
        tcp_receive(sender, destination, source, response, sizeof(response));
    }
    if ((packet[13] & TCP_FLAG_FIN) && auto_fin_response) {
        /* A single FIN|ACK covers both close directions and allows the test to
         * observe TIME_WAIT expiration without a second networking thread. */
        incoming(destination, source, 7001U, read_be32(packet + 4) + 1U,
                 TCP_FLAG_FIN | TCP_FLAG_ACK, 4096U, NULL, 0);
    }
    return true;
}

static void reset_fixture(void)
{
    tcp_init();
    now_ms = 0;
    transmissions = 0;
    data_transmissions = 0;
    sleeps_since_data = 0;
    data_history_count = 0;
    last_packet_length = 0;
    auto_syn_ack = true;
    auto_fin_response = true;
    sleeps_before_ack = 1;
    device.generation = 1;
}

static bool always_interrupted(void *context)
{
    (void)context;
    return true;
}

int main(void)
{
    tcp_connection_t *stream = NULL;
    tcp_status_t status;
    u8 payload[TCP_MSS + 1U];
    for (usize i = 0; i < sizeof(payload); i++) payload[i] = (u8)i;

    /* The ACK for the first segment must arrive before the next segment is
     * submitted. The old ESTABLISHED-state wait returned immediately, making
     * this 1,461-byte write fail after only the first packet. */
    reset_fixture();
    assert(tcp_connect(&device, peer, 80, 100U, NULL, NULL,
                       &stream, &status));
    assert(status == TCP_STATUS_SUCCESS && stream != NULL);
    assert(tcp_send(stream, payload, sizeof(payload), 100U, NULL, NULL,
                    &status) == sizeof(payload));
    assert(status == TCP_STATUS_SUCCESS);
    assert(data_transmissions == 2U);
    assert(data_lengths[0] == TCP_MSS && data_lengths[1] == 1U);
    assert(data_sequences[1] == data_sequences[0] + TCP_MSS);
    tcp_abort(stream);

    /* A single segment is accepted into the retransmit queue immediately;
     * the next segment waits for its delayed ACK. */
    reset_fixture();
    assert(tcp_connect(&device, peer, 80, 100U, NULL, NULL,
                       &stream, &status));
    sleeps_before_ack = 2U;
    assert(tcp_send(stream, payload, 1U, 100U, NULL, NULL, &status) == 1U);
    assert(status == TCP_STATUS_SUCCESS && now_ms == 0U);
    assert(tcp_send(stream, payload + 1U, 1U, 100U, NULL, NULL,
                    &status) == 1U);
    assert(status == TCP_STATUS_SUCCESS && now_ms >= 2U);
    tcp_abort(stream);

    reset_fixture();
    assert(tcp_connect(&device, peer, 80, 100U, NULL, NULL,
                       &stream, &status));
    sleeps_before_ack = UINT_MAX;
    assert(tcp_send(stream, payload, sizeof(payload), 0U, NULL, NULL,
                    &status) == TCP_MSS);
    assert(status == TCP_STATUS_WOULD_BLOCK);
    tcp_abort(stream);

    /* Lost ACKs retransmit the exact outstanding segment. Exhaustion reports
     * timeout and does not let another connection reuse the live owner slot. */
    reset_fixture();
    assert(tcp_connect(&device, peer, 80, 100U, NULL, NULL,
                       &stream, &status));
    sleeps_before_ack = UINT_MAX;
    assert(tcp_send(stream, payload, 1U, 100U, NULL, NULL, &status) == 1U);
    assert(tcp_send(stream, payload + 1U, 1U, 5000U, NULL, NULL,
                    &status) == 0U);
    assert(status == TCP_STATUS_TIMEOUT);
    assert(data_transmissions == TEST_TCP_RETRIES + 1U);
    for (u32 i = 1; i < data_history_count; i++) {
        assert(data_sequences[i] == data_sequences[0]);
        assert(data_lengths[i] == data_lengths[0]);
    }
    tcp_connection_t *busy = NULL;
    assert(!tcp_connect(&device, peer, 80, 10U, NULL, NULL, &busy, &status));
    assert(status == TCP_STATUS_BUSY);
    tcp_abort(stream);

    /* Retransmitted/duplicate peer payload is not delivered a second time;
     * receive-buffer draining advertises the freed capacity. */
    reset_fixture();
    assert(tcp_connect(&device, peer, 80, 100U, NULL, NULL,
                       &stream, &status));
    static const u8 duplicate[] = {'o', 'k'};
    incoming(peer, configuration.address, 7001U, 0,
             TCP_FLAG_ACK | TCP_FLAG_PSH, 4096U, duplicate, sizeof(duplicate));
    incoming(peer, configuration.address, 7001U, 0,
             TCP_FLAG_ACK | TCP_FLAG_PSH, 4096U, duplicate, sizeof(duplicate));
    u8 received[4096];
    assert(tcp_receive_bytes(stream, received, sizeof(received), &status) == 2U);
    assert(status == TCP_STATUS_SUCCESS && received[0] == 'o' && received[1] == 'k');
    assert(tcp_receive_bytes(stream, received, sizeof(received), &status) == 0U);
    assert(status == TCP_STATUS_SUCCESS);

    u32 sequence = 7003U;
    usize remaining = sizeof(received);
    while (remaining) {
        usize chunk = remaining < TCP_MSS ? remaining : TCP_MSS;
        incoming(peer, configuration.address, sequence, 0,
                 TCP_FLAG_ACK | TCP_FLAG_PSH, 4096U, payload, chunk);
        sequence += (u32)chunk;
        remaining -= chunk;
    }
    incoming(peer, configuration.address, sequence, 0,
             TCP_FLAG_ACK | TCP_FLAG_PSH, 4096U, payload, 1U);
    assert(tcp_receive_bytes(stream, received, 100U, &status) == 100U);
    assert(status == TCP_STATUS_SUCCESS && read_be16(last_packet + 14) == 100U);
    tcp_abort(stream);

    /* A peer FIN closes only its sending half. The local side may still send
     * until it performs its own close. */
    reset_fixture();
    assert(tcp_connect(&device, peer, 80, 100U, NULL, NULL,
                       &stream, &status));
    incoming(peer, configuration.address, 7001U, 0,
             TCP_FLAG_FIN | TCP_FLAG_ACK, 4096U, NULL, 0);
    assert(tcp_connection_state(stream) == TCP_STATE_CLOSE_WAIT);
    assert(tcp_receive_bytes(stream, received, sizeof(received), &status) == 0U);
    assert(status == TCP_STATUS_CLOSED);
    assert(tcp_send(stream, payload, 1U, 100U, NULL, NULL, &status) == 1U);
    assert(status == TCP_STATUS_SUCCESS && data_transmissions == 1U);
    tcp_abort(stream);

    /* Graceful FIN, TIME_WAIT and reuse complete with bounded state handling. */
    reset_fixture();
    assert(tcp_connect(&device, peer, 80, 100U, NULL, NULL,
                       &stream, &status));
    assert(tcp_close(stream, 3000U, NULL, NULL, &status));
    assert(status == TCP_STATUS_SUCCESS);
    assert(tcp_connect(&device, peer, 80, 100U, NULL, NULL,
                       &stream, &status));
    tcp_abort(stream);

    /* A peer reset is surfaced to the next receive and retains the singleton
     * reservation until the owning stream is explicitly aborted. */
    reset_fixture();
    assert(tcp_connect(&device, peer, 80, 100U, NULL, NULL,
                       &stream, &status));
    incoming(peer, configuration.address, 7001U, 0, TCP_FLAG_RST,
             4096U, NULL, 0);
    u8 reset_buffer[8];
    assert(tcp_receive_bytes(stream, reset_buffer, sizeof(reset_buffer),
                             &status) == 0U);
    assert(status == TCP_STATUS_RESET);
    assert(!tcp_connect(&device, peer, 80, 10U, NULL, NULL, &busy, &status));
    assert(status == TCP_STATUS_BUSY);
    tcp_abort(stream);

    /* Close timeout and interruption abort the half-closed transport instead
     * of permanently reserving the only TCP slot. */
    reset_fixture();
    assert(tcp_connect(&device, peer, 80, 100U, NULL, NULL,
                       &stream, &status));
    auto_fin_response = false;
    assert(!tcp_close(stream, 3U, NULL, NULL, &status));
    assert(status == TCP_STATUS_TIMEOUT);
    assert(tcp_connect(&device, peer, 80, 100U, NULL, NULL,
                       &stream, &status));
    assert(!tcp_close(stream, 10U, always_interrupted, NULL, &status));
    assert(status == TCP_STATUS_INTERRUPTED);
    assert(tcp_connect(&device, peer, 80, 100U, NULL, NULL,
                       &stream, &status));
    tcp_abort(stream);

    /* Failed and interrupted connect attempts roll back their reservation. */
    reset_fixture();
    auto_syn_ack = false;
    assert(!tcp_connect(&device, peer, 80, 3U, NULL, NULL, &stream, &status));
    assert(status == TCP_STATUS_TIMEOUT);
    assert(!tcp_connect(&device, peer, 80, 100U, always_interrupted, NULL,
                        &stream, &status));
    assert(status == TCP_STATUS_INTERRUPTED);
    auto_syn_ack = true;
    assert(tcp_connect(&device, peer, 80, 100U, NULL, NULL,
                       &stream, &status));
    sleeps_before_ack = UINT_MAX;
    assert(tcp_send(stream, payload, sizeof(payload), 100U,
                    always_interrupted, NULL, &status) == TCP_MSS);
    assert(status == TCP_STATUS_INTERRUPTED);
    assert(tcp_send(stream, payload + TCP_MSS, 1U, 100U, NULL, NULL,
                    &status) == 0U);
    assert(status == TCP_STATUS_INTERRUPTED);
    tcp_abort(stream);

    /* Generation changes invalidate an existing connection without freeing
     * the owner slot until its handle aborts. */
    reset_fixture();
    assert(tcp_connect(&device, peer, 80, 100U, NULL, NULL,
                       &stream, &status));
    device.generation++;
    assert(tcp_receive_bytes(stream, received, sizeof(received), &status) == 0U);
    assert(status == TCP_STATUS_DEVICE_GONE);
    tcp_abort(stream);

    puts("TCP stream tests passed");
    return 0;
}
