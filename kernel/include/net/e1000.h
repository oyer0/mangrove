#pragma once

#include <types.h>

/* Initializes the QEMU-compatible Intel 82540EM/E1000 PCI controller. */
bool e1000_init(void);
u64 e1000_received_frames(void);
u64 e1000_transmitted_frames(void);
