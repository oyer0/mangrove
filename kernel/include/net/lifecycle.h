/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/* Starts the bounded kernel lifecycle worker used to discover supported
 * Ethernet PCI functions after boot.  The worker is a recovery/probing path;
 * normal packet and link work remains interrupt driven. */
bool net_lifecycle_init(void);
