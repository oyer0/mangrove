#include <acpi.h>
#include <aml.h>
#include <kprint.h>
#include <mangrove_errors.h>
#include <mg/power.h>
#include <platform_power.h>
#include <platform_thermal.h>

#define ACPI_BATTERY_UNKNOWN 0xffffffffULL

#ifdef ACPI_POWER_DEBUG
#define POWER_DEBUG(...) kprint(__VA_ARGS__)
#else
#define POWER_DEBUG(...) ((void)0)
#endif

static void power_debug_method(aml_handle_t device, const char name[4])
{
#ifdef ACPI_POWER_DEBUG
    char path[192];
    if (!aml_handle_path(device, path, sizeof(path)))
        path[0] = '\0';
    POWER_DEBUG("[ACPI] power %s %c%c%c%c failed\n",
                path[0] ? path : "\\",
                name[0], name[1], name[2], name[3]);
    aml_debug_last_failure(name);
#else
    (void)device;
    (void)name;
#endif
}

static bool power_evaluate(aml_handle_t device, const char name[4],
                           aml_value_t *value)
{
    if (aml_evaluate_child(device, name, value))
        return true;
    power_debug_method(device, name);
    return false;
}

static void power_set_battery_state(mg_power_status_t *status, u64 state)
{
    if (state & 0x04U)
        status->battery_state = MG_BATTERY_STATE_CRITICAL;
    else if (state & 0x02U)
        status->battery_state = MG_BATTERY_STATE_CHARGING;
    else if (state & 0x01U)
        status->battery_state = MG_BATTERY_STATE_DISCHARGING;
    else
        status->battery_state = MG_BATTERY_STATE_IDLE;
}

static bool power_query_battery(aml_handle_t battery,
                                mg_power_status_t *status)
{
    aml_value_t sta = {0};
    aml_value_t bst = {0};
    aml_value_t info = {0};
    u64 sta_value = 0x1fU;
    u64 state = 0;
    u64 remaining = ACPI_BATTERY_UNKNOWN;
    u64 full;
    bool has_sta;
    bool got_info;
    u32 full_index;

    status->available = true;
    status->battery_state = MG_BATTERY_STATE_UNKNOWN;
    has_sta = power_evaluate(battery, "_STA", &sta) &&
              aml_value_integer(&sta, &sta_value);
    aml_value_release(&sta);
    if (has_sta && !(sta_value & 0x10U)) {
        status->battery_present = false;
        return true;
    }
    status->battery_present = true;
    if (power_evaluate(battery, "_BST", &bst) &&
        aml_value_package_integer(&bst, 0U, &state) &&
        aml_value_package_integer(&bst, 2U, &remaining)) {
        power_set_battery_state(status, state);
        if (remaining != ACPI_BATTERY_UNKNOWN)
            status->remaining_capacity = (u32)remaining;
    }
    aml_value_release(&bst);

    got_info = power_evaluate(battery, "_BIX", &info);
    full_index = 3U;
    if (!got_info) {
        aml_value_release(&info);
        got_info = power_evaluate(battery, "_BIF", &info);
        full_index = 2U;
    }
    if (got_info && aml_value_package_integer(&info, full_index, &full) &&
        full && full != ACPI_BATTERY_UNKNOWN && full <= 0xffffffffU) {
        status->full_charge_capacity = (u32)full;
        if (remaining != ACPI_BATTERY_UNKNOWN && remaining <= 0xffffffffU) {
            u64 percent = remaining * 100U / full;
            status->battery_percent = (u8)(percent > 100U ? 100U : percent);
            status->battery_percent_valid = true;
        }
    }
    aml_value_release(&info);
    return true;
}

static bool power_query_adapter(aml_handle_t adapter,
                                mg_power_status_t *status)
{
    aml_value_t psr = {0};
    u64 connected;

    status->available = true;
    if (power_evaluate(adapter, "_PSR", &psr) &&
        aml_value_integer(&psr, &connected)) {
        status->ac_available = true;
        status->ac_connected = connected != 0;
    }
    aml_value_release(&psr);
    return true;
}

