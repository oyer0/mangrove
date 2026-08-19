#pragma once

#include <types.h>

#define BLOCK_MAX_DEVICES 32

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

struct block_device
{
    u64 id;

    block_device_type_t type;

    u32 sector_size;
    u64 sector_count;

    block_read_t read;
    block_write_t write;

    void *driver_data;
};

bool block_register(block_device_t *device);

u32 block_device_count(void);
block_device_t *block_get_device(u32 index);

bool block_read(
    block_device_t *device,
    u64 lba,
    u32 sector_count,
    void *buffer);

bool block_write(
    block_device_t *device,
    u64 lba,
    u32 sector_count,
    const void *buffer);

const char *block_type_name(block_device_type_t type);
