/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

const char *pci_vendor_name(u16 vendor_id);
const char *pci_device_name(u16 vendor_id, u16 device_id);
