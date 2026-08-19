#include <net/checksum.h>

u16 net_checksum(const void *data, usize length)
{
    const u8 *bytes = (const u8 *)data;
    u32 sum = 0;

    if (!data && length != 0) return 0;
    while (length >= 2) {
        sum += ((u32)bytes[0] << 8) | bytes[1];
        bytes += 2;
        length -= 2;
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    if (length != 0) sum += (u32)bytes[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFU) + (sum >> 16);
    return (u16)~sum;
}

bool net_checksum_valid(const void *data, usize length)
{
    return data && net_checksum(data, length) == 0;
}
