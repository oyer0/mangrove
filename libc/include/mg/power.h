#pragma once

#include <mg/types.h>

enum mg_battery_state {
    MG_BATTERY_STATE_UNKNOWN = 0,
    MG_BATTERY_STATE_DISCHARGING = 1,
    MG_BATTERY_STATE_CHARGING = 2,
    MG_BATTERY_STATE_IDLE = 3,
    MG_BATTERY_STATE_CRITICAL = 4,
};

enum mg_temperature_sensor {
    MG_TEMPERATURE_SENSOR_NONE = 0,
    MG_TEMPERATURE_SENSOR_CPU_PACKAGE = 1,
};

typedef struct {
    bool available;
    u8 sensor;
    u8 reserved[2];
    i32 millidegrees_celsius;
} mg_temperature_t;

typedef struct {
    bool available;
    bool battery_present;
    bool battery_percent_valid;
    u8 battery_percent;
    u8 battery_state;
    bool ac_available;
    bool ac_connected;
    u8 reserved[2];
    u32 remaining_capacity;
    u32 full_charge_capacity;
    mg_temperature_t temperature;
} mg_power_status_t;
