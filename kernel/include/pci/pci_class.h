/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

#define PCI_CLASS_DISPLAY 0x03U

const char *pci_class_name(u8 class_code, u8 subclass, u8 prog_if);
