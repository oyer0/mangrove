#include <stdio.h>
#include <string.h>

#include <net/checksum.h>

static int check(int condition, const char *name)
{
    if (!condition) fprintf(stderr, "checksum test failed: %s\n", name);
    return condition;
}

int main(void)
{
    const u8 even[] = {0x00, 0x01, 0xF2, 0x03};
    const u8 odd[] = {0x01, 0x02, 0x03};
    u8 packet[20];
    u8 icmp[] = {8, 0, 0, 0, 0x12, 0x34, 0, 1, 'M', 'a', 'n', 'g', 'r', 'o', 'v', 'e'};
    u16 value;
    int ok = 1;

    ok &= check(net_checksum(even, sizeof(even)) == 0x0DFB, "even vector");
    ok &= check(net_checksum(odd, sizeof(odd)) == 0xFBFD, "odd vector");
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x45;
    packet[2] = 0;
    packet[3] = sizeof(packet);
    packet[8] = 64;
    packet[9] = 1;
    value = net_checksum(packet, sizeof(packet));
    packet[10] = (u8)(value >> 8);
    packet[11] = (u8)value;
    ok &= check(net_checksum_valid(packet, sizeof(packet)), "IPv4 validation");
    value = net_checksum(icmp, sizeof(icmp));
    icmp[2] = (u8)(value >> 8);
    icmp[3] = (u8)value;
    ok &= check(net_checksum_valid(icmp, sizeof(icmp)), "ICMP validation");
    if (ok) puts("network checksum tests passed");
    return ok ? 0 : 1;
}
