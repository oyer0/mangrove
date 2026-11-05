/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <types.h>

/* Optional boot transport. Called once after ACPI, the monotonic clock and
 * I/O APIC are ready, before CPU interrupts are enabled. On failure only the
 * keyboard port is quiesced. Its shared receiver owns IRQ1 and IRQ12; the
 * auxiliary clock and mouse protocol are not reconfigured. */
bool ps2_keyboard_init(const char **reason);