static aml_handle_t lid_device = AML_HANDLE_INVALID;
static aml_handle_t internal_display = AML_HANDLE_INVALID;
static platform_lid_state_t current_lid_state = PLATFORM_LID_UNKNOWN;
static volatile bool lid_notification_pending;
static bool lid_initialized;

#ifdef ACPI_POWER_DEBUG
static const char *lid_state_name(platform_lid_state_t state)
{
    switch (state) {
    case PLATFORM_LID_OPEN: return "open";
    case PLATFORM_LID_CLOSED: return "closed";
    default: return "unknown";
    }
}
#endif

static bool power_lid_read(platform_lid_state_t *state)
{
    aml_value_t value = {0};
    u64 integer;

    if (!state || lid_device == AML_HANDLE_INVALID ||
        !aml_evaluate_child(lid_device, "_LID", &value) ||
        !aml_value_integer(&value, &integer)) {
        aml_value_release(&value);
        return false;
    }
    *state = integer ? PLATFORM_LID_OPEN : PLATFORM_LID_CLOSED;
    aml_value_release(&value);
    return true;
}

static bool power_display_set(bool active)
{
    aml_value_t argument = {
        .kind = AML_PUBLIC_VALUE_INTEGER,
        .integer = active ? 1U : 0U,
    };
    aml_value_t result = {0};
    bool success;

    if (internal_display == AML_HANDLE_INVALID)
        return false;
    success = aml_evaluate_child_args(internal_display, "_DSS",
                                      &argument, 1U, &result);
#ifdef ACPI_POWER_DEBUG
    if (!success) {
        char path[192] = "\\";
        (void)aml_handle_path(internal_display, path, sizeof(path));
        POWER_DEBUG("[ACPI] display %s _DSS(%u) failed\n",
                    path, active ? 1U : 0U);
        aml_debug_last_failure("display _DSS");
    }
#endif
    aml_value_release(&result);
    return success;
}

static void power_lid_apply_policy(platform_lid_state_t state)
{
    if (state == PLATFORM_LID_UNKNOWN || internal_display == AML_HANDLE_INVALID)
        return;
    if (!power_display_set(state == PLATFORM_LID_OPEN)) {
#ifdef ACPI_POWER_DEBUG
        POWER_DEBUG("[ACPI] lid policy could not change internal display\n");
#endif
    }
}

bool platform_lid_initialize(void)
{
    aml_handle_t cursor = AML_HANDLE_INVALID;
    aml_value_t status = {0};
    u64 status_value;
    platform_lid_state_t state;

    if (lid_initialized)
        return lid_device != AML_HANDLE_INVALID;
    lid_initialized = true;
    if (!aml_namespace_ready() && !aml_namespace_init())
        return false;

    while (aml_find_device_by_hid("PNP0C0D", &cursor)) {
        if (aml_child_exists(cursor, "_STA")) {
            if (!aml_evaluate_child(cursor, "_STA", &status) ||
                !aml_value_integer(&status, &status_value)) {
                aml_value_release(&status);
                continue;
            }
            aml_value_release(&status);
            if (!(status_value & 1U))
                continue;
        }
        if (!aml_child_exists(cursor, "_LID"))
            continue;
        lid_device = cursor;
        break;
    }
    if (lid_device == AML_HANDLE_INVALID)
        return false;
    if (!power_lid_read(&state)) {
        lid_device = AML_HANDLE_INVALID;
        return false;
    }
    current_lid_state = state;

    /* _BCL is not valid for external outputs.  Requiring it alongside the
     * standard output _DSS method selects the firmware-described built-in
     * panel without naming a GPU or LCD path. */
    cursor = AML_HANDLE_INVALID;
    while (aml_find_device_with_method("_DSS", &cursor)) {
        if (aml_child_exists(cursor, "_ADR") &&
            aml_child_exists(cursor, "_BCL")) {
            internal_display = cursor;
            break;
        }
    }

#ifdef ACPI_POWER_DEBUG
    {
        char lid_path[192] = "none";
        char display_path[192] = "none";
        (void)aml_handle_path(lid_device, lid_path, sizeof(lid_path));
        if (internal_display != AML_HANDLE_INVALID)
            (void)aml_handle_path(internal_display, display_path,
                                  sizeof(display_path));
        POWER_DEBUG("[ACPI] lid path=%s state=%s display=%s mechanism=%s\n",
                    lid_path, lid_state_name(current_lid_state), display_path,
                    internal_display != AML_HANDLE_INVALID ? "_DSS" :
                                                            "unavailable");
    }
#endif
    power_lid_apply_policy(current_lid_state);
    return true;
}

