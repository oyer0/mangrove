/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/types.h>

/* Opens one optional path and runs the full-screen Reed editor. */
int reed_run(const char *pathname);

#if defined(REED_GUEST_CONFLICT_SYNC)
/* Test-build-only switch for a bounded in-guest stale-save rendezvous. */
int reed_guest_conflict_sync_enable(const char *run_id);
#endif
