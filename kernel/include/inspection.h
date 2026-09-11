/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/inspection.h>

/* Fill one bounded, copied machine-description record for userspace
 * inspection clients.  No kernel pointers escape this interface. */
void system_info_read(mg_system_info_t *output);
