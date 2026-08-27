#pragma once

#include <acpi.h>
#include <types.h>

/* AML is intentionally a kernel-private platform service.  Handles identify
 * immutable namespace nodes; callers never retain pointers into firmware
 * tables or interpreter-owned values. */
typedef u16 aml_handle_t;

#define AML_HANDLE_INVALID ((aml_handle_t)0xffffU)

typedef enum {
    AML_PUBLIC_VALUE_INVALID = 0,
    AML_PUBLIC_VALUE_INTEGER,
    AML_PUBLIC_VALUE_STRING,
    AML_PUBLIC_VALUE_BUFFER,
    AML_PUBLIC_VALUE_PACKAGE,
    AML_PUBLIC_VALUE_REFERENCE,
} aml_public_value_kind_t;

typedef struct {
    aml_public_value_kind_t kind;
    u64 integer;
    void *object;
} aml_value_t;

typedef void (*aml_notify_handler_t)(aml_handle_t target, u64 value);

/* Builds the DSDT/SSDT namespace once, after the permanent mapping and heap
 * are available.  Every definition block is loaded into one hierarchy. */
bool aml_namespace_init(void);
bool aml_namespace_ready(void);

/* Device enumeration and child-object evaluation used by the ACPI platform
 * layers.  `cursor` is AML_HANDLE_INVALID for the first match and is updated
 * to the returned device. */
bool aml_find_device_by_hid(const char *hid, aml_handle_t *cursor);
bool aml_find_thermal_zone(aml_handle_t *cursor);
bool aml_find_device_with_method(const char name[4], aml_handle_t *cursor);
bool aml_handle_path(aml_handle_t handle, char *path, usize capacity);
bool aml_child_exists(aml_handle_t parent, const char name[4]);
bool aml_evaluate_child(aml_handle_t parent, const char name[4],
                        aml_value_t *result);
bool aml_evaluate_child_args(aml_handle_t parent, const char name[4],
                             const aml_value_t *arguments,
                             u8 argument_count, aml_value_t *result);

/* Notify callbacks only record/signal platform work.  The callback executes
 * while the AML evaluator owns its serialization lock and must not perform
 * another AML evaluation or block. */
void aml_set_notify_handler(aml_notify_handler_t handler);
bool aml_dispatch_gpe(u8 gpe);

bool aml_value_integer(const aml_value_t *value, u64 *integer);
bool aml_value_package_integer(const aml_value_t *value, u32 index,
                               u64 *integer);
void aml_value_release(aml_value_t *value);

/* Firmware-driven EC discovery and region-handler connection.  Connection
 * executes the relevant ancestor _INI methods followed by the EC's _REG(3,
 * 1); EmbeddedControl fields remain inaccessible unless that sequence
 * completes successfully. */
bool aml_discover_ec(acpi_ec_info_t *info, aml_handle_t *device);
bool aml_connect_ec_regions(aml_handle_t ec_device);

/* Compact diagnostics for a failed public evaluation. */
void aml_debug_last_failure(const char *operation);
