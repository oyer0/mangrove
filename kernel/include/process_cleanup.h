/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Kernel-private notifications; bits carry no borrowed object pointers. */
#define PROCESS_CLEANUP_WORKER_REAP (1U << 0)
#define PROCESS_CLEANUP_SESSION_PROGRESS (1U << 1)
#define PROCESS_CLEANUP_COLLECTION (1U << 2)
#define PROCESS_CLEANUP_EXIT (1U << 3)
void process_cleanup_request(unsigned int work);
