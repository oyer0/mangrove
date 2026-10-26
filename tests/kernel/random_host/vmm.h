/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <types.h>
bool vmm_user_range_writable(const void *address, usize length);
