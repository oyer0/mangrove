#pragma once

#include <types.h>

u16 net_checksum(const void *data, usize length);
bool net_checksum_valid(const void *data, usize length);
