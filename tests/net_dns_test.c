#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long usize;
typedef _Bool bool;
typedef struct { u8 octet[4]; } net_ipv4_t;
typedef enum { DNS_STATUS_SUCCESS = 0, DNS_STATUS_TIMEOUT, DNS_STATUS_NXDOMAIN,
               DNS_STATUS_SERVFAIL, DNS_STATUS_NO_ANSWER, DNS_STATUS_INVALID,
               DNS_STATUS_UNCONFIGURED } dns_status_t;
bool dns_encode_hostname(const char *, u8 *, usize, usize *);
dns_status_t dns_parse_response(const u8 *, usize, u16, net_ipv4_t *, u32 *);

static void be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

int main(void)
{
    u8 name[256], packet[64] = {0};
    usize length;
    net_ipv4_t address = {{0}};
    u32 answers = 0;
    assert(dns_encode_hostname("example.com", name, sizeof(name), &length));
    assert(length == 13 && name[0] == 7 && !memcmp(name + 1, "example", 7));
    assert(name[8] == 3 && !memcmp(name + 9, "com", 3) && name[12] == 0);
    assert(!dns_encode_hostname("example..com", name, sizeof(name), &length));
    assert(!dns_encode_hostname("", name, sizeof(name), &length));

    be16(packet + 0, 0x1234); be16(packet + 2, 0x8180);
    be16(packet + 4, 1); be16(packet + 6, 1);
    packet[12] = 7; memcpy(packet + 13, "example", 7);
    packet[20] = 3; memcpy(packet + 21, "com", 3); packet[24] = 0;
    be16(packet + 25, 1); be16(packet + 27, 1);
    packet[29] = 0xc0; packet[30] = 0x0c; be16(packet + 31, 1);
    be16(packet + 33, 1); packet[35] = packet[36] = packet[37] = packet[38] = 0;
    packet[39] = 0; packet[40] = 4;
    packet[41] = 93; packet[42] = 184; packet[43] = 216; packet[44] = 34;
    assert(dns_parse_response(packet, 45, 0x1234, &address, &answers) == DNS_STATUS_SUCCESS);
    assert(answers == 1 && address.octet[0] == 93 && address.octet[3] == 34);
    assert(dns_parse_response(packet, 45, 0x4321, &address, &answers) == DNS_STATUS_INVALID);
    assert(dns_parse_response(packet, 44, 0x1234, &address, &answers) == DNS_STATUS_INVALID);
    packet[29] = 0xc0; packet[30] = 29; /* compression pointer loop */
    assert(dns_parse_response(packet, 45, 0x1234, &address, &answers) == DNS_STATUS_INVALID);
    packet[29] = 0xc0; packet[30] = 0xff; /* pointer outside packet */
    assert(dns_parse_response(packet, 45, 0x1234, &address, &answers) == DNS_STATUS_INVALID);
    packet[2] = 0x85; packet[3] = 0x83;
    assert(dns_parse_response(packet, 45, 0x1234, &address, &answers) == DNS_STATUS_NXDOMAIN);
    puts("DNS wire tests passed");
    return 0;
}
