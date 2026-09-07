/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/types.h>

/* Reserved kernel-reported result for a process terminated by a Ring 3 CPU
 * exception.  It is distinct from ordinary successful/failed program exits. */
#define MG_PROCESS_STATUS_CRASHED ((i32)-128)
