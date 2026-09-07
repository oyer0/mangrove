/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <mg/types.h>
#include <mg/error.h>
#include <mg/object.h>
#include <mg/process.h>
#include <mg/memory.h>
#include <mg/filesystem.h>
#include <mg/net.h>
#include <mg/network_service.h>
#include <mg/power.h>
#include <mg/identity.h>
#include <mg/account.h>
#include <mg/session.h>
#include <mg/service.h>
#include <mg/ipc.h>
#include <mg/device_service.h>
#include <mg/inspection.h>
#include <mg/log_service.h>
#include <mg/time.h>
#include <mg/terminal.h>

u64 uptime_ms(void);
mg_result_t system_poweroff(void);
mg_result_t system_reboot(void);
mg_result_t power_status(mg_power_status_t *status);
