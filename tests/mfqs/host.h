/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <vfs.h>

/* Single-threaded host services, not filesystem implementations. */
bool mfqs_media_open(const char *path);
bool mfqs_media_reopen(void);
void mfqs_media_close(void);
block_device_t *mfqs_device(void);
bool mfqs_heap_contains(const void *pointer);
void mfqs_heap_release_nodes(vfs_node_t **nodes, unsigned count);
typedef struct {
    u64 reads, writes, flushes, heap_requests, heap_peak, heap_live;
} mfqs_metrics_t;
void mfqs_metrics(mfqs_metrics_t *out);
