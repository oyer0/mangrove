#include <net/dns.h>

static u16 dns_read16(const u8 *p)
{
    return ((u16)p[0] << 8) | p[1];
}

bool dns_encode_hostname(const char *hostname, u8 *output, usize capacity,
                         usize *encoded_length)
{
    usize input = 0, out = 0, label_start, label_length, length_position;

    if (!hostname || !output || !encoded_length || capacity == 0) return false;
    label_start = 0;
    for (;;) {
        input = label_start;
        while (hostname[input] && hostname[input] != '.') {
            if ((u8)hostname[input] < 0x21 || (u8)hostname[input] > 0x7e) return false;
            input++;
            if (input > 253) return false;
        }
        label_length = input - label_start;
        if (label_length == 0 || label_length > 63 || out + label_length + 2 > capacity)
            return false;
        length_position = out++;
        for (usize i = 0; i < label_length; i++) output[out++] = (u8)hostname[label_start + i];
        output[length_position] = (u8)label_length;
        if (!hostname[input]) break;
        label_start = input + 1;
    }
    if (out >= capacity) return false;
    output[out++] = 0;
    if (out > 255) return false;
    *encoded_length = out;
    return true;
}

/* Decode/skip a possibly compressed name.  The caller's offset advances only
 * through the original stream; pointers are followed in a bounded walk. */
static bool dns_name(const u8 *packet, usize length, usize *offset)
{
    usize position, next;
    bool jumped = false;
    u32 jumps = 0;
    if (!packet || !offset || *offset >= length) return false;
    position = *offset;
    next = position;
    for (;;) {
        u8 size;
        if (position >= length || jumps++ > 32) return false;
        size = packet[position];
        if (size == 0) {
            if (!jumped) next = position + 1;
            *offset = next;
            return true;
        }
        if ((size & 0xc0) == 0xc0) {
            u16 pointer;
            if (position + 1 >= length) return false;
            pointer = (u16)(((u16)(size & 0x3f) << 8) | packet[position + 1]);
            if (pointer >= length) return false;
            if (!jumped) next = position + 2;
            jumped = true;
            position = pointer;
            continue;
        }
        if (size & 0xc0 || size > 63 || position + 1 + size > length) return false;
        position += 1 + size;
    }
}

static bool dns_question(const u8 *packet, usize length, usize *offset)
{
    if (!dns_name(packet, length, offset) || *offset + 4 > length) return false;
    *offset += 4;
    return true;
}

static bool dns_rr(const u8 *packet, usize length, usize *offset,
                   net_ipv4_t *address, bool *is_a)
{
    u16 type, class_code, data_length;
    usize data_offset;
    if (!dns_name(packet, length, offset) || *offset + 10 > length) return false;
    type = dns_read16(packet + *offset);
    class_code = dns_read16(packet + *offset + 2);
    data_length = dns_read16(packet + *offset + 8);
    *offset += 10;
    data_offset = *offset;
    if (data_offset + data_length > length) return false;
    if (type == 5 && class_code == 1) {
        usize cname_end = data_offset;
        if (!dns_name(packet, data_offset + data_length, &cname_end) ||
            cname_end > data_offset + data_length) return false;
    }
    *is_a = type == 1 && class_code == 1 && data_length == 4;
    if (*is_a && address) {
        address->octet[0] = packet[data_offset];
        address->octet[1] = packet[data_offset + 1];
        address->octet[2] = packet[data_offset + 2];
        address->octet[3] = packet[data_offset + 3];
    }
    *offset = data_offset + data_length;
    return true;
}

dns_status_t dns_parse_response(const u8 *packet, usize length, u16 query_id,
                                net_ipv4_t *address_out, u32 *answer_count)
{
    u16 flags, questions, answers, authority, additional;
    usize offset = 12;
    u32 found = 0;
    net_ipv4_t first = {{0}};
    if (answer_count) *answer_count = 0;
    if (!packet || length < 12 || dns_read16(packet) != query_id) return DNS_STATUS_INVALID;
    flags = dns_read16(packet + 2);
    if (!(flags & 0x8000) || ((flags >> 11) & 0xf) != 0) return DNS_STATUS_INVALID;
    if ((flags & 0xf) == 3) return DNS_STATUS_NXDOMAIN;
    if ((flags & 0xf) == 2) return DNS_STATUS_SERVFAIL;
    if (flags & 0xf) return DNS_STATUS_INVALID;
    questions = dns_read16(packet + 4);
    answers = dns_read16(packet + 6);
    authority = dns_read16(packet + 8);
    additional = dns_read16(packet + 10);
    if (questions == 0) return DNS_STATUS_INVALID;
    for (u16 i = 0; i < questions; i++) if (!dns_question(packet, length, &offset)) return DNS_STATUS_INVALID;
    for (u16 i = 0; i < answers; i++) {
        net_ipv4_t candidate;
        bool is_a;
        if (!dns_rr(packet, length, &offset, &candidate, &is_a)) return DNS_STATUS_INVALID;
        if (is_a) { if (!found) first = candidate; found++; }
    }
    for (u16 i = 0; i < authority; i++) if (!dns_rr(packet, length, &offset, 0, &(bool){false})) return DNS_STATUS_INVALID;
    for (u16 i = 0; i < additional; i++) if (!dns_rr(packet, length, &offset, 0, &(bool){false})) return DNS_STATUS_INVALID;
    if (answer_count) *answer_count = found;
    if (!found) return DNS_STATUS_NO_ANSWER;
    if (address_out) *address_out = first;
    return DNS_STATUS_SUCCESS;
}
