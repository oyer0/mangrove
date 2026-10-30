/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include "../../kernel/include/process_cleanup.h"

/* Isolate notification producers in fixtures concerned with other ownership
 * boundaries. The autonomous-worker fixture executes the real consumer. */
static unsigned int process_cleanup_fixture_work;
void process_cleanup_request(unsigned int work)
{ process_cleanup_fixture_work |= work; }
/* Avoid selecting host libc features before a fixture defines _GNU_SOURCE. */
static __UINT64_TYPE__ process_handoff_release_after __attribute__((unused));
static __UINT64_TYPE__ process_reference_release_after __attribute__((unused));
static __UINT64_TYPE__ process_handoff_release_limit __attribute__((unused));
static __UINT64_TYPE__ process_reference_release_limit __attribute__((unused));
#ifndef PROCESS_FIXTURE_REAL_COLLECTION_LATCH
static bool process_session_collection_retry_pending __attribute__((unused));
#endif
static unsigned int process_cleanup_work_pending __attribute__((unused));
static bool process_worker_reap_sweep_active __attribute__((unused));
static __attribute__((unused)) bool process_cleanup_worker_reap_once(void) { return false; }

/* Session-domain consumer is a boundary mock in non-session fixtures. */
void session_cleanup_progress(void) {}
