/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <vfs.h>

/* Registers the "initramfs" filesystem driver with the VFS */
int initramfs_init(void);
