#pragma once

#include <block.h>

/* Native exFAT filesystem driver and bounded formatter support. */
int exfat_init(void);
bool exfat_read_label(block_device_t *dev, char *out, usize capacity);
bool exfat_format_device(block_device_t *dev);
bool exfat_set_label(block_device_t *dev, const char *label);
