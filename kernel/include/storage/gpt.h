#pragma once

#include <block.h>

/* Scan a 512-byte block device and register its GPT partitions. */
bool gpt_scan_device(block_device_t *device);
