/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <block.h>

/* Native exFAT filesystem driver and bounded formatter support. */
int exfat_init(void);
bool exfat_read_label(block_device_t *dev, char *out, usize capacity);
bool exfat_format_device(block_device_t *dev);
/* Exclusive unmounted mutation; VFS_ERR_UNCERTAIN retains registry ownership. */
int exfat_set_label(block_device_t *dev, const char *label);
int exfat_set_label_for_id(block_device_t *dev, u64 expected_id, const char *label);
