#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

static uint16_t checksum(const uint8_t *p, size_t n)
{
    uint32_t sum = 0;
    while (n >= 2) { sum += ((uint32_t)p[0] << 8) | p[1]; p += 2; n -= 2; }
    if (n) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)~sum;
}

int main(void)
{
    uint8_t packet[22] = {10,0,2,15,10,0,2,2,0,17,0,10,
                          0,68,0,67,0,10,0,0,1,2};
    uint16_t sum = checksum(packet, sizeof(packet));
    assert(sum != 0);
    packet[18] = (uint8_t)(sum >> 8);
    packet[19] = (uint8_t)sum;
    assert(checksum(packet, sizeof(packet)) == 0);
    puts("UDP checksum test passed");
    return 0;
}
