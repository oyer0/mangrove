/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/* Initializes the QEMU-compatible Intel 82540EM/E1000 PCI controller. */
bool e1000_init(void);
/* Reconciles supported controller instances with the current PCI snapshot. */
bool e1000_rescan(void);
u64 e1000_received_frames(void);
u64 e1000_transmitted_frames(void);