bool platform_lid_available(void)
{
    return lid_initialized && lid_device != AML_HANDLE_INVALID;
}

platform_lid_state_t platform_lid_state(void)
{
    return current_lid_state;
}

void platform_power_acpi_notify(aml_handle_t target, u64 value)
{
    if (target == lid_device && value == 0x80U) {
        __atomic_store_n(&lid_notification_pending, true, __ATOMIC_RELEASE);
#ifdef ACPI_POWER_DEBUG
        POWER_DEBUG("[ACPI] lid Notify value=0x%llx\n", value);
#endif
    }
}

void platform_power_process_lid_events(void)
{
    platform_lid_state_t state;

    if (!__atomic_exchange_n(&lid_notification_pending, false,
                             __ATOMIC_ACQ_REL) || !platform_lid_available())
        return;
    if (!power_lid_read(&state)) {
#ifdef ACPI_POWER_DEBUG
        POWER_DEBUG("[ACPI] lid _LID refresh failed\n");
        aml_debug_last_failure("lid _LID");
#endif
        return;
    }
    current_lid_state = state;
#ifdef ACPI_POWER_DEBUG
    POWER_DEBUG("[ACPI] lid notification state=%s\n", lid_state_name(state));
#endif
    power_lid_apply_policy(state);
}

static bool acpi_power_status_query(mg_power_status_t *status)
{
    aml_handle_t battery = AML_HANDLE_INVALID;
    aml_handle_t adapter = AML_HANDLE_INVALID;

    if (!status)
        return false;
    *status = (mg_power_status_t){0};
    if (!acpi_present())
        return true;
    if (!aml_namespace_ready() && !aml_namespace_init())
        return true;
    if (!aml_find_device_by_hid("PNP0C0A", &battery))
        battery = AML_HANDLE_INVALID;
    if (!aml_find_device_by_hid("ACPI0003", &adapter))
        adapter = AML_HANDLE_INVALID;
#ifdef ACPI_POWER_DEBUG
    {
        char battery_path[192] = "none";
        char adapter_path[192] = "none";
        if (battery != AML_HANDLE_INVALID)
            (void)aml_handle_path(battery, battery_path,
                                  sizeof(battery_path));
        if (adapter != AML_HANDLE_INVALID)
            (void)aml_handle_path(adapter, adapter_path,
                                  sizeof(adapter_path));
        POWER_DEBUG("[ACPI] power selected battery=%s adapter=%s\n",
                    battery_path, adapter_path);
    }
#endif
    if (battery != AML_HANDLE_INVALID)
        (void)power_query_battery(battery, status);
    if (adapter != AML_HANDLE_INVALID)
        (void)power_query_adapter(adapter, status);
    return true;
}

i64 platform_power_status(mg_power_status_t *status)
{
    platform_temperature_t temperature;

    if (!status)
        return MG_ERR_BAD_ARGUMENT;
    if (!acpi_power_status_query(status))
        return MG_ERR_UNSUPPORTED;
    if (platform_thermal_read(&temperature)) {
        status->temperature.available = true;
        status->temperature.sensor = (u8)temperature.sensor;
        status->temperature.millidegrees_celsius =
            temperature.millidegrees_celsius;
        status->available = true;
    }
    return MG_OK;
}
