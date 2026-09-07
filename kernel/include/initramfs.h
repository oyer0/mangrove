/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <vfs.h>

/* Registers the "initramfs" filesystem driver with the VFS */
int initramfs_init(void);
