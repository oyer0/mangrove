#pragma once

#include <types.h>

/* Returns true only after ACPI described the EC interface and a bounded
 * status-register access succeeded. */
bool ec_available(void);

/* Read the EC host-interface status register without touching EC RAM. */
bool ec_status(u8 *status);

/* Access one byte in the ACPI EC address space.  Callers must use only
 * offsets whose meaning is provided by a later platform-specific ACPI
 * consumer; Stage 18.6 itself performs no arbitrary RAM access. */
bool ec_read(u8 offset, u8 *value);
bool ec_write(u8 offset, u8 value);

/* Discover and validate the ACPI EC interface during early platform setup. */
bool ec_init(void);
