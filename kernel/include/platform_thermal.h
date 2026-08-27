#pragma once

#include <types.h>

typedef enum {
    PLATFORM_TEMPERATURE_SENSOR_NONE = 0,
    PLATFORM_TEMPERATURE_SENSOR_CPU_PACKAGE = 1,
} platform_temperature_sensor_t;

typedef struct {
    bool available;
    platform_temperature_sensor_t sensor;
    i32 millidegrees_celsius;
} platform_temperature_t;

/* Discovers a supported read-only platform sensor.  Discovery is lazy so
 * platforms without one do not acquire a new boot dependency. */
bool platform_thermal_initialize(void);

/* Reads a fresh sample from the discovered sensor. */
bool platform_thermal_read(platform_temperature_t *temperature);
