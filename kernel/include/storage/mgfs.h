#pragma once

#include <vfs.h>

/* Registers the read-only MGFS v1 filesystem driver with the VFS. */
int mgfs_init(void);

/* Returns the most recent MGFS mount/probe diagnostic. */
const char *mgfs_last_error(void);
