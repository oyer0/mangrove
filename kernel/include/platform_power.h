#pragma once

#include <aml.h>
#include <types.h>
#include <mg/power.h>

/* Requests an ACPI S5 transition.  Negative results leave the machine
 * running; a successful request does not return. */
i64 platform_poweroff(void);

/* Requests a platform reset. A successful hardware reset does not return. */
i64 platform_reboot(void);

/* Performs a fresh read-only ACPI power-status query. */
i64 platform_power_status(mg_power_status_t *status);

typedef enum {
    PLATFORM_LID_UNKNOWN = 0,
    PLATFORM_LID_OPEN,
    PLATFORM_LID_CLOSED,
} platform_lid_state_t;

/* The platform layer owns the current lid state.  No firmware paths, GPE
 * numbers, or EC offsets are exposed outside the kernel. */
bool platform_lid_initialize(void);
bool platform_lid_available(void);
platform_lid_state_t platform_lid_state(void);

/* Called by the AML Notify path.  It only records work and is not allowed to
 * evaluate AML, access the display, or block. */
void platform_power_acpi_notify(aml_handle_t target, u64 value);

/* Evaluates a pending lid notification and applies the current policy from
 * normal kernel context. */
void platform_power_process_lid_events(void);
