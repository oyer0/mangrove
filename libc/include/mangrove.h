#pragma once

#include <mg/types.h>
#include <mg/error.h>
#include <mg/object.h>
#include <mg/process.h>
#include <mg/memory.h>
#include <mg/filesystem.h>
#include <mg/net.h>
#include <mg/power.h>

u64 uptime_ms(void);
mg_result_t system_poweroff(void);
mg_result_t system_reboot(void);
mg_result_t power_status(mg_power_status_t *status);
