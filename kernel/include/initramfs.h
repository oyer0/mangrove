#pragma once

#include <vfs.h>

/* Registers the "initramfs" filesystem driver with the VFS */
int initramfs_init(void);
