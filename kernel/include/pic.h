/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

/* Keep the compatibility PIC silent while APIC routing is active. */
void pic_disable(void);
