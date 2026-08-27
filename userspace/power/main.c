#include <mangrove.h>
#include <stdio.h>

static const char *battery_state(u8 state)
{
    switch (state) {
        case MG_BATTERY_STATE_CHARGING: return "charging";
        case MG_BATTERY_STATE_DISCHARGING: return "discharging";
        case MG_BATTERY_STATE_IDLE: return "idle";
        case MG_BATTERY_STATE_CRITICAL: return "critical";
        default: return "unknown";
    }
}

int main(int argc, char **argv)
{
    mg_power_status_t status;
    mg_result_t result;

    if (argc != 1) {
        printf("Usage: power\n");
        return 1;
    }
    result = power_status(&status);
    if (result < 0 || !status.available) {
        printf("Power information unavailable.\n");
        return result < 0 ? 1 : 0;
    }
    if (!status.battery_present) {
        printf("Battery: unavailable\n");
    } else if (status.battery_percent_valid) {
        printf("Battery: %u%%\n", status.battery_percent);
        printf("State: %s\n", battery_state(status.battery_state));
    } else {
        printf("Battery: unknown\n");
        printf("State: %s\n", battery_state(status.battery_state));
    }
    if (!status.ac_available)
        printf("AC: unknown\n");
    else
        printf("AC: %s\n", status.ac_connected ? "connected" :
               "disconnected");
    if (status.temperature.available &&
        status.temperature.sensor == MG_TEMPERATURE_SENSOR_CPU_PACKAGE)
        printf("Temperature: %d C\n",
               status.temperature.millidegrees_celsius / 1000);
    return 0;
}
