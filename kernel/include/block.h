/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>

#define BLOCK_MAX_DEVICES 32
#define BLOCK_CACHE_ENTRY_COUNT 64
#define BLOCK_CACHE_ENTRY_BYTES 4096U
#define BLOCK_DEVICE_ID_BASE 0x2000000000000000ULL
#define BLOCK_CONNECTION_MAX 16U
#define BLOCK_MODEL_MAX 64U

typedef enum
{
    BLOCK_DEVICE_SATA,
    BLOCK_DEVICE_NVME,
    BLOCK_DEVICE_USB,
    BLOCK_DEVICE_RAM,
    BLOCK_DEVICE_PARTITION,
} block_device_type_t;

typedef struct block_device block_device_t;

typedef bool (*block_read_t)(
    block_device_t *device,
    u64 lba,
    u32 sector_count,
    void *buffer);

typedef bool (*block_write_t)(
    block_device_t *device,
    u64 lba,
    u32 sector_count,
    const void *buffer);

/* Complete all previously accepted writes for this exact block instance.
 * Backends without a hardware cache may omit the callback: their synchronous
 * write contract is already the strongest completion guarantee available. */
typedef bool (*block_flush_t)(block_device_t *device);

struct block_device
{
    u64 id;

    block_device_type_t type;

    u32 sector_size;
    u64 sector_count;

    block_read_t read;
    block_write_t write;
    block_flush_t flush;

    void *driver_data;

    /* Bounded origin/model metadata for read-only inspection clients.  These
     * fields are copied into the registered block instance and never used as
     * lifecycle identity. */
    char connection[BLOCK_CONNECTION_MAX];
    char model[BLOCK_MODEL_MAX];

    /* Registry-owned lifecycle bits.  A device remains in its slot while
     * teardown drains, but no new I/O is admitted after online is cleared. */
    bool registered;
    bool online;
    bool read_only;
    /* Current-boot policy bit for the exact removable device instance.  It
     * is deliberately reset when the registry object is replaced. */
    bool automount_suppressed;
};

bool block_register(block_device_t *device);
bool block_unregister_by_id(u64 id);
bool block_unregister_by_driver_data(void *driver_data);
bool block_device_mark_gone(u64 id);
bool block_device_id_is_live(u64 id);
bool block_device_is_live(const block_device_t *device);
bool block_device_set_automount_suppressed(u64 id, bool suppressed);
bool block_device_automount_suppressed(u64 id);

/* Hold the bounded block-I/O gate exclusively while a removable driver
 * invalidates its device.  This prevents a callback from racing teardown. */
bool block_io_begin_quiesce(void);
void block_io_end_quiesce(void);

u32 block_device_count(void);
block_device_t *block_get_device(u32 index);
/* Resolve the immutable public instance ID used by snapshot clients. */
block_device_t *block_get_device_by_public_id(u64 public_id);

bool block_read(
    block_device_t *device,
    u64 lba,
    u32 sector_count,
    void *buffer);

/* Boot/runtime discovery may need to inspect a driver-owned descriptor before
 * the complete device is published in the registry (for example while
 * scanning its GPT).  This is a bounded kernel-only probe path; normal I/O
 * must use block_read(), which requires a live registry identity. */
bool block_probe_read(
    block_device_t *device,
    u64 lba,
    u32 sector_count,
    void *buffer);

bool block_write(
    block_device_t *device,
    u64 lba,
    u32 sector_count,
    const void *buffer);
/* Complete writes through the backend's explicit flush operation where one
 * exists.  The call is generation-safe and serialized with normal I/O. */
bool block_flush(block_device_t *device);

typedef struct {
    u64 read_requests;
    u64 device_reads;
    u64 cache_hits;
    u64 cache_misses;
    u64 write_requests;
} block_io_stats_t;

void block_io_stats_reset(void);
void block_io_stats_get(block_io_stats_t *out_stats);

const char *block_type_name(block_device_type_t type);
