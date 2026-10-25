/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* The initial native argv is installed in one 4 KiB user-stack page. Reserve
 * one 64-bit pointer per argument and one NULL sentinel before admitting the
 * aggregate bytes occupied by argument strings. */
#define MG_PROCESS_MAX_ARGUMENTS 16U
#define MG_PROCESS_ARGUMENT_BYTES_MAX \
    (4096U - ((MG_PROCESS_MAX_ARGUMENTS + 1U) * 8U))
