#include <aml.h>

#include <address_space.h>
#include <ec.h>
#include <heap.h>
#include <io.h>
#include <kprint.h>
#include <stddef.h>
#include <string.h>
#include <timer.h>
#include <vmm.h>

#define AML_MAX_NODES             6144U
#define AML_MAX_NAME_SEGMENTS       32U
#define AML_MAX_PATH               192U
#define AML_MAX_DYNAMIC_OBJECTS      48U
#define AML_MAX_METHOD_DEPTH         16U
#define AML_MAX_METHOD_OPS      1000000U
#define AML_MAX_LOOP_ITERATIONS  250000U
#define AML_METHOD_TIMEOUT_US   2000000ULL
#define AML_MUTEX_TIMEOUT_CAP_MS  2000U

#define AML_ROOT_NODE ((aml_handle_t)0U)

#define AML_ZERO_OP              0x00U
#define AML_ONE_OP               0x01U
#define AML_ALIAS_OP             0x06U
#define AML_NAME_OP              0x08U
#define AML_BYTE_PREFIX          0x0aU
#define AML_WORD_PREFIX          0x0bU
#define AML_DWORD_PREFIX         0x0cU
#define AML_STRING_PREFIX        0x0dU
#define AML_QWORD_PREFIX         0x0eU
#define AML_SCOPE_OP             0x10U
#define AML_BUFFER_OP            0x11U
#define AML_PACKAGE_OP           0x12U
#define AML_VAR_PACKAGE_OP       0x13U
#define AML_METHOD_OP            0x14U
#define AML_EXTERNAL_OP          0x15U

#define AML_LOCAL0_OP            0x60U
#define AML_LOCAL7_OP            0x67U
#define AML_ARG0_OP              0x68U
#define AML_ARG6_OP              0x6eU
#define AML_STORE_OP             0x70U
#define AML_REF_OF_OP            0x71U
#define AML_ADD_OP               0x72U
#define AML_CONCAT_OP            0x73U
#define AML_SUBTRACT_OP          0x74U
#define AML_INCREMENT_OP         0x75U
#define AML_DECREMENT_OP         0x76U
#define AML_MULTIPLY_OP          0x77U
#define AML_DIVIDE_OP            0x78U
#define AML_SHIFT_LEFT_OP        0x79U
#define AML_SHIFT_RIGHT_OP       0x7aU
#define AML_AND_OP               0x7bU
#define AML_NAND_OP              0x7cU
#define AML_OR_OP                0x7dU
#define AML_NOR_OP               0x7eU
#define AML_XOR_OP               0x7fU
#define AML_NOT_OP               0x80U
#define AML_DEREF_OF_OP          0x83U
#define AML_MOD_OP               0x85U
#define AML_NOTIFY_OP            0x86U
#define AML_SIZE_OF_OP           0x87U
#define AML_INDEX_OP             0x88U
#define AML_CREATE_DWORD_FIELD   0x8aU
#define AML_CREATE_WORD_FIELD    0x8bU
#define AML_CREATE_BYTE_FIELD    0x8cU
#define AML_CREATE_BIT_FIELD     0x8dU
#define AML_CREATE_QWORD_FIELD   0x8fU
#define AML_LAND_OP              0x90U
#define AML_LOR_OP               0x91U
#define AML_LNOT_OP              0x92U
#define AML_LEQUAL_OP            0x93U
#define AML_LGREATER_OP          0x94U
#define AML_LLESS_OP             0x95U
#define AML_TO_BUFFER_OP         0x96U
#define AML_TO_DECIMAL_STRING_OP 0x97U
#define AML_TO_HEX_STRING_OP     0x98U
#define AML_TO_INTEGER_OP        0x99U
#define AML_TO_STRING_OP         0x9cU
#define AML_COPY_OBJECT_OP       0x9dU
#define AML_MID_OP               0x9eU
#define AML_CONTINUE_OP          0x9fU
#define AML_IF_OP                0xa0U
#define AML_ELSE_OP              0xa1U
#define AML_WHILE_OP             0xa2U
#define AML_NOOP_OP              0xa3U
#define AML_RETURN_OP            0xa4U
#define AML_BREAK_OP             0xa5U
#define AML_ONES_OP              0xffU

#define AML_EXT_OP               0x5bU
#define AML_EXT_MUTEX_OP         0x01U
#define AML_EXT_EVENT_OP         0x02U
#define AML_EXT_COND_REF_OF_OP   0x12U
#define AML_EXT_CREATE_FIELD_OP  0x13U
#define AML_EXT_STALL_OP         0x21U
#define AML_EXT_SLEEP_OP         0x22U
#define AML_EXT_ACQUIRE_OP       0x23U
#define AML_EXT_RELEASE_OP       0x27U
#define AML_EXT_OP_REGION_OP     0x80U
#define AML_EXT_FIELD_OP         0x81U
#define AML_EXT_DEVICE_OP        0x82U
#define AML_EXT_PROCESSOR_OP     0x83U
#define AML_EXT_POWER_RES_OP     0x84U
#define AML_EXT_THERMAL_ZONE_OP  0x85U
#define AML_EXT_INDEX_FIELD_OP   0x86U
#define AML_EXT_BANK_FIELD_OP    0x87U

#define AML_REGION_SYSTEM_MEMORY  0x00U
#define AML_REGION_SYSTEM_IO      0x01U
#define AML_REGION_EMBEDDED_CTRL  0x03U

#define AML_FIELD_ACCESS_MASK     0x0fU
#define AML_FIELD_LOCK            0x10U
#define AML_FIELD_UPDATE_MASK     0x60U
#define AML_FIELD_WRITE_ONES      0x20U
#define AML_FIELD_WRITE_ZEROS     0x40U

#define AML_FACS_GLOBAL_LOCK_OFFSET 16U
#define AML_PM1_GLOBAL_LOCK_RELEASE (1U << 2)

typedef enum {
    AML_NODE_ROOT = 0,
    AML_NODE_SCOPE,
    AML_NODE_DEVICE,
    AML_NODE_THERMAL_ZONE,
    AML_NODE_NAME,
    AML_NODE_METHOD,
    AML_NODE_ALIAS,
    AML_NODE_REGION,
    AML_NODE_FIELD,
    AML_NODE_INDEX_FIELD,
    AML_NODE_BANK_FIELD,
    AML_NODE_MUTEX,
    AML_NODE_EVENT,
    AML_NODE_EXTERNAL,
    AML_NODE_NATIVE_METHOD,
} aml_node_kind_t;

typedef enum {
    AML_DATA_BUFFER = 1,
    AML_DATA_STRING,
    AML_DATA_PACKAGE,
    AML_DATA_REFERENCE,
} aml_data_kind_t;

typedef enum {
    AML_REF_NODE = 1,
    AML_REF_SLOT,
    AML_REF_PACKAGE_ELEMENT,
    AML_REF_BUFFER_FIELD,
    AML_REF_DYNAMIC,
} aml_reference_kind_t;

typedef enum {
    AML_FAILURE_NONE = 0,
    AML_FAILURE_NAMESPACE_FULL,
    AML_FAILURE_MALFORMED_AML,
    AML_FAILURE_UNRESOLVED_NAME,
    AML_FAILURE_UNSUPPORTED_OPCODE,
    AML_FAILURE_UNSUPPORTED_REGION,
    AML_FAILURE_OBJECT_TYPE,
    AML_FAILURE_HARDWARE_ACCESS,
    AML_FAILURE_TIMEOUT,
    AML_FAILURE_BUDGET,
    AML_FAILURE_RECURSION,
    AML_FAILURE_DIVIDE_BY_ZERO,
} aml_failure_kind_t;

typedef struct aml_data aml_data_t;
typedef struct aml_dynamic_object aml_dynamic_object_t;

typedef struct {
    aml_reference_kind_t kind;
    aml_handle_t node;
    aml_value_t *slot;
    aml_dynamic_object_t *dynamic;
    aml_data_t *owner;
    u32 index;
    u32 bit_offset;
    u32 bit_length;
} aml_reference_t;

struct aml_data {
    u32 references;
    aml_data_kind_t kind;
    u32 length;
    union {
        u8 *bytes;
        aml_value_t *elements;
        aml_reference_t reference;
    } value;
};

typedef struct {
    bool absolute;
    bool null_name;
    u8 parent_prefixes;
    u8 count;
    u32 segment[AML_MAX_NAME_SEGMENTS];
} aml_name_path_t;

typedef struct {
    const u8 *expression;
    u32 expression_length;
    aml_value_t value;
    bool initialized;
    bool initializing;
} aml_name_object_t;

typedef struct {
    const u8 *body;
    u32 body_length;
    u8 argument_count;
    u8 sync_level;
    bool serialized;
    volatile u32 owner;
    u16 recursion;
} aml_method_object_t;

typedef struct {
    u8 space;
    const u8 *offset_expression;
    u32 offset_length;
    const u8 *length_expression;
    u32 length_length;
    u64 evaluated_offset;
    u64 evaluated_length;
    volatile u8 *mapping;
    bool evaluated;
    bool connected;
} aml_region_object_t;

typedef struct {
    aml_handle_t region;
    aml_region_object_t *runtime_region;
    aml_handle_t index_register;
    aml_handle_t data_register;
    aml_handle_t bank_register;
    const u8 *bank_expression;
    u32 bank_expression_length;
    const u8 *source_name;
    u32 source_name_length;
    u32 bit_offset;
    u32 bit_length;
    u8 flags;
    u8 access_attribute;
    u8 access_length;
} aml_field_object_t;

typedef struct {
    const u8 *source_name;
    u32 source_name_length;
    aml_handle_t target;
} aml_alias_object_t;

typedef struct {
    u8 object_type;
    u8 argument_count;
} aml_external_object_t;

typedef struct {
    volatile u32 owner;
    u8 sync_level;
    u16 recursion;
} aml_mutex_object_t;

typedef struct {
    u32 name;
    aml_handle_t parent;
    aml_handle_t first_child;
    aml_handle_t next_sibling;
    aml_node_kind_t kind;
    u8 table_index;
    u32 table_offset;
    bool initialized;
    union {
        aml_name_object_t name;
        aml_method_object_t method;
        aml_region_object_t region;
        aml_field_object_t field;
        aml_alias_object_t alias;
        aml_external_object_t external;
        aml_mutex_object_t mutex;
    } object;
} aml_node_t;

struct aml_dynamic_object {
    aml_node_kind_t kind;
    u32 name;
    aml_value_t value;
    aml_region_object_t region;
    aml_field_object_t field;
};

typedef struct {
    u32 operations;
    u32 loop_iterations;
    u32 invocation_id;
    u8 depth;
    timer_monotonic_deadline_t deadline;
    bool deadline_valid;
} aml_budget_t;

typedef struct aml_execution {
    struct aml_execution *caller;
    aml_handle_t method;
    aml_handle_t scope;
    aml_value_t local[8];
    aml_value_t argument[7];
    aml_dynamic_object_t dynamic[AML_MAX_DYNAMIC_OBJECTS];
    u8 dynamic_count;
    aml_budget_t *budget;
    bool returned;
    bool break_requested;
    bool continue_requested;
    aml_value_t return_value;
} aml_execution_t;

typedef struct {
    aml_failure_kind_t kind;
    u8 opcode;
    aml_handle_t scope;
    u8 table_index;
    u32 table_offset;
    aml_name_path_t name;
    bool has_name;
} aml_failure_t;

static aml_node_t aml_nodes[AML_MAX_NODES];
static u32 aml_node_count;
static bool aml_ready;
static bool aml_building;
static bool aml_ec_connected;
static u64 aml_integer_mask;
static u32 aml_next_invocation_id = 1U;
static aml_failure_t aml_failure;
static aml_notify_handler_t aml_notify_handler;
static volatile u32 *aml_global_lock;
static volatile u32 aml_evaluator_owner;
static const acpi_sdt_header_t *aml_tables[65];
static u8 aml_table_count;

static bool aml_eval_term(aml_execution_t *execution, const u8 *aml,
                          u32 available, aml_value_t *value, u32 *used);
static bool aml_execute_block(aml_execution_t *execution, const u8 *aml,
                              u32 available);
static bool aml_read_node(aml_execution_t *execution, aml_handle_t handle,
                          aml_value_t *value);
static bool aml_write_reference(aml_execution_t *execution,
                                const aml_reference_t *reference,
                                const aml_value_t *value);
static aml_handle_t aml_alias_target(aml_handle_t handle);
static bool aml_invoke_method(aml_execution_t *caller, aml_handle_t method,
                              const aml_value_t *arguments,
                              u8 argument_count, aml_value_t *result);
static bool aml_evaluate_handle(aml_handle_t handle,
                                const aml_value_t *arguments,
                                u8 argument_count, aml_value_t *result);
static void aml_execution_release(aml_execution_t *execution);
static bool aml_execution_integer(aml_execution_t *execution,
                                  const aml_value_t *value, u64 *integer);
static bool aml_read_dynamic(aml_execution_t *execution,
                             aml_dynamic_object_t *object,
                             aml_value_t *value);
static bool aml_write_dynamic(aml_execution_t *execution,
                              aml_dynamic_object_t *object,
                              const aml_value_t *value);
static aml_handle_t aml_ensure_scope_path(aml_handle_t scope,
                                          const aml_name_path_t *path,
                                          u8 table_index,
                                          u32 table_offset);

static u32 aml_name_pack(const u8 *name)
{
    return (u32)name[0] | ((u32)name[1] << 8) |
           ((u32)name[2] << 16) | ((u32)name[3] << 24);
}

static void aml_name_unpack(u32 name, char text[5])
{
    text[0] = (char)(name & 0xffU);
    text[1] = (char)((name >> 8) & 0xffU);
    text[2] = (char)((name >> 16) & 0xffU);
    text[3] = (char)((name >> 24) & 0xffU);
    text[4] = '\0';
}

static bool aml_name_lead(u8 value)
{
    return (value >= 'A' && value <= 'Z') || value == '_';
}

static bool aml_name_tail(u8 value)
{
    return aml_name_lead(value) || (value >= '0' && value <= '9');
}

static bool aml_name_segment_valid(const u8 *name)
{
    return name && aml_name_lead(name[0]) && aml_name_tail(name[1]) &&
           aml_name_tail(name[2]) && aml_name_tail(name[3]);
}

static void aml_note_failure(aml_failure_kind_t kind, u8 opcode,
                             aml_handle_t scope, const aml_name_path_t *name,
                             u8 table_index, u32 table_offset)
{
    if (aml_failure.kind != AML_FAILURE_NONE)
        return;
    aml_failure.kind = kind;
    aml_failure.opcode = opcode;
    aml_failure.scope = scope;
    aml_failure.table_index = table_index;
    aml_failure.table_offset = table_offset;
    if (name) {
        aml_failure.name = *name;
        aml_failure.has_name = true;
    }
}

static void aml_failure_clear(void)
{
    aml_failure = (aml_failure_t){0};
    aml_failure.scope = AML_HANDLE_INVALID;
}

static aml_data_t *aml_data_allocate(aml_data_kind_t kind, u32 length)
{
    aml_data_t *data = (aml_data_t *)kmalloc(sizeof(*data));

    if (!data)
        return NULL;
    *data = (aml_data_t){0};
    data->references = 1U;
    data->kind = kind;
    data->length = length;
    if (kind == AML_DATA_BUFFER || kind == AML_DATA_STRING) {
        u32 bytes = length + (kind == AML_DATA_STRING ? 1U : 0U);
        data->value.bytes = bytes ? (u8 *)kmalloc(bytes) : NULL;
        if (bytes && !data->value.bytes) {
            kfree(data);
            return NULL;
        }
        if (bytes)
            memset(data->value.bytes, 0, bytes);
    } else if (kind == AML_DATA_PACKAGE) {
        data->value.elements = length ?
            (aml_value_t *)kmalloc((usize)length * sizeof(aml_value_t)) : NULL;
        if (length && !data->value.elements) {
            kfree(data);
            return NULL;
        }
        if (length)
            memset(data->value.elements, 0,
                   (usize)length * sizeof(aml_value_t));
    }
    return data;
}

static void aml_data_retain(aml_data_t *data)
{
    if (data)
        data->references++;
}

void aml_value_release(aml_value_t *value)
{
    aml_data_t *data;

    if (!value || !value->object) {
        if (value)
            *value = (aml_value_t){0};
        return;
    }
    data = (aml_data_t *)value->object;
    if (data->references)
        data->references--;
    if (!data->references) {
        if (data->kind == AML_DATA_PACKAGE && data->value.elements) {
            for (u32 i = 0; i < data->length; i++)
                aml_value_release(&data->value.elements[i]);
            kfree(data->value.elements);
        } else if ((data->kind == AML_DATA_BUFFER ||
                    data->kind == AML_DATA_STRING) && data->value.bytes) {
            kfree(data->value.bytes);
        } else if (data->kind == AML_DATA_REFERENCE) {
            aml_data_t *owner = data->value.reference.owner;
            if (owner) {
                aml_value_t owner_value = {
                    .kind = owner->kind == AML_DATA_PACKAGE ?
                        AML_PUBLIC_VALUE_PACKAGE :
                        owner->kind == AML_DATA_STRING ?
                            AML_PUBLIC_VALUE_STRING :
                            AML_PUBLIC_VALUE_BUFFER,
                    .object = owner,
                };
                aml_value_release(&owner_value);
            }
        }
        kfree(data);
    }
    *value = (aml_value_t){0};
}

static bool aml_value_copy(aml_value_t *destination,
                           const aml_value_t *source)
{
    if (!destination || !source)
        return false;
    aml_value_release(destination);
    *destination = *source;
    if (destination->object)
        aml_data_retain((aml_data_t *)destination->object);
    return true;
}

static aml_value_t aml_integer_value(u64 integer)
{
    aml_value_t value = {
        .kind = AML_PUBLIC_VALUE_INTEGER,
        .integer = integer & aml_integer_mask,
    };
    return value;
}

static bool aml_data_value(aml_value_t *value, aml_data_kind_t data_kind,
                           aml_public_value_kind_t public_kind, u32 length)
{
    aml_data_t *data;

    if (!value)
        return false;
    data = aml_data_allocate(data_kind, length);
    if (!data)
        return false;
    aml_value_release(value);
    value->kind = public_kind;
    value->object = data;
    return true;
}

static bool aml_string_value(aml_value_t *value, const u8 *bytes, u32 length)
{
    aml_data_t *data;

    if (!aml_data_value(value, AML_DATA_STRING, AML_PUBLIC_VALUE_STRING,
                        length))
        return false;
    data = (aml_data_t *)value->object;
    if (length && bytes)
        memcpy(data->value.bytes, bytes, length);
    data->value.bytes[length] = 0;
    return true;
}

static bool aml_buffer_value(aml_value_t *value, u32 length)
{
    return aml_data_value(value, AML_DATA_BUFFER, AML_PUBLIC_VALUE_BUFFER,
                          length);
}

static bool aml_package_value(aml_value_t *value, u32 length)
{
    return aml_data_value(value, AML_DATA_PACKAGE, AML_PUBLIC_VALUE_PACKAGE,
                          length);
}

static bool aml_reference_value(aml_value_t *value,
                                const aml_reference_t *reference)
{
    aml_data_t *data;

    if (!value || !reference ||
        !aml_data_value(value, AML_DATA_REFERENCE,
                        AML_PUBLIC_VALUE_REFERENCE, 0))
        return false;
    data = (aml_data_t *)value->object;
    data->value.reference = *reference;
    if (reference->owner)
        aml_data_retain(reference->owner);
    return true;
}

static const aml_reference_t *aml_value_reference(const aml_value_t *value)
{
    aml_data_t *data;

    if (!value || value->kind != AML_PUBLIC_VALUE_REFERENCE ||
        !(data = (aml_data_t *)value->object) ||
        data->kind != AML_DATA_REFERENCE)
        return NULL;
    return &data->value.reference;
}

static bool aml_budget_step(aml_execution_t *execution, u8 opcode)
{
    aml_budget_t *budget;

    if (!execution || !(budget = execution->budget))
        return false;
    if (++budget->operations > AML_MAX_METHOD_OPS) {
        aml_note_failure(AML_FAILURE_BUDGET, opcode, execution->scope,
                         NULL, 0, 0);
        return false;
    }
    if (budget->deadline_valid && (budget->operations & 0x3fU) == 0U &&
        timer_monotonic_deadline_expired(&budget->deadline)) {
        aml_note_failure(AML_FAILURE_TIMEOUT, opcode, execution->scope,
                         NULL, 0, 0);
        return false;
    }
    return true;
}

bool aml_value_integer(const aml_value_t *value, u64 *integer)
{
    if (!value || !integer || value->kind != AML_PUBLIC_VALUE_INTEGER)
        return false;
    *integer = value->integer & aml_integer_mask;
    return true;
}

bool aml_value_package_integer(const aml_value_t *value, u32 index,
                               u64 *integer)
{
    aml_data_t *package;

    if (!value || value->kind != AML_PUBLIC_VALUE_PACKAGE ||
        !(package = (aml_data_t *)value->object) ||
        package->kind != AML_DATA_PACKAGE || index >= package->length)
        return false;
    return aml_value_integer(&package->value.elements[index], integer);
}

static bool aml_value_to_integer(const aml_value_t *value, u64 *integer)
{
    aml_data_t *data;
    u64 result = 0;

    if (aml_value_integer(value, integer))
        return true;
    if (!value || !(data = (aml_data_t *)value->object))
        return false;
    if (value->kind == AML_PUBLIC_VALUE_BUFFER &&
        data->kind == AML_DATA_BUFFER) {
        u32 count = data->length < (aml_integer_mask == 0xffffffffULL ? 4U : 8U) ?
                    data->length : (aml_integer_mask == 0xffffffffULL ? 4U : 8U);
        for (u32 i = 0; i < count; i++)
            result |= (u64)data->value.bytes[i] << (8U * i);
        *integer = result & aml_integer_mask;
        return true;
    }
    if (value->kind == AML_PUBLIC_VALUE_STRING &&
        data->kind == AML_DATA_STRING) {
        u32 base = 10U;
        u32 index = 0;
        if (data->length >= 2U && data->value.bytes[0] == '0' &&
            (data->value.bytes[1] == 'x' || data->value.bytes[1] == 'X')) {
            base = 16U;
            index = 2U;
        }
        for (; index < data->length; index++) {
            u8 c = data->value.bytes[index];
            u32 digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (base == 16U && c >= 'A' && c <= 'F') digit = c - 'A' + 10U;
            else if (base == 16U && c >= 'a' && c <= 'f') digit = c - 'a' + 10U;
            else break;
            result = (result * base + digit) & aml_integer_mask;
        }
        *integer = result;
        return true;
    }
    return false;
}

static bool aml_package_length_raw(const u8 *aml, u32 available,
                                   u32 *encoded, u32 *length)
{
    u8 follow;
    u32 value;

    if (!aml || !available || !encoded || !length)
        return false;
    follow = aml[0] >> 6;
    if (follow > 3U || available < (u32)follow + 1U)
        return false;
    if (!follow) {
        value = aml[0] & 0x3fU;
    } else {
        value = aml[0] & 0x0fU;
        for (u8 i = 0; i < follow; i++)
            value |= (u32)aml[1U + i] << (4U + 8U * i);
    }
    *encoded = (u32)follow + 1U;
    *length = value;
    return true;
}

static bool aml_package_span(const u8 *aml, u32 available,
                             u32 *encoded, u32 *length)
{
    return aml_package_length_raw(aml, available, encoded, length) &&
           *length >= *encoded && *length <= available;
}

static bool aml_parse_name(const u8 *aml, u32 available,
                           aml_name_path_t *path, u32 *used)
{
    u32 offset = 0;
    u32 count;

    if (!aml || !available || !path || !used)
        return false;
    *path = (aml_name_path_t){0};
    if (aml[offset] == '\\') {
        path->absolute = true;
        offset++;
        if (offset == available) {
            *used = offset;
            return true;
        }
    }
    while (offset < available && aml[offset] == '^') {
        if (path->parent_prefixes == 0xffU)
            return false;
        path->parent_prefixes++;
        offset++;
    }
    if (offset >= available)
        return false;
    if (aml[offset] == 0x00U) {
        path->null_name = true;
        *used = offset + 1U;
        return true;
    }
    if (aml[offset] == 0x2eU) {
        count = 2U;
        offset++;
    } else if (aml[offset] == 0x2fU) {
        if (available - offset < 2U)
            return false;
        count = aml[offset + 1U];
        offset += 2U;
    } else {
        count = 1U;
    }
    if (!count || count > AML_MAX_NAME_SEGMENTS ||
        available - offset < count * 4U)
        return false;
    for (u32 i = 0; i < count; i++) {
        if (!aml_name_segment_valid(aml + offset + i * 4U))
            return false;
        path->segment[i] = aml_name_pack(aml + offset + i * 4U);
    }
    path->count = (u8)count;
    *used = offset + count * 4U;
    return true;
}

static bool aml_name_starts(const u8 *aml, u32 available)
{
    return aml && available &&
           (aml[0] == '\\' || aml[0] == '^' || aml[0] == 0x2eU ||
            aml[0] == 0x2fU || aml_name_lead(aml[0]));
}

static aml_handle_t aml_find_child(aml_handle_t parent, u32 name)
{
    aml_handle_t child;

    if (parent >= aml_node_count)
        return AML_HANDLE_INVALID;
    child = aml_nodes[parent].first_child;
    while (child != AML_HANDLE_INVALID) {
        if (aml_nodes[child].name == name)
            return child;
        child = aml_nodes[child].next_sibling;
    }
    return AML_HANDLE_INVALID;
}

static aml_handle_t aml_ascend(aml_handle_t scope, u8 count)
{
    while (count--) {
        if (scope == AML_ROOT_NODE || scope >= aml_node_count)
            return AML_HANDLE_INVALID;
        scope = aml_nodes[scope].parent;
    }
    return scope;
}

static aml_handle_t aml_follow(aml_handle_t scope,
                               const aml_name_path_t *path)
{
    aml_handle_t current = scope;

    if (!path || path->null_name)
        return AML_HANDLE_INVALID;
    if (path->absolute)
        current = AML_ROOT_NODE;
    else if (path->parent_prefixes) {
        current = aml_ascend(current, path->parent_prefixes);
        if (current == AML_HANDLE_INVALID)
            return current;
    }
    for (u8 i = 0; i < path->count; i++) {
        current = aml_find_child(current, path->segment[i]);
        if (current == AML_HANDLE_INVALID)
            return current;
    }
    return current;
}

static aml_handle_t aml_resolve(aml_handle_t scope,
                                const aml_name_path_t *path)
{
    aml_handle_t found;

    if (!path || path->null_name)
        return AML_HANDLE_INVALID;
    if (path->absolute || path->parent_prefixes || path->count != 1U)
        return aml_follow(scope, path);

    /* ACPI parent-scope search applies only to an unprefixed single NameSeg. */
    while (scope != AML_HANDLE_INVALID) {
        found = aml_find_child(scope, path->segment[0]);
        if (found != AML_HANDLE_INVALID)
            return found;
        if (scope == AML_ROOT_NODE)
            break;
        scope = aml_nodes[scope].parent;
    }
    return AML_HANDLE_INVALID;
}

static aml_handle_t aml_create_node(aml_handle_t parent, u32 name,
                                    aml_node_kind_t kind, u8 table_index,
                                    u32 table_offset)
{
    aml_handle_t existing;
    aml_handle_t handle;

    if (parent >= aml_node_count || !name)
        return AML_HANDLE_INVALID;
    existing = aml_find_child(parent, name);
    if (existing != AML_HANDLE_INVALID) {
        if (aml_nodes[existing].kind == AML_NODE_EXTERNAL ||
            aml_nodes[existing].kind == AML_NODE_SCOPE) {
            if (kind != AML_NODE_SCOPE) {
                aml_nodes[existing].kind = kind;
                aml_nodes[existing].table_index = table_index;
                aml_nodes[existing].table_offset = table_offset;
            }
            return existing;
        }
        return existing;
    }
    if (aml_node_count >= AML_MAX_NODES) {
        aml_note_failure(AML_FAILURE_NAMESPACE_FULL, 0, parent, NULL,
                         table_index, table_offset);
        return AML_HANDLE_INVALID;
    }
    handle = (aml_handle_t)aml_node_count++;
    aml_nodes[handle] = (aml_node_t){
        .name = name,
        .parent = parent,
        .first_child = AML_HANDLE_INVALID,
        .next_sibling = aml_nodes[parent].first_child,
        .kind = kind,
        .table_index = table_index,
        .table_offset = table_offset,
    };
    aml_nodes[parent].first_child = handle;
    return handle;
}

static aml_handle_t aml_create_path(aml_handle_t scope,
                                    const aml_name_path_t *path,
                                    aml_node_kind_t kind, u8 table_index,
                                    u32 table_offset)
{
    aml_handle_t parent;

    if (!path || path->null_name || !path->count)
        return AML_HANDLE_INVALID;
    parent = path->absolute ? AML_ROOT_NODE :
             aml_ascend(scope, path->parent_prefixes);
    if (parent == AML_HANDLE_INVALID)
        return parent;
    for (u8 i = 0; i + 1U < path->count; i++) {
        parent = aml_find_child(parent, path->segment[i]);
        if (parent == AML_HANDLE_INVALID)
            return parent;
    }
    return aml_create_node(parent, path->segment[path->count - 1U], kind,
                           table_index, table_offset);
}

bool aml_handle_path(aml_handle_t handle, char *path, usize capacity)
{
    u32 segments[AML_MAX_NAME_SEGMENTS];
    u32 count = 0;
    usize offset = 0;

    if (!path || capacity < 2U || handle >= aml_node_count)
        return false;
    while (handle != AML_ROOT_NODE) {
        if (count >= AML_MAX_NAME_SEGMENTS)
            return false;
        segments[count++] = aml_nodes[handle].name;
        handle = aml_nodes[handle].parent;
    }
    path[offset++] = '\\';
    for (u32 i = count; i > 0; i--) {
        char name[5];
        if (offset > 1U) {
            if (offset + 1U >= capacity)
                return false;
            path[offset++] = '.';
        }
        if (offset + 4U >= capacity)
            return false;
        aml_name_unpack(segments[i - 1U], name);
        memcpy(path + offset, name, 4U);
        offset += 4U;
    }
    path[offset] = '\0';
    return true;
}

static bool aml_integer_literal(const u8 *aml, u32 available,
                                u64 *integer, u32 *used)
{
    u64 value = 0;

    if (!aml || !available || !integer || !used)
        return false;
    switch (aml[0]) {
        case AML_ZERO_OP:
            *integer = 0;
            *used = 1U;
            return true;
        case AML_ONE_OP:
            *integer = 1U;
            *used = 1U;
            return true;
        case AML_BYTE_PREFIX:
            if (available < 2U) return false;
            *integer = aml[1];
            *used = 2U;
            return true;
        case AML_WORD_PREFIX:
            if (available < 3U) return false;
            *integer = (u64)aml[1] | ((u64)aml[2] << 8);
            *used = 3U;
            return true;
        case AML_DWORD_PREFIX:
            if (available < 5U) return false;
            for (u8 i = 0; i < 4U; i++)
                value |= (u64)aml[1U + i] << (8U * i);
            *integer = value;
            *used = 5U;
            return true;
        case AML_QWORD_PREFIX:
            if (available < 9U) return false;
            for (u8 i = 0; i < 8U; i++)
                value |= (u64)aml[1U + i] << (8U * i);
            *integer = value;
            *used = 9U;
            return true;
        case AML_ONES_OP:
            *integer = aml_integer_mask;
            *used = 1U;
            return true;
        default:
            return false;
    }
}

static bool aml_term_span_depth(aml_handle_t scope, const u8 *aml,
                                u32 available, u32 *span, u8 depth);

static bool aml_target_span(aml_handle_t scope, const u8 *aml,
                            u32 available, u32 *span, u8 depth)
{
    aml_name_path_t path;
    u32 used;

    if (!aml || !available || !span || depth > AML_MAX_METHOD_DEPTH)
        return false;
    if (aml[0] == AML_ZERO_OP ||
        (aml[0] >= AML_LOCAL0_OP && aml[0] <= AML_ARG6_OP)) {
        *span = 1U;
        return true;
    }
    if (aml_name_starts(aml, available) &&
        aml_parse_name(aml, available, &path, &used)) {
        *span = used;
        return true;
    }
    if (aml[0] == AML_INDEX_OP)
        return aml_term_span_depth(scope, aml, available, span, depth + 1U);
    return false;
}

static bool aml_terms_span(aml_handle_t scope, const u8 *aml, u32 available,
                           u8 count, u32 *span, u8 depth)
{
    u32 offset = 1U;

    for (u8 i = 0; i < count; i++) {
        u32 term;
        if (offset > available ||
            !aml_term_span_depth(scope, aml + offset, available - offset,
                                 &term, depth + 1U))
            return false;
        offset += term;
    }
    *span = offset;
    return true;
}

static bool aml_term_span_depth(aml_handle_t scope, const u8 *aml,
                                u32 available, u32 *span, u8 depth)
{
    aml_name_path_t path;
    aml_handle_t handle;
    u64 integer;
    u32 used;
    u32 offset;
    u32 term;
    u32 encoded;
    u32 package;

    if (!aml || !available || !span || depth > AML_MAX_METHOD_DEPTH)
        return false;
    if (aml_integer_literal(aml, available, &integer, &used)) {
        *span = used;
        return true;
    }
    if (aml[0] == AML_STRING_PREFIX) {
        for (used = 1U; used < available; used++) {
            if (aml[used] == 0) {
                *span = used + 1U;
                return true;
            }
        }
        return false;
    }
    if (aml[0] == AML_BUFFER_OP || aml[0] == AML_PACKAGE_OP ||
        aml[0] == AML_VAR_PACKAGE_OP || aml[0] == AML_SCOPE_OP ||
        aml[0] == AML_METHOD_OP || aml[0] == AML_IF_OP ||
        aml[0] == AML_ELSE_OP || aml[0] == AML_WHILE_OP) {
        if (!aml_package_span(aml + 1U, available - 1U,
                              &encoded, &package))
            return false;
        *span = 1U + package;
        return true;
    }
    if (aml[0] >= AML_LOCAL0_OP && aml[0] <= AML_ARG6_OP) {
        *span = 1U;
        return true;
    }
    switch (aml[0]) {
        case AML_STORE_OP:
            if (!aml_term_span_depth(scope, aml + 1U, available - 1U,
                                     &term, depth + 1U))
                return false;
            if (!aml_target_span(scope, aml + 1U + term,
                                 available - 1U - term, &used, depth + 1U))
                return false;
            *span = 1U + term + used;
            return true;
        case AML_REF_OF_OP:
        case AML_INCREMENT_OP:
        case AML_DECREMENT_OP:
        case AML_SIZE_OF_OP:
            if (!aml_target_span(scope, aml + 1U, available - 1U,
                                 &term, depth + 1U))
                return false;
            *span = 1U + term;
            return true;
        case AML_ADD_OP:
        case AML_SUBTRACT_OP:
        case AML_MULTIPLY_OP:
        case AML_SHIFT_LEFT_OP:
        case AML_SHIFT_RIGHT_OP:
        case AML_AND_OP:
        case AML_NAND_OP:
        case AML_OR_OP:
        case AML_NOR_OP:
        case AML_XOR_OP:
        case AML_MOD_OP:
        case AML_INDEX_OP:
            offset = 1U;
            for (u8 i = 0; i < 2U; i++) {
                if (!aml_term_span_depth(scope, aml + offset,
                                         available - offset, &term,
                                         depth + 1U))
                    return false;
                offset += term;
            }
            if (!aml_target_span(scope, aml + offset, available - offset,
                                 &term, depth + 1U))
                return false;
            *span = offset + term;
            return true;
        case AML_NOT_OP:
        case AML_TO_BUFFER_OP:
        case AML_TO_INTEGER_OP:
            if (!aml_term_span_depth(scope, aml + 1U, available - 1U,
                                     &term, depth + 1U))
                return false;
            if (!aml_target_span(scope, aml + 1U + term,
                                 available - 1U - term, &used, depth + 1U))
                return false;
            *span = 1U + term + used;
            return true;
        case AML_DIVIDE_OP:
            offset = 1U;
            for (u8 i = 0; i < 2U; i++) {
                if (!aml_term_span_depth(scope, aml + offset,
                                         available - offset, &term,
                                         depth + 1U))
                    return false;
                offset += term;
            }
            for (u8 i = 0; i < 2U; i++) {
                if (!aml_target_span(scope, aml + offset,
                                     available - offset, &term, depth + 1U))
                    return false;
                offset += term;
            }
            *span = offset;
            return true;
        case AML_DEREF_OF_OP:
        case AML_LNOT_OP:
            return aml_terms_span(scope, aml, available, 1U, span, depth);
        case AML_LAND_OP:
        case AML_LOR_OP:
        case AML_LEQUAL_OP:
        case AML_LGREATER_OP:
        case AML_LLESS_OP:
            return aml_terms_span(scope, aml, available, 2U, span, depth);
        case AML_NOTIFY_OP:
            if (!aml_target_span(scope, aml + 1U, available - 1U,
                                 &term, depth + 1U))
                return false;
            if (!aml_term_span_depth(scope, aml + 1U + term,
                                     available - 1U - term, &used,
                                     depth + 1U))
                return false;
            *span = 1U + term + used;
            return true;
        case AML_CREATE_DWORD_FIELD:
        case AML_CREATE_WORD_FIELD:
        case AML_CREATE_BYTE_FIELD:
        case AML_CREATE_BIT_FIELD:
        case AML_CREATE_QWORD_FIELD:
            offset = 1U;
            for (u8 i = 0; i < 2U; i++) {
                if (!aml_term_span_depth(scope, aml + offset,
                                         available - offset, &term,
                                         depth + 1U))
                    return false;
                offset += term;
            }
            if (!aml_parse_name(aml + offset, available - offset,
                                &path, &term))
                return false;
            *span = offset + term;
            return true;
        case AML_TO_STRING_OP:
            offset = 1U;
            for (u8 i = 0; i < 2U; i++) {
                if (!aml_term_span_depth(scope, aml + offset,
                                         available - offset, &term,
                                         depth + 1U))
                    return false;
                offset += term;
            }
            if (!aml_target_span(scope, aml + offset, available - offset,
                                 &term, depth + 1U))
                return false;
            *span = offset + term;
            return true;
        case AML_RETURN_OP:
            if (available == 1U) {
                *span = 1U;
                return true;
            }
            if (!aml_term_span_depth(scope, aml + 1U, available - 1U,
                                     &term, depth + 1U))
                return false;
            *span = 1U + term;
            return true;
        case AML_NOOP_OP:
        case AML_BREAK_OP:
        case AML_CONTINUE_OP:
            *span = 1U;
            return true;
        default:
            break;
    }
    if (aml[0] == AML_EXT_OP) {
        if (available < 2U)
            return false;
        switch (aml[1]) {
            case AML_EXT_MUTEX_OP:
                if (!aml_parse_name(aml + 2U, available - 2U, &path, &used) ||
                    available - 2U - used < 1U)
                    return false;
                *span = 3U + used;
                return true;
            case AML_EXT_EVENT_OP:
                if (!aml_parse_name(aml + 2U, available - 2U, &path, &used))
                    return false;
                *span = 2U + used;
                return true;
            case AML_EXT_COND_REF_OF_OP:
                if (!aml_target_span(scope, aml + 2U, available - 2U,
                                     &term, depth + 1U))
                    return false;
                if (!aml_target_span(scope, aml + 2U + term,
                                     available - 2U - term, &used,
                                     depth + 1U))
                    return false;
                *span = 2U + term + used;
                return true;
            case AML_EXT_CREATE_FIELD_OP:
                offset = 2U;
                for (u8 i = 0; i < 3U; i++) {
                    if (!aml_term_span_depth(scope, aml + offset,
                                             available - offset, &term,
                                             depth + 1U))
                        return false;
                    offset += term;
                }
                if (!aml_parse_name(aml + offset, available - offset,
                                    &path, &term))
                    return false;
                *span = offset + term;
                return true;
            case AML_EXT_STALL_OP:
            case AML_EXT_SLEEP_OP:
                if (!aml_term_span_depth(scope, aml + 2U, available - 2U,
                                         &term, depth + 1U))
                    return false;
                *span = 2U + term;
                return true;
            case AML_EXT_ACQUIRE_OP:
                if (!aml_target_span(scope, aml + 2U, available - 2U,
                                     &term, depth + 1U) ||
                    available - 2U - term < 2U)
                    return false;
                *span = 4U + term;
                return true;
            case AML_EXT_RELEASE_OP:
                if (!aml_target_span(scope, aml + 2U, available - 2U,
                                     &term, depth + 1U))
                    return false;
                *span = 2U + term;
                return true;
            case AML_EXT_OP_REGION_OP:
                offset = 2U;
                if (!aml_parse_name(aml + offset, available - offset,
                                    &path, &term))
                    return false;
                offset += term;
                if (available - offset < 1U)
                    return false;
                offset++;
                for (u8 i = 0; i < 2U; i++) {
                    if (!aml_term_span_depth(scope, aml + offset,
                                             available - offset, &term,
                                             depth + 1U))
                        return false;
                    offset += term;
                }
                *span = offset;
                return true;
            case AML_EXT_FIELD_OP:
            case AML_EXT_DEVICE_OP:
            case AML_EXT_PROCESSOR_OP:
            case AML_EXT_POWER_RES_OP:
            case AML_EXT_THERMAL_ZONE_OP:
            case AML_EXT_INDEX_FIELD_OP:
            case AML_EXT_BANK_FIELD_OP:
                if (!aml_package_span(aml + 2U, available - 2U,
                                      &encoded, &package))
                    return false;
                *span = 2U + package;
                return true;
            default:
                return false;
        }
    }
    if (aml_name_starts(aml, available) &&
        aml_parse_name(aml, available, &path, &used)) {
        handle = aml_resolve(scope, &path);
        offset = used;
        if (handle != AML_HANDLE_INVALID &&
            (aml_nodes[handle].kind == AML_NODE_METHOD ||
             aml_nodes[handle].kind == AML_NODE_NATIVE_METHOD)) {
            u8 arguments = aml_nodes[handle].object.method.argument_count;
            for (u8 i = 0; i < arguments; i++) {
                if (!aml_term_span_depth(scope, aml + offset,
                                         available - offset, &term,
                                         depth + 1U))
                    return false;
                offset += term;
            }
        }
        *span = offset;
        return true;
    }
    return false;
}

static bool aml_term_span(aml_handle_t scope, const u8 *aml, u32 available,
                          u32 *span)
{
    return aml_term_span_depth(scope, aml, available, span, 0U);
}

static aml_handle_t aml_resolve_encoded(aml_handle_t scope, const u8 *aml,
                                        u32 available, u32 *used)
{
    aml_name_path_t path;

    if (!aml_parse_name(aml, available, &path, used))
        return AML_HANDLE_INVALID;
    return aml_resolve(scope, &path);
}

static bool aml_load_termlist(aml_handle_t scope, const u8 *aml,
                              u32 length, u8 table_index,
                              const u8 *table_start);

static bool aml_load_field_list(aml_handle_t scope, const u8 *aml, u32 length,
                                u8 flags, aml_node_kind_t kind,
                                aml_handle_t first, aml_handle_t second,
                                const u8 *source_name, u32 source_name_length,
                                const u8 *bank_expression,
                                u32 bank_expression_length,
                                u8 table_index, const u8 *table_start)
{
    u32 offset = 0;
    u32 bit_offset = 0;
    u8 current_flags = flags;
    u8 access_attribute = 0;
    u8 access_length = 0;

    while (offset < length) {
        u32 encoded;
        u32 bits;
        u32 name;
        aml_handle_t field;

        if (aml[offset] == 0x00U) { /* ReservedField */
            if (!aml_package_length_raw(aml + offset + 1U,
                                        length - offset - 1U,
                                        &encoded, &bits))
                return false;
            if (bits > 0xffffffffU - bit_offset)
                return false;
            bit_offset += bits;
            offset += 1U + encoded;
            continue;
        }
        if (aml[offset] == 0x01U) { /* AccessField */
            if (length - offset < 3U)
                return false;
            current_flags = (current_flags & ~AML_FIELD_ACCESS_MASK) |
                            (aml[offset + 1U] & AML_FIELD_ACCESS_MASK);
            access_attribute = aml[offset + 2U];
            access_length = 0;
            offset += 3U;
            continue;
        }
        if (aml[offset] == 0x02U) { /* ConnectField */
            u32 connection;
            if (length - offset < 2U)
                return false;
            if (aml[offset + 1U] == AML_BUFFER_OP) {
                if (!aml_term_span(scope, aml + offset + 1U,
                                   length - offset - 1U, &connection))
                    return false;
            } else {
                aml_name_path_t connection_name;
                if (!aml_parse_name(aml + offset + 1U,
                                    length - offset - 1U,
                                    &connection_name, &connection))
                    return false;
            }
            offset += 1U + connection;
            continue;
        }
        if (aml[offset] == 0x03U) { /* ExtendedAccessField */
            if (length - offset < 4U)
                return false;
            current_flags = (current_flags & ~AML_FIELD_ACCESS_MASK) |
                            (aml[offset + 1U] & AML_FIELD_ACCESS_MASK);
            access_attribute = aml[offset + 2U];
            access_length = aml[offset + 3U];
            offset += 4U;
            continue;
        }
        if (length - offset < 5U ||
            !aml_name_segment_valid(aml + offset) ||
            !aml_package_length_raw(aml + offset + 4U,
                                    length - offset - 4U,
                                    &encoded, &bits) || !bits ||
            bits > 0xffffffffU - bit_offset)
            return false;
        name = aml_name_pack(aml + offset);
        field = aml_create_node(scope, name, kind, table_index,
                                (u32)((aml + offset) - table_start));
        if (field == AML_HANDLE_INVALID)
            return false;
        aml_nodes[field].object.field = (aml_field_object_t){
            .region = kind == AML_NODE_FIELD || kind == AML_NODE_BANK_FIELD ?
                      first : AML_HANDLE_INVALID,
            .index_register = kind == AML_NODE_INDEX_FIELD ? first :
                              AML_HANDLE_INVALID,
            .data_register = kind == AML_NODE_INDEX_FIELD ? second :
                             AML_HANDLE_INVALID,
            .bank_register = kind == AML_NODE_BANK_FIELD ? second :
                             AML_HANDLE_INVALID,
            .bank_expression = bank_expression,
            .bank_expression_length = bank_expression_length,
            .source_name = source_name,
            .source_name_length = source_name_length,
            .bit_offset = bit_offset,
            .bit_length = bits,
            .flags = current_flags,
            .access_attribute = access_attribute,
            .access_length = access_length,
        };
        bit_offset += bits;
        offset += 4U + encoded;
    }
    return offset == length;
}

static bool aml_load_field(aml_handle_t scope, const u8 *aml, u32 available,
                           u8 table_index, const u8 *table_start,
                           u32 *span)
{
    aml_name_path_t source_path;
    aml_handle_t first = AML_HANDLE_INVALID;
    aml_handle_t second = AML_HANDLE_INVALID;
    aml_node_kind_t kind;
    const u8 *source_name;
    const u8 *bank_expression = NULL;
    u32 source_name_length;
    u32 bank_expression_length = 0;
    u32 encoded;
    u32 package;
    u32 content;
    u32 offset;
    u32 used;
    u8 flags;

    if (available < 3U || aml[0] != AML_EXT_OP ||
        !aml_package_span(aml + 2U, available - 2U, &encoded, &package))
        return false;
    if (aml[1] == AML_EXT_FIELD_OP)
        kind = AML_NODE_FIELD;
    else if (aml[1] == AML_EXT_INDEX_FIELD_OP)
        kind = AML_NODE_INDEX_FIELD;
    else if (aml[1] == AML_EXT_BANK_FIELD_OP)
        kind = AML_NODE_BANK_FIELD;
    else
        return false;
    content = package - encoded;
    offset = 2U + encoded;
    source_name = aml + offset;
    if (!aml_parse_name(aml + offset, content, &source_path, &used))
        return false;
    source_name_length = used;
    first = aml_resolve(scope, &source_path);
    offset += used;
    content -= used;
    if (kind != AML_NODE_FIELD) {
        aml_name_path_t second_path;
        if (!aml_parse_name(aml + offset, content, &second_path, &used))
            return false;
        second = aml_resolve(scope, &second_path);
        offset += used;
        content -= used;
    }
    if (kind == AML_NODE_BANK_FIELD) {
        bank_expression = aml + offset;
        if (!aml_term_span(scope, aml + offset, content,
                           &bank_expression_length))
            return false;
        offset += bank_expression_length;
        content -= bank_expression_length;
    }
    if (!content)
        return false;
    flags = aml[offset++];
    content--;
    if (!aml_load_field_list(scope, aml + offset, content, flags, kind,
                             first, second, source_name, source_name_length,
                             bank_expression, bank_expression_length,
                             table_index, table_start))
        return false;
    *span = 2U + package;
    return true;
}

static bool aml_load_termlist(aml_handle_t scope, const u8 *aml,
                              u32 length, u8 table_index,
                              const u8 *table_start)
{
    u32 offset = 0;
    const u8 *failure_term = aml;

    while (offset < length) {
        const u8 *term = aml + offset;
        u32 available = length - offset;
        u32 encoded;
        u32 package;
        u32 name_used;
        u32 term_span;
        aml_name_path_t path;
        aml_handle_t node;

        failure_term = term;

        if (term[0] == AML_NAME_OP) {
            u32 value_span;
            if (!aml_parse_name(term + 1U, available - 1U,
                                &path, &name_used) ||
                !aml_term_span(scope, term + 1U + name_used,
                               available - 1U - name_used, &value_span))
                goto malformed;
            node = aml_create_path(scope, &path, AML_NODE_NAME, table_index,
                                   (u32)(term - table_start));
            if (node == AML_HANDLE_INVALID)
                goto malformed;
            aml_nodes[node].object.name.expression = term + 1U + name_used;
            aml_nodes[node].object.name.expression_length = value_span;
            offset += 1U + name_used + value_span;
            continue;
        }
        if (term[0] == AML_ALIAS_OP) {
            aml_name_path_t alias_path;
            u32 source_used;
            if (!aml_parse_name(term + 1U, available - 1U,
                                &path, &source_used) ||
                !aml_parse_name(term + 1U + source_used,
                                available - 1U - source_used,
                                &alias_path, &name_used))
                goto malformed;
            node = aml_create_path(scope, &alias_path, AML_NODE_ALIAS,
                                   table_index, (u32)(term - table_start));
            if (node == AML_HANDLE_INVALID)
                goto malformed;
            aml_nodes[node].object.alias.source_name = term + 1U;
            aml_nodes[node].object.alias.source_name_length = source_used;
            aml_nodes[node].object.alias.target = AML_HANDLE_INVALID;
            offset += 1U + source_used + name_used;
            continue;
        }
        if (term[0] == AML_EXTERNAL_OP) {
            if (!aml_parse_name(term + 1U, available - 1U,
                                &path, &name_used) ||
                available - 1U - name_used < 2U)
                goto malformed;
            node = aml_create_path(scope, &path, AML_NODE_EXTERNAL,
                                   table_index, (u32)(term - table_start));
            if (node != AML_HANDLE_INVALID &&
                aml_nodes[node].kind == AML_NODE_EXTERNAL) {
                aml_nodes[node].object.external.object_type =
                    term[1U + name_used];
                aml_nodes[node].object.external.argument_count =
                    term[2U + name_used];
            }
            offset += 3U + name_used;
            continue;
        }
        if (term[0] == AML_SCOPE_OP || term[0] == AML_METHOD_OP) {
            u32 body_offset;
            u32 body_length;
            if (!aml_package_span(term + 1U, available - 1U,
                                  &encoded, &package) ||
                !aml_parse_name(term + 1U + encoded, package - encoded,
                                &path, &name_used))
                goto malformed;
            body_offset = 1U + encoded + name_used;
            body_length = package - encoded - name_used;
            if (term[0] == AML_SCOPE_OP) {
                node = aml_ensure_scope_path(scope, &path, table_index,
                                             (u32)(term - table_start));
                if (node == AML_HANDLE_INVALID ||
                    !aml_load_termlist(node, term + body_offset, body_length,
                                       table_index, table_start)) {
                    if (aml_failure.kind == AML_FAILURE_NONE)
                        aml_note_failure(AML_FAILURE_MALFORMED_AML,
                                         term[0], scope, NULL, table_index,
                                         (u32)(term - table_start));
                    return false;
                }
            } else {
                if (!body_length)
                    goto malformed;
                node = aml_create_path(scope, &path, AML_NODE_METHOD,
                                       table_index,
                                       (u32)(term - table_start));
                if (node == AML_HANDLE_INVALID)
                    goto malformed;
                aml_nodes[node].object.method.body = term + body_offset + 1U;
                aml_nodes[node].object.method.body_length = body_length - 1U;
                aml_nodes[node].object.method.argument_count =
                    term[body_offset] & 0x07U;
                aml_nodes[node].object.method.serialized =
                    (term[body_offset] & 0x08U) != 0;
                aml_nodes[node].object.method.sync_level =
                    (term[body_offset] >> 4) & 0x0fU;
            }
            offset += 1U + package;
            continue;
        }
        if (term[0] == AML_EXT_OP && available >= 2U &&
            (term[1] == AML_EXT_FIELD_OP ||
             term[1] == AML_EXT_INDEX_FIELD_OP ||
             term[1] == AML_EXT_BANK_FIELD_OP)) {
            if (!aml_load_field(scope, term, available, table_index,
                                table_start, &term_span))
                goto malformed;
            offset += term_span;
            continue;
        }
        if (term[0] == AML_EXT_OP && available >= 2U &&
            term[1] == AML_EXT_OP_REGION_OP) {
            u32 cursor = 2U;
            u32 first_span;
            u32 second_span;
            if (!aml_parse_name(term + cursor, available - cursor,
                                &path, &name_used))
                goto malformed;
            cursor += name_used;
            if (available - cursor < 1U)
                goto malformed;
            u8 space = term[cursor++];
            if (!aml_term_span(scope, term + cursor, available - cursor,
                               &first_span))
                goto malformed;
            if (!aml_term_span(scope, term + cursor + first_span,
                               available - cursor - first_span,
                               &second_span))
                goto malformed;
            node = aml_create_path(scope, &path, AML_NODE_REGION,
                                   table_index, (u32)(term - table_start));
            if (node == AML_HANDLE_INVALID)
                goto malformed;
            aml_nodes[node].object.region = (aml_region_object_t){
                .space = space,
                .offset_expression = term + cursor,
                .offset_length = first_span,
                .length_expression = term + cursor + first_span,
                .length_length = second_span,
            };
            offset += cursor + first_span + second_span;
            continue;
        }
        if (term[0] == AML_EXT_OP && available >= 2U &&
            (term[1] == AML_EXT_MUTEX_OP || term[1] == AML_EXT_EVENT_OP)) {
            if (!aml_parse_name(term + 2U, available - 2U,
                                &path, &name_used))
                goto malformed;
            node = aml_create_path(scope, &path,
                                   term[1] == AML_EXT_MUTEX_OP ?
                                       AML_NODE_MUTEX : AML_NODE_EVENT,
                                   table_index, (u32)(term - table_start));
            if (node == AML_HANDLE_INVALID)
                goto malformed;
            if (term[1] == AML_EXT_MUTEX_OP) {
                if (available - 2U - name_used < 1U)
                    goto malformed;
                aml_nodes[node].object.mutex.sync_level =
                    term[2U + name_used] & 0x0fU;
                offset += 3U + name_used;
            } else {
                offset += 2U + name_used;
            }
            continue;
        }
        if (term[0] == AML_EXT_OP && available >= 2U &&
            (term[1] == AML_EXT_DEVICE_OP ||
             term[1] == AML_EXT_PROCESSOR_OP ||
             term[1] == AML_EXT_POWER_RES_OP ||
             term[1] == AML_EXT_THERMAL_ZONE_OP)) {
            u32 body_offset;
            u32 body_length;
            u32 fixed = 0;
            aml_node_kind_t kind = AML_NODE_SCOPE;
            if (!aml_package_span(term + 2U, available - 2U,
                                  &encoded, &package) ||
                !aml_parse_name(term + 2U + encoded, package - encoded,
                                &path, &name_used))
                goto malformed;
            if (term[1] == AML_EXT_DEVICE_OP)
                kind = AML_NODE_DEVICE;
            else if (term[1] == AML_EXT_THERMAL_ZONE_OP)
                kind = AML_NODE_THERMAL_ZONE;
            else if (term[1] == AML_EXT_PROCESSOR_OP)
                fixed = 6U;
            else
                fixed = 3U;
            if (package - encoded - name_used < fixed)
                goto malformed;
            node = aml_create_path(scope, &path, kind, table_index,
                                   (u32)(term - table_start));
            if (node == AML_HANDLE_INVALID)
                goto malformed;
            body_offset = 2U + encoded + name_used + fixed;
            body_length = package - encoded - name_used - fixed;
            if (!aml_load_termlist(node, term + body_offset, body_length,
                                   table_index, table_start)) {
                if (aml_failure.kind == AML_FAILURE_NONE)
                    aml_note_failure(AML_FAILURE_MALFORMED_AML, term[0],
                                     scope, NULL, table_index,
                                     (u32)(term - table_start));
                return false;
            }
            offset += 2U + package;
            continue;
        }

        if (!aml_term_span(scope, term, available, &term_span) || !term_span)
            goto malformed;
        offset += term_span;
    }
    return offset == length;

malformed:
    aml_note_failure(AML_FAILURE_MALFORMED_AML,
                     failure_term ? failure_term[0] : 0, scope, NULL,
                     table_index,
                     failure_term ? (u32)(failure_term - table_start) : 0);
    return false;
}

static bool aml_table_checksum_valid(const acpi_sdt_header_t *table)
{
    const u8 *bytes = (const u8 *)table;
    u8 checksum = 0;

    if (!table || table->length < sizeof(*table) ||
        table->length > 16U * 1024U * 1024U)
        return false;
    for (u32 i = 0; i < table->length; i++)
        checksum += bytes[i];
    return checksum == 0;
}

static bool aml_signature_is(const acpi_sdt_header_t *table,
                             const char signature[4])
{
    return table && memcmp(table->signature, signature, 4U) == 0;
}

static bool aml_global_lock_prepare(void)
{
    const acpi_fadt_info_t *fadt = acpi_fadt_get();
    const u8 *facs;
    u32 length;

    aml_global_lock = NULL;
    if (!fadt || !fadt->x_firmware_control ||
        !phys_map_contains((phys_addr_t)fadt->x_firmware_control))
        return false;
    facs = (const u8 *)phys_to_virt((phys_addr_t)fadt->x_firmware_control);
    if (!facs || memcmp(facs, "FACS", 4U) != 0)
        return false;
    length = (u32)facs[4] | ((u32)facs[5] << 8) |
             ((u32)facs[6] << 16) | ((u32)facs[7] << 24);
    if (length < AML_FACS_GLOBAL_LOCK_OFFSET + sizeof(u32))
        return false;
    aml_global_lock = (volatile u32 *)(facs + AML_FACS_GLOBAL_LOCK_OFFSET);
    return true;
}

static bool aml_pm1_global_lock_release(void)
{
    const acpi_fadt_info_t *fadt = acpi_fadt_get();
    const acpi_generic_address_t *gas;
    volatile u16 *memory;
    u16 value;

    if (!fadt)
        return false;
    gas = &fadt->pm1a_control_block;
    if (!gas->address || gas->register_bit_offset != 0 ||
        gas->register_bit_width < 16U)
        return false;
    if (gas->address_space_id == ACPI_ADDRESS_SPACE_SYSTEM_IO) {
        if (gas->address > 0xffffU)
            return false;
        value = inw((u16)gas->address);
        outw((u16)gas->address,
             (u16)(value | AML_PM1_GLOBAL_LOCK_RELEASE));
        return true;
    }
    if (gas->address_space_id != ACPI_ADDRESS_SPACE_SYSTEM_MEMORY)
        return false;
    memory = (volatile u16 *)vmm_map_mmio((phys_addr_t)gas->address,
                                          sizeof(u16));
    if (!memory)
        return false;
    value = *memory;
    __asm__ volatile("" ::: "memory");
    *memory = (u16)(value | AML_PM1_GLOBAL_LOCK_RELEASE);
    __asm__ volatile("" ::: "memory");
    return true;
}

static bool aml_global_lock_acquire(aml_execution_t *execution)
{
    timer_monotonic_deadline_t deadline;

    if (!aml_global_lock)
        return true;
    if (!timer_monotonic_deadline_start(&deadline,
                                        AML_METHOD_TIMEOUT_US))
        return false;
    for (;;) {
        u32 old = __atomic_load_n(aml_global_lock, __ATOMIC_ACQUIRE);
        u32 desired = old & ~3U;

        if (old & 2U)
            desired |= 1U;
        else
            desired |= 2U;
        if (__atomic_compare_exchange_n(aml_global_lock, &old, desired,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE) &&
            !(old & 2U))
            return true;
        if ((execution && !aml_budget_step(execution, AML_EXT_FIELD_OP)) ||
            timer_monotonic_deadline_expired(&deadline)) {
            aml_note_failure(AML_FAILURE_TIMEOUT, AML_EXT_FIELD_OP,
                             execution ? execution->scope : AML_ROOT_NODE,
                             NULL, 0, 0);
            return false;
        }
        __asm__ volatile("pause");
    }
}

static void aml_global_lock_release(void)
{
    if (aml_global_lock) {
        u32 old;
        u32 desired;
        do {
            old = __atomic_load_n(aml_global_lock, __ATOMIC_ACQUIRE);
            desired = old & ~3U;
        } while (!__atomic_compare_exchange_n(aml_global_lock, &old,
                                               desired, false,
                                               __ATOMIC_RELEASE,
                                               __ATOMIC_ACQUIRE));
        if (old & 1U)
            (void)aml_pm1_global_lock_release();
    }
}

static aml_handle_t aml_alias_target(aml_handle_t handle)
{
    for (u8 depth = 0; depth < AML_MAX_METHOD_DEPTH; depth++) {
        aml_alias_object_t *alias;
        aml_handle_t target;
        u32 used;

        if (handle >= aml_node_count ||
            aml_nodes[handle].kind != AML_NODE_ALIAS)
            return handle;
        alias = &aml_nodes[handle].object.alias;
        target = alias->target;
        if (target == AML_HANDLE_INVALID) {
            target = aml_resolve_encoded(aml_nodes[handle].parent,
                                         alias->source_name,
                                         alias->source_name_length, &used);
            if (target == AML_HANDLE_INVALID ||
                used != alias->source_name_length)
                return AML_HANDLE_INVALID;
            alias->target = target;
        }
        handle = target;
    }
    return AML_HANDLE_INVALID;
}

static bool aml_region_evaluate(aml_execution_t *execution,
                                aml_handle_t region_handle)
{
    aml_region_object_t *region;
    aml_value_t offset_value = {0};
    aml_value_t length_value = {0};
    u64 offset;
    u64 length;
    u32 used;

    if (!execution)
        return false;
    region_handle = aml_alias_target(region_handle);
    if (region_handle == AML_HANDLE_INVALID ||
        aml_nodes[region_handle].kind != AML_NODE_REGION)
        return false;
    region = &aml_nodes[region_handle].object.region;
    if (region->evaluated)
        return true;
    if (!aml_eval_term(execution, region->offset_expression,
                       region->offset_length, &offset_value, &used) ||
        used != region->offset_length ||
        !aml_value_to_integer(&offset_value, &offset) ||
        !aml_eval_term(execution, region->length_expression,
                       region->length_length, &length_value, &used) ||
        used != region->length_length ||
        !aml_value_to_integer(&length_value, &length) || !length ||
        offset > ~(u64)0 - (length - 1U)) {
        aml_value_release(&offset_value);
        aml_value_release(&length_value);
        return false;
    }
    aml_value_release(&offset_value);
    aml_value_release(&length_value);
    region->evaluated_offset = offset;
    region->evaluated_length = length;
    region->mapping = NULL;
    if (region->space == AML_REGION_SYSTEM_MEMORY) {
        region->mapping = (volatile u8 *)vmm_map_mmio((phys_addr_t)offset,
                                                       length);
        if (!region->mapping)
            return false;
    } else if (region->space == AML_REGION_SYSTEM_IO) {
        if (offset > 0xffffU || length > 0x10000ULL - offset)
            return false;
    } else if (region->space != AML_REGION_EMBEDDED_CTRL) {
        aml_note_failure(AML_FAILURE_UNSUPPORTED_REGION, region->space,
                         aml_nodes[region_handle].parent, NULL,
                         aml_nodes[region_handle].table_index,
                         aml_nodes[region_handle].table_offset);
        return false;
    }
    region->evaluated = true;
    return true;
}

static bool aml_runtime_region_evaluate(aml_execution_t *execution,
                                        aml_region_object_t *region)
{
    aml_value_t offset_value = {0};
    aml_value_t length_value = {0};
    u64 offset;
    u64 length;
    u32 used;

    if (!execution || !region)
        return false;
    if (region->evaluated)
        return true;
    if (!aml_eval_term(execution, region->offset_expression,
                       region->offset_length, &offset_value, &used) ||
        used != region->offset_length ||
        !aml_execution_integer(execution, &offset_value, &offset) ||
        !aml_eval_term(execution, region->length_expression,
                       region->length_length, &length_value, &used) ||
        used != region->length_length ||
        !aml_execution_integer(execution, &length_value, &length) || !length ||
        offset > ~(u64)0 - (length - 1U)) {
        aml_value_release(&offset_value);
        aml_value_release(&length_value);
        return false;
    }
    aml_value_release(&offset_value);
    aml_value_release(&length_value);
    region->evaluated_offset = offset;
    region->evaluated_length = length;
    if (region->space == AML_REGION_SYSTEM_MEMORY) {
        region->mapping = (volatile u8 *)vmm_map_mmio((phys_addr_t)offset,
                                                       length);
        if (!region->mapping)
            return false;
    } else if (region->space == AML_REGION_SYSTEM_IO) {
        if (offset > 0xffffU || length > 0x10000ULL - offset)
            return false;
    } else if (region->space != AML_REGION_EMBEDDED_CTRL) {
        return false;
    }
    region->evaluated = true;
    return true;
}

static bool aml_region_object_read(aml_region_object_t *region, u64 offset,
                                   u8 width, u64 *value)
{
    u64 result = 0;

    if (!region || !region->evaluated || !value ||
        (width != 1U && width != 2U && width != 4U && width != 8U) ||
        offset > region->evaluated_length ||
        width > region->evaluated_length - offset)
        return false;
    if (region->space == AML_REGION_EMBEDDED_CTRL) {
        if (!region->connected || !aml_ec_connected || !ec_available() ||
            region->evaluated_offset > 0xffU ||
            offset > 0xffU - region->evaluated_offset ||
            (u64)width - 1U >
                0xffU - region->evaluated_offset - offset)
            return false;
        for (u8 i = 0; i < width; i++) {
            u8 byte;
            if (!ec_read((u8)(region->evaluated_offset + offset + i), &byte))
                return false;
            result |= (u64)byte << (8U * i);
        }
    } else if (region->space == AML_REGION_SYSTEM_IO) {
        u16 port = (u16)(region->evaluated_offset + offset);
        if (width == 1U) result = inb(port);
        else if (width == 2U) result = inw(port);
        else if (width == 4U) result = inl(port);
        else {
            result = inl(port);
            result |= (u64)inl((u16)(port + 4U)) << 32;
        }
    } else if (region->space == AML_REGION_SYSTEM_MEMORY &&
               region->mapping) {
        volatile u8 *address = region->mapping + offset;
        if (width == 1U) result = *(volatile u8 *)address;
        else if (width == 2U) result = *(volatile u16 *)address;
        else if (width == 4U) result = *(volatile u32 *)address;
        else result = *(volatile u64 *)address;
        __asm__ volatile("" ::: "memory");
    } else {
        return false;
    }
    *value = result;
    return true;
}

static bool aml_region_object_write(aml_region_object_t *region, u64 offset,
                                    u8 width, u64 value)
{
    if (!region || !region->evaluated ||
        (width != 1U && width != 2U && width != 4U && width != 8U) ||
        offset > region->evaluated_length ||
        width > region->evaluated_length - offset)
        return false;
    if (offset > region->evaluated_length ||
        width > region->evaluated_length - offset)
        return false;
    if (region->space == AML_REGION_EMBEDDED_CTRL) {
        if (!region->connected || !aml_ec_connected || !ec_available() ||
            region->evaluated_offset > 0xffU ||
            offset > 0xffU - region->evaluated_offset ||
            (u64)width - 1U >
                0xffU - region->evaluated_offset - offset)
            return false;
        for (u8 i = 0; i < width; i++) {
            if (!ec_write((u8)(region->evaluated_offset + offset + i),
                          (u8)(value >> (8U * i))))
                return false;
        }
    } else if (region->space == AML_REGION_SYSTEM_IO) {
        u16 port = (u16)(region->evaluated_offset + offset);
        if (width == 1U)
            outb(port, (u8)value);
        else if (width == 2U)
            outw(port, (u16)value);
        else if (width == 4U)
            outl(port, (u32)value);
        else {
            outl(port, (u32)value);
            outl((u16)(port + 4U), (u32)(value >> 32));
        }
    } else if (region->space == AML_REGION_SYSTEM_MEMORY &&
               region->mapping) {
        volatile u8 *address = region->mapping + offset;
        __asm__ volatile("" ::: "memory");
        if (width == 1U)
            *(volatile u8 *)address = (u8)value;
        else if (width == 2U)
            *(volatile u16 *)address = (u16)value;
        else if (width == 4U)
            *(volatile u32 *)address = (u32)value;
        else
            *(volatile u64 *)address = value;
        __asm__ volatile("" ::: "memory");
    } else return false;
    return true;
}

static bool aml_region_read_unit(aml_execution_t *execution,
                                 aml_handle_t region_handle, u64 offset,
                                 u8 width, u64 *value)
{
    if (!aml_region_evaluate(execution, region_handle))
        return false;
    region_handle = aml_alias_target(region_handle);
    return aml_region_object_read(&aml_nodes[region_handle].object.region,
                                  offset, width, value);
}

static bool aml_region_write_unit(aml_execution_t *execution,
                                  aml_handle_t region_handle, u64 offset,
                                  u8 width, u64 value)
{
    if (!aml_region_evaluate(execution, region_handle))
        return false;
    region_handle = aml_alias_target(region_handle);
    return aml_region_object_write(&aml_nodes[region_handle].object.region,
                                   offset, width, value);
}

static u8 aml_field_access_bytes(const aml_field_object_t *field)
{
    if (field->access_length == 1U || field->access_length == 2U ||
        field->access_length == 4U || field->access_length == 8U)
        return field->access_length;
    switch (field->flags & AML_FIELD_ACCESS_MASK) {
        case 2U: return 2U;
        case 3U: return 4U;
        case 4U: return 8U;
        case 1U:
        case 0U:
        default: return 1U;
    }
}

static bool aml_field_bank_select(aml_execution_t *execution,
                                  const aml_field_object_t *field)
{
    aml_value_t bank = {0};
    aml_reference_t reference = {0};
    u64 integer;
    u32 used;

    if (field->bank_register == AML_HANDLE_INVALID)
        return true;
    if (!field->bank_expression || !field->bank_expression_length ||
        !aml_eval_term(execution, field->bank_expression,
                       field->bank_expression_length, &bank, &used) ||
        used != field->bank_expression_length ||
        !aml_value_to_integer(&bank, &integer)) {
        aml_value_release(&bank);
        return false;
    }
    aml_value_release(&bank);
    reference.kind = AML_REF_NODE;
    reference.node = field->bank_register;
    bank = aml_integer_value(integer);
    return aml_write_reference(execution, &reference, &bank);
}

static bool aml_field_read_access(aml_execution_t *execution,
                                  const aml_field_object_t *field,
                                  u64 unit_bit, u8 bytes, u64 *value)
{
    aml_reference_t reference = {0};
    aml_value_t index = {0};

    if (!aml_field_bank_select(execution, field))
        return false;
    if (field->index_register != AML_HANDLE_INVALID) {
        reference.kind = AML_REF_NODE;
        reference.node = field->index_register;
        index = aml_integer_value(unit_bit / 8U);
        if (!aml_write_reference(execution, &reference, &index))
            return false;
        if (!aml_read_node(execution, field->data_register, &index) ||
            !aml_value_to_integer(&index, value)) {
            aml_value_release(&index);
            return false;
        }
        aml_value_release(&index);
        return true;
    }
    if (field->runtime_region) {
        return aml_runtime_region_evaluate(execution,
                                           field->runtime_region) &&
               aml_region_object_read(field->runtime_region,
                                      unit_bit / 8U, bytes, value);
    }
    return aml_region_read_unit(execution, field->region, unit_bit / 8U,
                                bytes, value);
}

static bool aml_field_write_access(aml_execution_t *execution,
                                   const aml_field_object_t *field,
                                   u64 unit_bit, u8 bytes, u64 value)
{
    aml_reference_t reference = {0};
    aml_value_t index = {0};
    aml_value_t data = {0};

    if (!aml_field_bank_select(execution, field))
        return false;
    if (field->index_register != AML_HANDLE_INVALID) {
        reference.kind = AML_REF_NODE;
        reference.node = field->index_register;
        index = aml_integer_value(unit_bit / 8U);
        if (!aml_write_reference(execution, &reference, &index))
            return false;
        reference.node = field->data_register;
        data = aml_integer_value(value);
        return aml_write_reference(execution, &reference, &data);
    }
    if (field->runtime_region) {
        return aml_runtime_region_evaluate(execution,
                                           field->runtime_region) &&
               aml_region_object_write(field->runtime_region,
                                       unit_bit / 8U, bytes, value);
    }
    return aml_region_write_unit(execution, field->region, unit_bit / 8U,
                                 bytes, value);
}

static bool aml_field_resolve_sources(aml_handle_t handle)
{
    aml_field_object_t *field;
    aml_handle_t source;
    u32 used;

    if (handle >= aml_node_count)
        return false;
    field = &aml_nodes[handle].object.field;
    if ((aml_nodes[handle].kind == AML_NODE_FIELD ||
         aml_nodes[handle].kind == AML_NODE_BANK_FIELD) &&
        field->region == AML_HANDLE_INVALID) {
        source = aml_resolve_encoded(aml_nodes[handle].parent,
                                     field->source_name,
                                     field->source_name_length, &used);
        if (source == AML_HANDLE_INVALID || used != field->source_name_length)
            return false;
        field->region = aml_alias_target(source);
    }
    return field->runtime_region || field->region != AML_HANDLE_INVALID ||
           (field->index_register != AML_HANDLE_INVALID &&
            field->data_register != AML_HANDLE_INVALID);
}

static u64 aml_mask_bits(u32 bits)
{
    return bits >= 64U ? ~(u64)0 : ((1ULL << bits) - 1ULL);
}

static bool aml_output_bits(aml_value_t *value, u32 bit_offset, u32 bits,
                            u64 fragment)
{
    aml_data_t *data;

    if (value->kind == AML_PUBLIC_VALUE_INTEGER) {
        value->integer |= (fragment & aml_mask_bits(bits)) << bit_offset;
        value->integer &= aml_integer_mask;
        return true;
    }
    if (value->kind != AML_PUBLIC_VALUE_BUFFER ||
        !(data = (aml_data_t *)value->object) ||
        data->kind != AML_DATA_BUFFER || bit_offset + bits > data->length * 8U)
        return false;
    for (u32 bit = 0; bit < bits; bit++) {
        u32 destination = bit_offset + bit;
        if (fragment & (1ULL << bit))
            data->value.bytes[destination / 8U] |=
                (u8)(1U << (destination % 8U));
    }
    return true;
}

static bool aml_input_bits(const aml_value_t *value, u32 bit_offset,
                           u32 bits, u64 *fragment)
{
    aml_data_t *data;
    u64 result = 0;

    if (!value || !fragment || bits > 64U)
        return false;
    if (value->kind == AML_PUBLIC_VALUE_INTEGER) {
        *fragment = bit_offset >= 64U ? 0 :
                    (value->integer >> bit_offset) & aml_mask_bits(bits);
        return true;
    }
    if ((value->kind != AML_PUBLIC_VALUE_BUFFER &&
         value->kind != AML_PUBLIC_VALUE_STRING) ||
        !(data = (aml_data_t *)value->object) ||
        (data->kind != AML_DATA_BUFFER && data->kind != AML_DATA_STRING))
        return false;
    for (u32 bit = 0; bit < bits; bit++) {
        u32 source = bit_offset + bit;
        if (source < data->length * 8U &&
            (data->value.bytes[source / 8U] &
             (u8)(1U << (source % 8U))))
            result |= 1ULL << bit;
    }
    *fragment = result;
    return true;
}

static bool aml_read_field(aml_execution_t *execution, aml_handle_t handle,
                           aml_value_t *value)
{
    aml_field_object_t *field;
    u8 bytes;
    u32 access_bits;
    u32 completed = 0;
    bool locked = false;

    if (!value || !aml_field_resolve_sources(handle))
        return false;
    field = &aml_nodes[handle].object.field;
    bytes = aml_field_access_bytes(field);
    access_bits = (u32)bytes * 8U;
    aml_value_release(value);
    if (field->bit_length <=
        (aml_integer_mask == 0xffffffffULL ? 32U : 64U)) {
        *value = aml_integer_value(0);
    } else if (!aml_buffer_value(value, (field->bit_length + 7U) / 8U)) {
        return false;
    }
    if ((field->flags & AML_FIELD_LOCK) != 0) {
        if (!aml_global_lock_acquire(execution))
            goto failed;
        locked = true;
    }
    while (completed < field->bit_length) {
        u64 absolute = (u64)field->bit_offset + completed;
        u64 unit_bit = absolute - absolute % access_bits;
        u32 within = (u32)(absolute - unit_bit);
        u32 count = access_bits - within;
        u64 unit;
        u64 fragment;

        if (count > field->bit_length - completed)
            count = field->bit_length - completed;
        if (!aml_field_read_access(execution, field, unit_bit, bytes, &unit))
            goto failed;
        fragment = (unit >> within) & aml_mask_bits(count);
        if (!aml_output_bits(value, completed, count, fragment))
            goto failed;
        completed += count;
    }
    if (locked)
        aml_global_lock_release();
    return true;

failed:
    if (locked)
        aml_global_lock_release();
    aml_value_release(value);
    aml_note_failure(AML_FAILURE_HARDWARE_ACCESS, AML_EXT_FIELD_OP,
                     aml_nodes[handle].parent, NULL,
                     aml_nodes[handle].table_index,
                     aml_nodes[handle].table_offset);
    return false;
}

static bool aml_write_field(aml_execution_t *execution, aml_handle_t handle,
                            const aml_value_t *value)
{
    aml_field_object_t *field;
    u8 bytes;
    u32 access_bits;
    u32 completed = 0;
    bool locked = false;

    if (!value || !aml_field_resolve_sources(handle))
        return false;
    field = &aml_nodes[handle].object.field;
    bytes = aml_field_access_bytes(field);
    access_bits = (u32)bytes * 8U;
    if ((field->flags & AML_FIELD_LOCK) != 0) {
        if (!aml_global_lock_acquire(execution))
            return false;
        locked = true;
    }
    while (completed < field->bit_length) {
        u64 absolute = (u64)field->bit_offset + completed;
        u64 unit_bit = absolute - absolute % access_bits;
        u32 within = (u32)(absolute - unit_bit);
        u32 count = access_bits - within;
        u64 fragment;
        u64 unit;
        u64 mask;

        if (count > field->bit_length - completed)
            count = field->bit_length - completed;
        if (!aml_input_bits(value, completed, count, &fragment))
            goto failed;
        mask = aml_mask_bits(count) << within;
        if (mask == aml_mask_bits(access_bits)) {
            unit = 0;
        } else if ((field->flags & AML_FIELD_UPDATE_MASK) ==
                   AML_FIELD_WRITE_ONES) {
            unit = aml_mask_bits(access_bits);
        } else if ((field->flags & AML_FIELD_UPDATE_MASK) ==
                   AML_FIELD_WRITE_ZEROS) {
            unit = 0;
        } else if (!aml_field_read_access(execution, field, unit_bit, bytes,
                                          &unit)) {
            goto failed;
        }
        unit = (unit & ~mask) | ((fragment << within) & mask);
        if (!aml_field_write_access(execution, field, unit_bit, bytes, unit))
            goto failed;
        completed += count;
    }
    if (locked)
        aml_global_lock_release();
    return true;

failed:
    if (locked)
        aml_global_lock_release();
    aml_note_failure(AML_FAILURE_HARDWARE_ACCESS, AML_EXT_FIELD_OP,
                     aml_nodes[handle].parent, NULL,
                     aml_nodes[handle].table_index,
                     aml_nodes[handle].table_offset);
    return false;
}

static aml_dynamic_object_t *aml_dynamic_find(aml_execution_t *execution,
                                              u32 name)
{
    if (!execution)
        return NULL;
    for (u8 i = 0; i < execution->dynamic_count; i++) {
        if (execution->dynamic[i].name == name)
            return &execution->dynamic[i];
    }
    return NULL;
}

static aml_dynamic_object_t *aml_dynamic_create(aml_execution_t *execution,
                                                u32 name,
                                                aml_node_kind_t kind)
{
    aml_dynamic_object_t *object;

    if (!execution || !name)
        return NULL;
    object = aml_dynamic_find(execution, name);
    if (object)
        return object;
    if (execution->dynamic_count >= AML_MAX_DYNAMIC_OBJECTS) {
        aml_note_failure(AML_FAILURE_NAMESPACE_FULL, 0, execution->scope,
                         NULL, 0, 0);
        return NULL;
    }
    object = &execution->dynamic[execution->dynamic_count++];
    *object = (aml_dynamic_object_t){.kind = kind, .name = name};
    return object;
}

static bool aml_dynamic_name(const aml_name_path_t *path, u32 *name)
{
    if (!path || !name || path->absolute || path->parent_prefixes ||
        path->count != 1U)
        return false;
    *name = path->segment[0];
    return true;
}

static bool aml_read_reference(aml_execution_t *execution,
                               const aml_reference_t *reference,
                               aml_value_t *value)
{
    aml_data_t *owner;
    u64 result = 0;

    if (!reference || !value)
        return false;
    switch (reference->kind) {
        case AML_REF_NODE:
            return aml_read_node(execution, reference->node, value);
        case AML_REF_SLOT:
            return reference->slot && aml_value_copy(value, reference->slot);
        case AML_REF_PACKAGE_ELEMENT:
            owner = reference->owner;
            return owner && owner->kind == AML_DATA_PACKAGE &&
                   reference->index < owner->length &&
                   aml_value_copy(value,
                                  &owner->value.elements[reference->index]);
        case AML_REF_BUFFER_FIELD:
            owner = reference->owner;
            if (!owner || (owner->kind != AML_DATA_BUFFER &&
                           owner->kind != AML_DATA_STRING) ||
                reference->bit_length > 64U ||
                reference->bit_offset + reference->bit_length >
                    owner->length * 8U)
                return false;
            for (u32 bit = 0; bit < reference->bit_length; bit++) {
                u32 source = reference->bit_offset + bit;
                if (owner->value.bytes[source / 8U] &
                    (u8)(1U << (source % 8U)))
                    result |= 1ULL << bit;
            }
            aml_value_release(value);
            *value = aml_integer_value(result);
            return true;
        case AML_REF_DYNAMIC:
            return aml_read_dynamic(execution, reference->dynamic, value);
        default:
            return false;
    }
}

static bool aml_write_reference(aml_execution_t *execution,
                                const aml_reference_t *reference,
                                const aml_value_t *value)
{
    aml_data_t *owner;
    u64 integer;
    aml_handle_t node;

    if (!reference || !value)
        return false;
    if (reference->kind == 0)
        return true;
    switch (reference->kind) {
        case AML_REF_NODE:
            node = aml_alias_target(reference->node);
            if (node == AML_HANDLE_INVALID)
                return false;
            if (aml_nodes[node].kind == AML_NODE_NAME) {
                aml_name_object_t *name = &aml_nodes[node].object.name;
                if (!aml_value_copy(&name->value, value))
                    return false;
                name->initialized = true;
                name->initializing = false;
                return true;
            }
            if (aml_nodes[node].kind == AML_NODE_FIELD ||
                aml_nodes[node].kind == AML_NODE_INDEX_FIELD ||
                aml_nodes[node].kind == AML_NODE_BANK_FIELD)
                return aml_write_field(execution, node, value);
            return false;
        case AML_REF_SLOT:
            return reference->slot && aml_value_copy(reference->slot, value);
        case AML_REF_PACKAGE_ELEMENT:
            owner = reference->owner;
            return owner && owner->kind == AML_DATA_PACKAGE &&
                   reference->index < owner->length &&
                   aml_value_copy(&owner->value.elements[reference->index],
                                  value);
        case AML_REF_BUFFER_FIELD:
            owner = reference->owner;
            if (!owner || (owner->kind != AML_DATA_BUFFER &&
                           owner->kind != AML_DATA_STRING) ||
                reference->bit_offset + reference->bit_length >
                    owner->length * 8U ||
                !aml_value_to_integer(value, &integer))
                return false;
            for (u32 bit = 0; bit < reference->bit_length; bit++) {
                u32 destination = reference->bit_offset + bit;
                u8 mask = (u8)(1U << (destination % 8U));
                if (integer & (1ULL << bit))
                    owner->value.bytes[destination / 8U] |= mask;
                else
                    owner->value.bytes[destination / 8U] &= (u8)~mask;
            }
            return true;
        case AML_REF_DYNAMIC:
            return aml_write_dynamic(execution, reference->dynamic, value);
        default:
            return false;
    }
}

static bool aml_read_node(aml_execution_t *execution, aml_handle_t handle,
                          aml_value_t *value)
{
    aml_name_object_t *name;
    aml_execution_t *initializer;
    u32 used;

    handle = aml_alias_target(handle);
    if (!value || handle == AML_HANDLE_INVALID || handle >= aml_node_count)
        return false;
    if (aml_nodes[handle].kind == AML_NODE_FIELD ||
        aml_nodes[handle].kind == AML_NODE_INDEX_FIELD ||
        aml_nodes[handle].kind == AML_NODE_BANK_FIELD)
        return aml_read_field(execution, handle, value);
    if (aml_nodes[handle].kind != AML_NODE_NAME) {
        aml_reference_t reference = {
            .kind = AML_REF_NODE,
            .node = handle,
        };
        return aml_reference_value(value, &reference);
    }
    name = &aml_nodes[handle].object.name;
    if (!name->initialized) {
        if (name->initializing || !name->expression ||
            !name->expression_length || !execution)
            return false;
        initializer = (aml_execution_t *)kmalloc(sizeof(*initializer));
        if (!initializer)
            return false;
        memset(initializer, 0, sizeof(*initializer));
        name->initializing = true;
        initializer->caller = execution;
        initializer->scope = aml_nodes[handle].parent;
        initializer->budget = execution->budget;
        if (!aml_eval_term(initializer, name->expression,
                           name->expression_length, &name->value, &used) ||
            used != name->expression_length) {
            name->initializing = false;
            aml_execution_release(initializer);
            kfree(initializer);
            return false;
        }
        aml_execution_release(initializer);
        kfree(initializer);
        name->initializing = false;
        name->initialized = true;
    }
    return aml_value_copy(value, &name->value);
}

static bool aml_value_resolve(aml_execution_t *execution,
                              const aml_value_t *source,
                              aml_value_t *resolved)
{
    const aml_reference_t *reference;

    if (!source || !resolved)
        return false;
    reference = aml_value_reference(source);
    if (!reference)
        return aml_value_copy(resolved, source);
    return aml_read_reference(execution, reference, resolved);
}

static bool aml_execution_integer(aml_execution_t *execution,
                                  const aml_value_t *value, u64 *integer)
{
    aml_value_t resolved = {0};
    bool result;

    if (!aml_value_resolve(execution, value, &resolved))
        return false;
    result = aml_value_to_integer(&resolved, integer);
    aml_value_release(&resolved);
    return result;
}

static bool aml_parse_target(aml_execution_t *execution, const u8 *aml,
                             u32 available, aml_reference_t *reference,
                             u32 *used)
{
    aml_name_path_t path;
    aml_dynamic_object_t *dynamic;
    aml_value_t value = {0};
    const aml_reference_t *evaluated;
    u32 name;
    u32 span;

    if (!execution || !aml || !available || !reference || !used)
        return false;
    *reference = (aml_reference_t){0};
    if (aml[0] == AML_ZERO_OP) {
        *used = 1U;
        return true;
    }
    if (aml[0] >= AML_LOCAL0_OP && aml[0] <= AML_LOCAL7_OP) {
        reference->kind = AML_REF_SLOT;
        reference->slot = &execution->local[aml[0] - AML_LOCAL0_OP];
        *used = 1U;
        return true;
    }
    if (aml[0] >= AML_ARG0_OP && aml[0] <= AML_ARG6_OP) {
        reference->kind = AML_REF_SLOT;
        reference->slot = &execution->argument[aml[0] - AML_ARG0_OP];
        *used = 1U;
        return true;
    }
    if (aml[0] == AML_INDEX_OP) {
        if (!aml_eval_term(execution, aml, available, &value, &span) ||
            !(evaluated = aml_value_reference(&value))) {
            aml_value_release(&value);
            return false;
        }
        *reference = *evaluated;
        if (reference->owner)
            aml_data_retain(reference->owner);
        *used = span;
        aml_value_release(&value);
        return true;
    }
    if (!aml_parse_name(aml, available, &path, used))
        return false;
    if (aml_dynamic_name(&path, &name) &&
        (dynamic = aml_dynamic_find(execution, name))) {
        reference->kind = AML_REF_DYNAMIC;
        reference->dynamic = dynamic;
        return true;
    }
    reference->node = aml_resolve(execution->scope, &path);
    if (reference->node == AML_HANDLE_INVALID) {
        aml_note_failure(AML_FAILURE_UNRESOLVED_NAME, aml[0],
                         execution->scope, &path, 0, 0);
        return false;
    }
    reference->kind = AML_REF_NODE;
    return true;
}

static void aml_reference_release(aml_reference_t *reference)
{
    if (reference && reference->owner) {
        aml_data_t *owner = reference->owner;
        aml_value_t value = {
            .kind = owner->kind == AML_DATA_PACKAGE ?
                AML_PUBLIC_VALUE_PACKAGE :
                owner->kind == AML_DATA_STRING ? AML_PUBLIC_VALUE_STRING :
                AML_PUBLIC_VALUE_BUFFER,
            .object = owner,
        };
        reference->owner = NULL;
        aml_value_release(&value);
    }
}

static bool aml_eval_buffer(aml_execution_t *execution, const u8 *aml,
                            u32 available, aml_value_t *value, u32 *used)
{
    aml_value_t size_value = {0};
    aml_data_t *buffer;
    u32 encoded;
    u32 package;
    u32 size_used;
    u64 size;
    u32 initializers;

    if (!execution || !aml || available < 2U || aml[0] != AML_BUFFER_OP ||
        !aml_package_span(aml + 1U, available - 1U, &encoded, &package) ||
        !aml_eval_term(execution, aml + 1U + encoded, package - encoded,
                       &size_value, &size_used) ||
        !aml_execution_integer(execution, &size_value, &size) ||
        size > 1024U * 1024U || size_used > package - encoded) {
        aml_value_release(&size_value);
        return false;
    }
    aml_value_release(&size_value);
    if (!aml_buffer_value(value, (u32)size))
        return false;
    buffer = (aml_data_t *)value->object;
    initializers = package - encoded - size_used;
    if (initializers > size)
        initializers = (u32)size;
    if (initializers)
        memcpy(buffer->value.bytes, aml + 1U + encoded + size_used,
               initializers);
    *used = 1U + package;
    return true;
}

static bool aml_eval_package(aml_execution_t *execution, const u8 *aml,
                             u32 available, aml_value_t *value, u32 *used)
{
    aml_value_t count_value = {0};
    aml_data_t *package_data;
    u32 encoded;
    u32 package;
    u32 cursor;
    u32 count_used = 0;
    u64 count;

    if (!execution || !aml || available < 2U ||
        (aml[0] != AML_PACKAGE_OP && aml[0] != AML_VAR_PACKAGE_OP) ||
        !aml_package_span(aml + 1U, available - 1U, &encoded, &package))
        return false;
    cursor = 1U + encoded;
    if (aml[0] == AML_PACKAGE_OP) {
        if (cursor >= 1U + package)
            return false;
        count = aml[cursor++];
    } else {
        if (!aml_eval_term(execution, aml + cursor,
                           1U + package - cursor, &count_value,
                           &count_used) ||
            !aml_execution_integer(execution, &count_value, &count)) {
            aml_value_release(&count_value);
            return false;
        }
        aml_value_release(&count_value);
        cursor += count_used;
    }
    if (count > 1024U || !aml_package_value(value, (u32)count))
        return false;
    package_data = (aml_data_t *)value->object;
    for (u32 index = 0; index < count && cursor < 1U + package; index++) {
        u32 element_used;
        if (!aml_eval_term(execution, aml + cursor,
                           1U + package - cursor,
                           &package_data->value.elements[index],
                           &element_used) || !element_used) {
            aml_value_release(value);
            return false;
        }
        cursor += element_used;
    }
    if (cursor > 1U + package) {
        aml_value_release(value);
        return false;
    }
    *used = 1U + package;
    return true;
}

static bool aml_eval_index(aml_execution_t *execution, const u8 *aml,
                           u32 available, aml_value_t *value, u32 *used)
{
    aml_value_t source = {0};
    aml_value_t resolved = {0};
    aml_value_t index_value = {0};
    aml_reference_t target = {0};
    aml_reference_t reference = {0};
    aml_data_t *data;
    u64 index;
    u32 source_used;
    u32 index_used;
    u32 target_used;
    bool result = false;

    if (!execution || !aml || available < 2U || aml[0] != AML_INDEX_OP ||
        !aml_eval_term(execution, aml + 1U, available - 1U, &source,
                       &source_used) ||
        !aml_value_resolve(execution, &source, &resolved) ||
        !aml_eval_term(execution, aml + 1U + source_used,
                       available - 1U - source_used, &index_value,
                       &index_used) ||
        !aml_execution_integer(execution, &index_value, &index) ||
        !aml_parse_target(execution,
                          aml + 1U + source_used + index_used,
                          available - 1U - source_used - index_used,
                          &target, &target_used))
        goto finished;
    data = (aml_data_t *)resolved.object;
    if (!data || index > 0xffffffffU)
        goto finished;
    if (resolved.kind == AML_PUBLIC_VALUE_PACKAGE &&
        data->kind == AML_DATA_PACKAGE && index < data->length) {
        reference.kind = AML_REF_PACKAGE_ELEMENT;
        reference.owner = data;
        reference.index = (u32)index;
    } else if ((resolved.kind == AML_PUBLIC_VALUE_BUFFER ||
                resolved.kind == AML_PUBLIC_VALUE_STRING) &&
               (data->kind == AML_DATA_BUFFER ||
                data->kind == AML_DATA_STRING) && index < data->length) {
        reference.kind = AML_REF_BUFFER_FIELD;
        reference.owner = data;
        reference.bit_offset = (u32)index * 8U;
        reference.bit_length = 8U;
    } else {
        goto finished;
    }
    if (!aml_reference_value(value, &reference) ||
        !aml_write_reference(execution, &target, value))
        goto finished;
    *used = 1U + source_used + index_used + target_used;
    result = true;

finished:
    aml_reference_release(&target);
    aml_value_release(&source);
    aml_value_release(&resolved);
    aml_value_release(&index_value);
    if (!result)
        aml_value_release(value);
    return result;
}

static bool aml_eval_binary_integer(aml_execution_t *execution,
                                    const u8 *aml, u32 available,
                                    aml_value_t *value, u32 *used)
{
    aml_value_t left_value = {0};
    aml_value_t right_value = {0};
    aml_reference_t target = {0};
    u64 left;
    u64 right;
    u64 result;
    u32 left_used;
    u32 right_used;
    u32 target_used = 0;
    bool has_target;

    if (!execution || !aml || available < 2U ||
        !aml_eval_term(execution, aml + 1U, available - 1U, &left_value,
                       &left_used) ||
        !aml_execution_integer(execution, &left_value, &left) ||
        !aml_eval_term(execution, aml + 1U + left_used,
                       available - 1U - left_used, &right_value,
                       &right_used) ||
        !aml_execution_integer(execution, &right_value, &right))
        goto failed;
    switch (aml[0]) {
        case AML_ADD_OP: result = left + right; break;
        case AML_SUBTRACT_OP: result = left - right; break;
        case AML_MULTIPLY_OP: result = left * right; break;
        case AML_SHIFT_LEFT_OP:
            result = right >= 64U ? 0 : left << right;
            break;
        case AML_SHIFT_RIGHT_OP:
            result = right >= 64U ? 0 : left >> right;
            break;
        case AML_AND_OP: result = left & right; break;
        case AML_NAND_OP: result = ~(left & right); break;
        case AML_OR_OP: result = left | right; break;
        case AML_NOR_OP: result = ~(left | right); break;
        case AML_XOR_OP: result = left ^ right; break;
        case AML_MOD_OP:
            if (!right) {
                aml_note_failure(AML_FAILURE_DIVIDE_BY_ZERO, aml[0],
                                 execution->scope, NULL, 0, 0);
                goto failed;
            }
            result = left % right;
            break;
        case AML_LAND_OP: result = left && right; break;
        case AML_LOR_OP: result = left || right; break;
        case AML_LEQUAL_OP: result = left == right; break;
        case AML_LGREATER_OP: result = left > right; break;
        case AML_LLESS_OP: result = left < right; break;
        default: goto failed;
    }
    result &= aml_integer_mask;
    aml_value_release(value);
    *value = aml_integer_value(result);
    has_target = aml[0] != AML_LAND_OP && aml[0] != AML_LOR_OP &&
                 aml[0] != AML_LEQUAL_OP && aml[0] != AML_LGREATER_OP &&
                 aml[0] != AML_LLESS_OP;
    if (has_target) {
        if (!aml_parse_target(execution,
                              aml + 1U + left_used + right_used,
                              available - 1U - left_used - right_used,
                              &target, &target_used) ||
            !aml_write_reference(execution, &target, value))
            goto failed;
    }
    *used = 1U + left_used + right_used + target_used;
    aml_reference_release(&target);
    aml_value_release(&left_value);
    aml_value_release(&right_value);
    return true;

failed:
    aml_reference_release(&target);
    aml_value_release(&left_value);
    aml_value_release(&right_value);
    aml_value_release(value);
    return false;
}

static bool aml_owned_lock(volatile u32 *owner, u32 invocation_id,
                           u64 timeout_us)
{
    timer_monotonic_deadline_t deadline;

    if (!owner || !invocation_id)
        return false;
    if (__atomic_load_n(owner, __ATOMIC_ACQUIRE) == invocation_id)
        return true;
    if (!timer_monotonic_deadline_start(&deadline, timeout_us))
        return false;
    for (;;) {
        u32 expected = 0;
        if (__atomic_compare_exchange_n(owner, &expected, invocation_id,
                                        false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            return true;
        if (timer_monotonic_deadline_expired(&deadline))
            return false;
        __asm__ volatile("pause");
    }
}

static void aml_owned_unlock(volatile u32 *owner, u32 invocation_id)
{
    if (owner && __atomic_load_n(owner, __ATOMIC_RELAXED) == invocation_id)
        __atomic_store_n(owner, 0U, __ATOMIC_RELEASE);
}

static bool aml_native_osi(const aml_value_t *argument, aml_value_t *result)
{
    aml_data_t *string;
    static const char linux_name[] = "Linux";
    bool supported;

    if (!argument || !result || argument->kind != AML_PUBLIC_VALUE_STRING ||
        !(string = (aml_data_t *)argument->object) ||
        string->kind != AML_DATA_STRING)
        return false;
    supported = string->length == sizeof(linux_name) - 1U &&
                memcmp(string->value.bytes, linux_name,
                       sizeof(linux_name) - 1U) == 0;
    aml_value_release(result);
    *result = aml_integer_value(supported ? aml_integer_mask : 0U);
    return true;
}

static void aml_execution_release(aml_execution_t *execution)
{
    if (!execution)
        return;
    for (u8 i = 0; i < 8U; i++)
        aml_value_release(&execution->local[i]);
    for (u8 i = 0; i < 7U; i++)
        aml_value_release(&execution->argument[i]);
    for (u8 i = 0; i < execution->dynamic_count; i++)
        aml_value_release(&execution->dynamic[i].value);
    aml_value_release(&execution->return_value);
}

static bool aml_invoke_method(aml_execution_t *caller, aml_handle_t method,
                              const aml_value_t *arguments,
                              u8 argument_count, aml_value_t *result)
{
    aml_method_object_t *object;
    aml_execution_t *execution;
    bool acquired = false;
    bool success = false;

    method = aml_alias_target(method);
    if (!caller || !caller->budget || !result || method == AML_HANDLE_INVALID ||
        method >= aml_node_count ||
        (aml_nodes[method].kind != AML_NODE_METHOD &&
         aml_nodes[method].kind != AML_NODE_NATIVE_METHOD))
        return false;
    object = &aml_nodes[method].object.method;
    if (argument_count != object->argument_count)
        return false;
    if (caller->budget->depth >= AML_MAX_METHOD_DEPTH) {
        aml_note_failure(AML_FAILURE_RECURSION, 0, caller->scope, NULL,
                         aml_nodes[method].table_index,
                         aml_nodes[method].table_offset);
        return false;
    }
    execution = (aml_execution_t *)kmalloc(sizeof(*execution));
    if (!execution)
        return false;
    memset(execution, 0, sizeof(*execution));
    if (object->serialized && object->owner != caller->budget->invocation_id) {
        if (!aml_owned_lock(&object->owner, caller->budget->invocation_id,
                            AML_METHOD_TIMEOUT_US)) {
            aml_note_failure(AML_FAILURE_TIMEOUT, 0, caller->scope, NULL,
                             aml_nodes[method].table_index,
                             aml_nodes[method].table_offset);
            kfree(execution);
            return false;
        }
        acquired = true;
    }
    execution->caller = caller;
    execution->method = method;
    /* AML method execution scope is the method object itself.  This matters
     * for explicit ParentPrefixChar paths: one '^' leaves the method and
     * reaches its containing Device/Scope. */
    execution->scope = method;
    execution->budget = caller->budget;
    execution->budget->depth++;
    for (u8 i = 0; i < argument_count; i++) {
        if (!aml_value_copy(&execution->argument[i], &arguments[i]))
            goto finished;
    }
    if (aml_nodes[method].kind == AML_NODE_NATIVE_METHOD) {
        success = aml_native_osi(&execution->argument[0], result);
        goto finished;
    }
    if (!aml_execute_block(execution, object->body, object->body_length))
        goto finished;
    if (execution->returned) {
        if (!aml_value_resolve(execution, &execution->return_value, result))
            goto finished;
    } else {
        aml_value_release(result);
        *result = aml_integer_value(0);
    }
    success = true;

finished:
    execution->budget->depth--;
    aml_execution_release(execution);
    kfree(execution);
    if (acquired)
        aml_owned_unlock(&object->owner, caller->budget->invocation_id);
    return success;
}

static bool aml_execute_block(aml_execution_t *execution, const u8 *aml,
                              u32 available)
{
    u32 cursor = 0;

    if (!execution || (!aml && available))
        return false;
    while (cursor < available) {
        aml_value_t value = {0};
        u32 used;

        if (!aml_budget_step(execution, aml[cursor]))
            return false;
        if (aml[cursor] == AML_RETURN_OP) {
            if (cursor + 1U < available) {
                if (!aml_eval_term(execution, aml + cursor + 1U,
                                   available - cursor - 1U,
                                   &execution->return_value, &used))
                    return false;
            } else {
                aml_value_release(&execution->return_value);
                execution->return_value = aml_integer_value(0);
            }
            execution->returned = true;
            return true;
        }
        if (aml[cursor] == AML_BREAK_OP) {
            execution->break_requested = true;
            return true;
        }
        if (aml[cursor] == AML_CONTINUE_OP) {
            execution->continue_requested = true;
            return true;
        }
        if (aml[cursor] == AML_IF_OP) {
            aml_value_t predicate = {0};
            u64 condition;
            u32 encoded;
            u32 package;
            u32 predicate_used;
            const u8 *body;
            u32 body_length;

            if (!aml_package_span(aml + cursor + 1U,
                                  available - cursor - 1U,
                                  &encoded, &package))
                return false;
            body = aml + cursor + 1U + encoded;
            body_length = package - encoded;
            if (!aml_eval_term(execution, body, body_length, &predicate,
                               &predicate_used) ||
                !aml_execution_integer(execution, &predicate, &condition) ||
                predicate_used > body_length) {
                aml_value_release(&predicate);
                return false;
            }
            aml_value_release(&predicate);
            if (condition &&
                !aml_execute_block(execution, body + predicate_used,
                                   body_length - predicate_used))
                return false;
            cursor += 1U + package;
            if (cursor < available && aml[cursor] == AML_ELSE_OP) {
                u32 else_encoded;
                u32 else_package;
                if (!aml_package_span(aml + cursor + 1U,
                                      available - cursor - 1U,
                                      &else_encoded, &else_package))
                    return false;
                if (!condition &&
                    !aml_execute_block(execution,
                                       aml + cursor + 1U + else_encoded,
                                       else_package - else_encoded))
                    return false;
                cursor += 1U + else_package;
            }
            if (execution->returned || execution->break_requested ||
                execution->continue_requested)
                return true;
            continue;
        }
        if (aml[cursor] == AML_WHILE_OP) {
            u32 encoded;
            u32 package;
            const u8 *body;
            u32 body_length;

            if (!aml_package_span(aml + cursor + 1U,
                                  available - cursor - 1U,
                                  &encoded, &package))
                return false;
            body = aml + cursor + 1U + encoded;
            body_length = package - encoded;
            for (;;) {
                aml_value_t predicate = {0};
                u64 condition;
                u32 predicate_used;

                if (++execution->budget->loop_iterations >
                        AML_MAX_LOOP_ITERATIONS ||
                    !aml_eval_term(execution, body, body_length,
                                   &predicate, &predicate_used) ||
                    !aml_execution_integer(execution, &predicate,
                                           &condition) ||
                    predicate_used > body_length) {
                    aml_value_release(&predicate);
                    aml_note_failure(AML_FAILURE_BUDGET, AML_WHILE_OP,
                                     execution->scope, NULL, 0, 0);
                    return false;
                }
                aml_value_release(&predicate);
                if (!condition)
                    break;
                if (!aml_execute_block(execution, body + predicate_used,
                                       body_length - predicate_used))
                    return false;
                if (execution->returned)
                    return true;
                if (execution->break_requested) {
                    execution->break_requested = false;
                    break;
                }
                execution->continue_requested = false;
            }
            cursor += 1U + package;
            continue;
        }
        if (aml[cursor] == AML_ELSE_OP)
            return false;
        if (!aml_eval_term(execution, aml + cursor, available - cursor,
                           &value, &used) || !used) {
            aml_value_release(&value);
            return false;
        }
        aml_value_release(&value);
        cursor += used;
    }
    return cursor == available;
}

static aml_handle_t aml_ensure_scope_path(aml_handle_t scope,
                                          const aml_name_path_t *path,
                                          u8 table_index,
                                          u32 table_offset)
{
    aml_handle_t current;

    if (!path)
        return AML_HANDLE_INVALID;
    if (path->null_name)
        return path->absolute && !path->parent_prefixes ? AML_ROOT_NODE :
                                                          AML_HANDLE_INVALID;
    current = path->absolute ? AML_ROOT_NODE :
              aml_ascend(scope, path->parent_prefixes);
    if (current == AML_HANDLE_INVALID)
        return current;
    for (u8 i = 0; i < path->count; i++) {
        aml_handle_t child = aml_find_child(current, path->segment[i]);
        if (child == AML_HANDLE_INVALID) {
            child = aml_create_node(current, path->segment[i],
                                    AML_NODE_SCOPE, table_index,
                                    table_offset);
            if (child == AML_HANDLE_INVALID)
                return child;
        }
        current = child;
    }
    return current;
}

static aml_handle_t aml_predefined_node(const char name[4],
                                        aml_node_kind_t kind)
{
    return aml_create_node(AML_ROOT_NODE, aml_name_pack((const u8 *)name),
                           kind, 0, 0);
}

static bool aml_predefined_integer(const char name[4], u64 integer)
{
    aml_handle_t handle = aml_predefined_node(name, AML_NODE_NAME);
    if (handle == AML_HANDLE_INVALID)
        return false;
    aml_nodes[handle].object.name.value = aml_integer_value(integer);
    aml_nodes[handle].object.name.initialized = true;
    return true;
}

static bool aml_predefined_string(const char name[4], const char *string)
{
    aml_handle_t handle = aml_predefined_node(name, AML_NODE_NAME);
    if (handle == AML_HANDLE_INVALID ||
        !aml_string_value(&aml_nodes[handle].object.name.value,
                          (const u8 *)string, (u32)strlen(string)))
        return false;
    aml_nodes[handle].object.name.initialized = true;
    return true;
}

bool aml_namespace_init(void)
{
    static const char dsdt_signature[4] = {'D', 'S', 'D', 'T'};
    static const char ssdt_signature[4] = {'S', 'S', 'D', 'T'};
    static const char predefined_scopes[][4] = {
        {'_', 'S', 'B', '_'}, {'_', 'G', 'P', 'E'},
        {'_', 'P', 'R', '_'}, {'_', 'S', 'I', '_'},
        {'_', 'T', 'Z', '_'},
    };
    const acpi_sdt_header_t *dsdt;

    if (aml_ready)
        return true;
    if (aml_building || !acpi_present())
        return false;
    aml_building = true;
    aml_node_count = 1U;
    aml_nodes[AML_ROOT_NODE] = (aml_node_t){
        .parent = AML_HANDLE_INVALID,
        .first_child = AML_HANDLE_INVALID,
        .next_sibling = AML_HANDLE_INVALID,
        .kind = AML_NODE_ROOT,
    };
    aml_table_count = 0;
    aml_ec_connected = false;
    aml_notify_handler = NULL;
    aml_evaluator_owner = 0;
    aml_failure_clear();
    dsdt = acpi_dsdt();
    if (!dsdt || !aml_signature_is(dsdt, dsdt_signature) ||
        !aml_table_checksum_valid(dsdt))
        goto failed;
    aml_integer_mask = dsdt->revision < 2U ? 0xffffffffULL : ~(u64)0;
    for (u32 i = 0; i < sizeof(predefined_scopes) /
                            sizeof(predefined_scopes[0]); i++) {
        if (aml_predefined_node(predefined_scopes[i], AML_NODE_SCOPE) ==
            AML_HANDLE_INVALID)
            goto failed;
    }
    if (!aml_predefined_integer("_REV", 2U) ||
        !aml_predefined_string("_OS_", "Mangrove") ||
        aml_predefined_node("_GL_", AML_NODE_MUTEX) == AML_HANDLE_INVALID)
        goto failed;
    aml_handle_t osi = aml_predefined_node("_OSI", AML_NODE_NATIVE_METHOD);
    if (osi == AML_HANDLE_INVALID)
        goto failed;
    aml_nodes[osi].object.method.argument_count = 1U;
    aml_tables[aml_table_count++] = dsdt;
    if (!aml_load_termlist(AML_ROOT_NODE,
                           (const u8 *)dsdt + sizeof(*dsdt),
                           dsdt->length - sizeof(*dsdt), 0,
                           (const u8 *)dsdt)) {
        if (aml_failure.kind == AML_FAILURE_NONE)
            aml_note_failure(AML_FAILURE_MALFORMED_AML, 0,
                             AML_ROOT_NODE, NULL, 0, 0);
        goto failed;
    }
    for (u32 i = 0; i < acpi_definition_table_count(); i++) {
        const acpi_sdt_header_t *table = acpi_definition_table(i);
        if (!table || !aml_signature_is(table, ssdt_signature) ||
            !aml_table_checksum_valid(table))
            continue;
        if (aml_table_count >= sizeof(aml_tables) / sizeof(aml_tables[0]))
            goto failed;
        u8 table_index = aml_table_count;
        aml_tables[aml_table_count++] = table;
        if (!aml_load_termlist(AML_ROOT_NODE,
                               (const u8 *)table + sizeof(*table),
                               table->length - sizeof(*table), table_index,
                               (const u8 *)table)) {
            if (aml_failure.kind == AML_FAILURE_NONE)
                aml_note_failure(AML_FAILURE_MALFORMED_AML, 0,
                                 AML_ROOT_NODE, NULL, table_index, 0);
            goto failed;
        }
    }
    (void)aml_global_lock_prepare();
    aml_ready = true;
    aml_building = false;
#ifdef ACPI_POWER_DEBUG
    kprint("[ACPI] AML namespace ready tables=%u objects=%u integer=%u\n",
           aml_table_count, aml_node_count,
           aml_integer_mask == 0xffffffffULL ? 32U : 64U);
#endif
    return true;

failed:
    aml_ready = false;
    aml_building = false;
    aml_debug_last_failure("namespace");
    return false;
}

bool aml_namespace_ready(void)
{
    return aml_ready;
}

void aml_set_notify_handler(aml_notify_handler_t handler)
{
    aml_notify_handler = handler;
}

static bool aml_decode_eisa_id(u64 value, char output[8])
{
    u8 byte0 = (u8)value;
    u8 byte1 = (u8)(value >> 8);
    static const char hex[] = "0123456789ABCDEF";

    output[0] = (char)(0x40U + (byte0 >> 2));
    output[1] = (char)(0x40U + ((byte0 & 0x03U) << 3) + (byte1 >> 5));
    output[2] = (char)(0x40U + (byte1 & 0x1fU));
    output[3] = hex[(value >> 20) & 0x0fU];
    output[4] = hex[(value >> 16) & 0x0fU];
    output[5] = hex[(value >> 28) & 0x0fU];
    output[6] = hex[(value >> 24) & 0x0fU];
    output[7] = '\0';
    return output[0] >= 'A' && output[0] <= 'Z' &&
           output[1] >= 'A' && output[1] <= 'Z' &&
           output[2] >= 'A' && output[2] <= 'Z';
}

static bool aml_device_hid(aml_handle_t device, char hid[9])
{
    static const u32 hid_name =
        (u32)'_' | ((u32)'H' << 8) | ((u32)'I' << 16) | ((u32)'D' << 24);
    aml_handle_t child;
    aml_value_t value = {0};
    aml_data_t *string;
    u64 integer;
    bool result = false;

    child = aml_find_child(device, hid_name);
    if (child == AML_HANDLE_INVALID ||
        !aml_evaluate_handle(child, NULL, 0, &value))
        return false;
    if (aml_value_integer(&value, &integer)) {
        result = aml_decode_eisa_id(integer, hid);
    } else if (value.kind == AML_PUBLIC_VALUE_STRING &&
               (string = (aml_data_t *)value.object) &&
               string->kind == AML_DATA_STRING && string->length > 0U &&
               string->length < 9U) {
        memcpy(hid, string->value.bytes, string->length);
        hid[string->length] = '\0';
        result = true;
    }
    aml_value_release(&value);
    return result;
}

bool aml_find_device_by_hid(const char *hid, aml_handle_t *cursor)
{
    u32 start;

    if (!hid || !cursor || (!aml_ready && !aml_namespace_init()))
        return false;
    start = *cursor == AML_HANDLE_INVALID ? 1U : (u32)*cursor + 1U;
    for (u32 i = start; i < aml_node_count; i++) {
        char candidate[9];
        if (aml_nodes[i].kind == AML_NODE_DEVICE &&
            aml_device_hid((aml_handle_t)i, candidate) &&
            strcmp(candidate, hid) == 0) {
            *cursor = (aml_handle_t)i;
            return true;
        }
    }
    return false;
}

bool aml_find_thermal_zone(aml_handle_t *cursor)
{
    u32 start;

    if (!cursor || (!aml_ready && !aml_namespace_init()))
        return false;
    start = *cursor == AML_HANDLE_INVALID ? 1U : (u32)*cursor + 1U;
    for (u32 i = start; i < aml_node_count; i++) {
        if (aml_nodes[i].kind == AML_NODE_THERMAL_ZONE) {
            *cursor = (aml_handle_t)i;
            return true;
        }
    }
    return false;
}

bool aml_find_device_with_method(const char name[4], aml_handle_t *cursor)
{
    u32 start;
    u32 packed;

    if (!name || !cursor || (!aml_ready && !aml_namespace_init()))
        return false;
    packed = aml_name_pack((const u8 *)name);
    start = *cursor == AML_HANDLE_INVALID ? 1U : (u32)*cursor + 1U;
    for (u32 i = start; i < aml_node_count; i++) {
        aml_handle_t child;

        if (aml_nodes[i].kind != AML_NODE_DEVICE)
            continue;
        child = aml_find_child((aml_handle_t)i, packed);
        if (child == AML_HANDLE_INVALID ||
            (aml_nodes[child].kind != AML_NODE_METHOD &&
             aml_nodes[child].kind != AML_NODE_NATIVE_METHOD))
            continue;
        *cursor = (aml_handle_t)i;
        return true;
    }
    return false;
}

bool aml_child_exists(aml_handle_t parent, const char name[4])
{
    if (!name || parent >= aml_node_count ||
        (!aml_ready && !aml_namespace_init()))
        return false;
    return aml_find_child(parent, aml_name_pack((const u8 *)name)) !=
           AML_HANDLE_INVALID;
}

bool aml_evaluate_child(aml_handle_t parent, const char name[4],
                        aml_value_t *result)
{
    aml_handle_t child;

    if (!name || !result || parent >= aml_node_count ||
        (!aml_ready && !aml_namespace_init()))
        return false;
    child = aml_find_child(parent, aml_name_pack((const u8 *)name));
    if (child == AML_HANDLE_INVALID) {
        aml_name_path_t path = {.count = 1U};
        path.segment[0] = aml_name_pack((const u8 *)name);
        aml_failure_clear();
        aml_note_failure(AML_FAILURE_UNRESOLVED_NAME, 0, parent, &path,
                         0, 0);
        return false;
    }
    return aml_evaluate_handle(child, NULL, 0, result);
}

bool aml_evaluate_child_args(aml_handle_t parent, const char name[4],
                             const aml_value_t *arguments,
                             u8 argument_count, aml_value_t *result)
{
    aml_handle_t child;

    if (!name || !result || parent >= aml_node_count ||
        (!aml_ready && !aml_namespace_init()))
        return false;
    child = aml_find_child(parent, aml_name_pack((const u8 *)name));
    if (child == AML_HANDLE_INVALID) {
        aml_name_path_t path = {.count = 1U};
        path.segment[0] = aml_name_pack((const u8 *)name);
        aml_failure_clear();
        aml_note_failure(AML_FAILURE_UNRESOLVED_NAME, 0, parent, &path,
                         0, 0);
        return false;
    }
    return aml_evaluate_handle(child, arguments, argument_count, result);
}

bool aml_dispatch_gpe(u8 gpe)
{
    static const char hex[] = "0123456789ABCDEF";
    aml_handle_t gpe_scope;
    aml_handle_t method;
    aml_value_t result = {0};
    char method_name[4];
    u32 packed;
    bool handled = false;

    if (!aml_ready && !aml_namespace_init())
        return false;
    gpe_scope = aml_find_child(AML_ROOT_NODE,
                               aml_name_pack((const u8 *)"_GPE"));
    if (gpe_scope == AML_HANDLE_INVALID)
        return false;

    /* Level-triggered GPE handlers are preferred.  If firmware describes an
     * edge-triggered handler instead, use the corresponding _Exx method. */
    method_name[0] = '_';
    method_name[1] = 'L';
    method_name[2] = hex[(gpe >> 4) & 0x0fU];
    method_name[3] = hex[gpe & 0x0fU];
    packed = aml_name_pack((const u8 *)method_name);
    method = aml_find_child(gpe_scope, packed);
    if (method == AML_HANDLE_INVALID) {
        method_name[1] = 'E';
        packed = aml_name_pack((const u8 *)method_name);
        method = aml_find_child(gpe_scope, packed);
    }
    if (method != AML_HANDLE_INVALID &&
        (aml_nodes[method].kind == AML_NODE_METHOD ||
         aml_nodes[method].kind == AML_NODE_NATIVE_METHOD)) {
        handled = aml_evaluate_handle(method, NULL, 0, &result);
    }
#ifdef ACPI_POWER_DEBUG
    if (method != AML_HANDLE_INVALID) {
        char path[AML_MAX_PATH] = "\\";
        (void)aml_handle_path(method, path, sizeof(path));
        kprint("[ACPI] GPE %u handler=%s result=%u\n", gpe, path, handled);
        if (!handled)
            aml_debug_last_failure("GPE handler");
    }
#endif
    aml_value_release(&result);
    return handled;
}

static bool aml_resource_ec_ports(const aml_value_t *value,
                                  acpi_ec_info_t *info)
{
    aml_data_t *buffer;
    acpi_generic_address_t ports[2] = {{0}};
    u32 port_count = 0;
    u32 offset = 0;
    bool end_tag = false;

    if (!value || !info || value->kind != AML_PUBLIC_VALUE_BUFFER ||
        !(buffer = (aml_data_t *)value->object) ||
        buffer->kind != AML_DATA_BUFFER)
        return false;
    while (offset < buffer->length) {
        u8 tag = buffer->value.bytes[offset];
        u32 descriptor_length;

        if (tag == 0x79U) {
            if (buffer->length - offset < 2U)
                return false;
            end_tag = true;
            break;
        }
        if (tag & 0x80U) {
            if (buffer->length - offset < 3U)
                return false;
            descriptor_length = (u32)buffer->value.bytes[offset + 1U] |
                                ((u32)buffer->value.bytes[offset + 2U] << 8);
            if (descriptor_length > buffer->length - offset - 3U)
                return false;
            offset += 3U + descriptor_length;
            continue;
        }
        descriptor_length = tag & 0x07U;
        if (descriptor_length > buffer->length - offset - 1U)
            return false;
        if ((tag >> 3) == 0x08U && descriptor_length == 7U &&
            port_count < 2U) {
            u16 minimum = (u16)((u16)buffer->value.bytes[offset + 2U] |
                                ((u16)buffer->value.bytes[offset + 3U]
                                 << 8));
            u16 maximum = (u16)((u16)buffer->value.bytes[offset + 4U] |
                                ((u16)buffer->value.bytes[offset + 5U]
                                 << 8));
            u8 length = buffer->value.bytes[offset + 7U];
            if (minimum != maximum || length != 1U)
                return false;
            ports[port_count] = (acpi_generic_address_t){
                .address_space_id = ACPI_ADDRESS_SPACE_SYSTEM_IO,
                .register_bit_width = 8U,
                .address = minimum,
            };
            port_count++;
        }
        offset += 1U + descriptor_length;
    }
    if (!end_tag || port_count != 2U ||
        ports[0].address == ports[1].address)
        return false;
    info->data = ports[0];
    info->control = ports[1];
    return true;
}

bool aml_discover_ec(acpi_ec_info_t *info, aml_handle_t *device)
{
    aml_handle_t cursor = AML_HANDLE_INVALID;

    if (!info || !device || (!aml_ready && !aml_namespace_init()))
        return false;
    while (aml_find_device_by_hid("PNP0C09", &cursor)) {
        aml_value_t resources = {0};
        aml_value_t gpe = {0};
        aml_value_t uid = {0};
        u64 integer;

        *info = (acpi_ec_info_t){0};
        if (!aml_evaluate_child(cursor, "_CRS", &resources) ||
            !aml_resource_ec_ports(&resources, info)) {
#ifdef ACPI_POWER_DEBUG
            char path[AML_MAX_PATH];
            aml_handle_path(cursor, path, sizeof(path));
            kprint("[ACPI] EC %s _CRS failed\n", path);
            aml_debug_last_failure("EC _CRS");
#endif
            aml_value_release(&resources);
            continue;
        }
        aml_value_release(&resources);
        if (aml_evaluate_child(cursor, "_GPE", &gpe) &&
            aml_value_integer(&gpe, &integer) && integer <= 0xffU) {
            info->gpe = (u8)integer;
            info->gpe_valid = true;
        }
        aml_value_release(&gpe);
        if (aml_evaluate_child(cursor, "_UID", &uid) &&
            aml_value_integer(&uid, &integer) && integer <= 0xffffffffU)
            info->uid = (u32)integer;
        aml_value_release(&uid);
        info->discovery = ACPI_EC_DISCOVERY_NAMESPACE;
        if (!aml_handle_path(cursor, info->path, sizeof(info->path)))
            info->path[0] = '\0';
        *device = cursor;
        return true;
    }
    return false;
}

static bool aml_node_descends_from(aml_handle_t node, aml_handle_t ancestor)
{
    while (node != AML_HANDLE_INVALID && node < aml_node_count) {
        if (node == ancestor)
            return true;
        node = aml_nodes[node].parent;
    }
    return false;
}

static bool aml_run_ini(aml_handle_t node)
{
    static const u32 ini_name =
        (u32)'_' | ((u32)'I' << 8) | ((u32)'N' << 16) | ((u32)'I' << 24);
    aml_handle_t method;
    aml_value_t result = {0};

    if (node >= aml_node_count || aml_nodes[node].initialized)
        return true;
    method = aml_find_child(node, ini_name);
    if (method != AML_HANDLE_INVALID &&
        !aml_evaluate_handle(method, NULL, 0, &result)) {
        aml_value_release(&result);
        return false;
    }
    aml_value_release(&result);
    aml_nodes[node].initialized = true;
    return true;
}

bool aml_connect_ec_regions(aml_handle_t ec_device)
{
    static const u32 reg_name =
        (u32)'_' | ((u32)'R' << 8) | ((u32)'E' << 16) | ((u32)'G' << 24);
    aml_handle_t ancestry[AML_MAX_NAME_SEGMENTS];
    u32 ancestry_count = 0;
    aml_handle_t current;
    aml_handle_t reg;
    aml_value_t arguments[2] = {
        {.kind = AML_PUBLIC_VALUE_INTEGER, .integer = 3U},
        {.kind = AML_PUBLIC_VALUE_INTEGER, .integer = 1U},
    };
    aml_value_t result = {0};

    if (!aml_ready || ec_device == AML_HANDLE_INVALID ||
        ec_device >= aml_node_count || !ec_available())
        return false;
    current = ec_device;
    while (current != AML_ROOT_NODE &&
           ancestry_count < AML_MAX_NAME_SEGMENTS) {
        ancestry[ancestry_count++] = current;
        current = aml_nodes[current].parent;
    }
    for (u32 i = ancestry_count; i > 0; i--) {
        if (!aml_run_ini(ancestry[i - 1U])) {
            aml_debug_last_failure("EC ancestor _INI");
            return false;
        }
    }
    for (u32 i = 1U; i < aml_node_count; i++) {
        if (aml_nodes[i].kind == AML_NODE_REGION &&
            aml_nodes[i].object.region.space == AML_REGION_EMBEDDED_CTRL &&
            aml_node_descends_from((aml_handle_t)i, ec_device))
            aml_nodes[i].object.region.connected = true;
    }
    aml_ec_connected = true;
    reg = aml_find_child(ec_device, reg_name);
    if (reg != AML_HANDLE_INVALID &&
        !aml_evaluate_handle(reg, arguments, 2U, &result)) {
        aml_ec_connected = false;
        for (u32 i = 1U; i < aml_node_count; i++) {
            if (aml_nodes[i].kind == AML_NODE_REGION &&
                aml_nodes[i].object.region.space ==
                    AML_REGION_EMBEDDED_CTRL &&
                aml_node_descends_from((aml_handle_t)i, ec_device))
                aml_nodes[i].object.region.connected = false;
        }
        aml_value_release(&result);
        aml_debug_last_failure("EC _REG");
        return false;
    }
    aml_value_release(&result);
    return true;
}

void aml_debug_last_failure(const char *operation)
{
#ifdef ACPI_POWER_DEBUG
    static const char *const names[] = {
        "none", "namespace-full", "malformed-AML", "unresolved-name",
        "unsupported-opcode", "unsupported-region", "object-type",
        "hardware-access", "timeout", "operation-budget", "recursion",
        "divide-by-zero",
    };
    char scope[AML_MAX_PATH] = "\\";
    char unresolved[AML_MAX_PATH] = "";
    const char *kind = aml_failure.kind < sizeof(names) / sizeof(names[0]) ?
                       names[aml_failure.kind] : "unknown";
    if (aml_failure.scope != AML_HANDLE_INVALID)
        (void)aml_handle_path(aml_failure.scope, scope, sizeof(scope));
    if (aml_failure.has_name) {
        usize offset = 0;
        if (aml_failure.name.absolute)
            unresolved[offset++] = '\\';
        for (u8 i = 0; i < aml_failure.name.parent_prefixes &&
                       offset + 1U < sizeof(unresolved); i++)
            unresolved[offset++] = '^';
        for (u8 i = 0; i < aml_failure.name.count &&
                       offset + 5U < sizeof(unresolved); i++) {
            char segment[5];
            if (i && offset && unresolved[offset - 1U] != '^' &&
                unresolved[offset - 1U] != '\\')
                unresolved[offset++] = '.';
            aml_name_unpack(aml_failure.name.segment[i], segment);
            memcpy(unresolved + offset, segment, 4U);
            offset += 4U;
        }
        unresolved[offset] = '\0';
    }
    if (aml_failure.table_index < aml_table_count) {
        const acpi_sdt_header_t *table = aml_tables[aml_failure.table_index];
        kprint("[ACPI] AML %s failed: %s op=0x%02x scope=%s name=%s "
               "table=%c%c%c%c+0x%x\n",
               operation ? operation : "evaluation", kind,
               aml_failure.opcode, scope,
               unresolved[0] ? unresolved : "-",
               table->signature[0], table->signature[1],
               table->signature[2], table->signature[3],
               aml_failure.table_offset);
    } else {
        kprint("[ACPI] AML %s failed: %s op=0x%02x scope=%s name=%s\n",
               operation ? operation : "evaluation", kind,
               aml_failure.opcode, scope,
               unresolved[0] ? unresolved : "-");
    }
#else
    (void)operation;
#endif
}

static bool aml_evaluate_handle(aml_handle_t handle,
                                const aml_value_t *arguments,
                                u8 argument_count, aml_value_t *result)
{
    aml_budget_t budget = {0};
    aml_execution_t *root;
    aml_handle_t target;
    bool success;

    if (!result || handle == AML_HANDLE_INVALID || handle >= aml_node_count)
        return false;
    aml_failure_clear();
    budget.invocation_id = aml_next_invocation_id++;
    if (!budget.invocation_id)
        budget.invocation_id = aml_next_invocation_id++;
    budget.deadline_valid = timer_monotonic_deadline_start(
        &budget.deadline, AML_METHOD_TIMEOUT_US);
    root = (aml_execution_t *)kmalloc(sizeof(*root));
    if (!root)
        return false;
    memset(root, 0, sizeof(*root));
    root->scope = AML_ROOT_NODE;
    root->budget = &budget;
    if (!aml_owned_lock(&aml_evaluator_owner, budget.invocation_id,
                        AML_METHOD_TIMEOUT_US)) {
        kfree(root);
        return false;
    }
    target = aml_alias_target(handle);
    if (target == AML_HANDLE_INVALID) {
        success = false;
    } else if (aml_nodes[target].kind == AML_NODE_METHOD ||
               aml_nodes[target].kind == AML_NODE_NATIVE_METHOD) {
        success = aml_invoke_method(root, target, arguments,
                                    argument_count, result);
    } else {
        success = argument_count == 0U &&
                  aml_read_node(root, target, result);
    }
    aml_execution_release(root);
    kfree(root);
    /* An aborted control method must not strand an AML mutex.  ACPICA's
     * thread state performs the same unwind; Mangrove has one invocation ID
     * for the complete nested call tree. */
    for (u32 i = 1U; i < aml_node_count; i++) {
        if (aml_nodes[i].kind == AML_NODE_MUTEX &&
            __atomic_load_n(&aml_nodes[i].object.mutex.owner,
                            __ATOMIC_ACQUIRE) == budget.invocation_id) {
            aml_nodes[i].object.mutex.recursion = 0;
            aml_owned_unlock(&aml_nodes[i].object.mutex.owner,
                             budget.invocation_id);
        }
    }
    aml_owned_unlock(&aml_evaluator_owner, budget.invocation_id);
    return success;
}

static bool aml_dynamic_field_list(aml_execution_t *execution,
                                   aml_region_object_t *region,
                                   aml_handle_t static_region,
                                   const u8 *aml, u32 length, u8 flags)
{
    u32 offset = 0;
    u32 bit_offset = 0;
    u8 current_flags = flags;
    u8 access_attribute = 0;
    u8 access_length = 0;

    while (offset < length) {
        u32 encoded;
        u32 bits;
        aml_dynamic_object_t *field;

        if (aml[offset] == 0x00U) {
            if (!aml_package_length_raw(aml + offset + 1U,
                                        length - offset - 1U,
                                        &encoded, &bits) ||
                bits > 0xffffffffU - bit_offset)
                return false;
            bit_offset += bits;
            offset += 1U + encoded;
            continue;
        }
        if (aml[offset] == 0x01U) {
            if (length - offset < 3U)
                return false;
            current_flags = (current_flags & ~AML_FIELD_ACCESS_MASK) |
                            (aml[offset + 1U] & AML_FIELD_ACCESS_MASK);
            access_attribute = aml[offset + 2U];
            access_length = 0;
            offset += 3U;
            continue;
        }
        if (aml[offset] == 0x02U) {
            u32 connection;
            aml_name_path_t path;
            if (length - offset < 2U)
                return false;
            if (aml[offset + 1U] == AML_BUFFER_OP) {
                if (!aml_term_span(execution->scope, aml + offset + 1U,
                                   length - offset - 1U, &connection))
                    return false;
            } else if (!aml_parse_name(aml + offset + 1U,
                                       length - offset - 1U,
                                       &path, &connection)) {
                return false;
            }
            offset += 1U + connection;
            continue;
        }
        if (aml[offset] == 0x03U) {
            if (length - offset < 4U)
                return false;
            current_flags = (current_flags & ~AML_FIELD_ACCESS_MASK) |
                            (aml[offset + 1U] & AML_FIELD_ACCESS_MASK);
            access_attribute = aml[offset + 2U];
            access_length = aml[offset + 3U];
            offset += 4U;
            continue;
        }
        if (length - offset < 5U ||
            !aml_name_segment_valid(aml + offset) ||
            !aml_package_length_raw(aml + offset + 4U,
                                    length - offset - 4U,
                                    &encoded, &bits) || !bits ||
            bits > 0xffffffffU - bit_offset)
            return false;
        field = aml_dynamic_create(execution, aml_name_pack(aml + offset),
                                   AML_NODE_FIELD);
        if (!field)
            return false;
        field->kind = AML_NODE_FIELD;
        field->field = (aml_field_object_t){
            .region = static_region,
            .runtime_region = region,
            .index_register = AML_HANDLE_INVALID,
            .data_register = AML_HANDLE_INVALID,
            .bank_register = AML_HANDLE_INVALID,
            .bit_offset = bit_offset,
            .bit_length = bits,
            .flags = current_flags,
            .access_attribute = access_attribute,
            .access_length = access_length,
        };
        bit_offset += bits;
        offset += 4U + encoded;
    }
    return offset == length;
}

static bool aml_read_dynamic(aml_execution_t *execution,
                             aml_dynamic_object_t *object,
                             aml_value_t *value)
{
    aml_handle_t temporary;

    if (!execution || !object || !value)
        return false;
    if (object->kind == AML_NODE_NAME)
        return aml_value_copy(value, &object->value);
    if (object->kind != AML_NODE_FIELD)
        return false;
    /* Reuse the field access engine without publishing a namespace node. */
    if (aml_node_count >= AML_MAX_NODES)
        return false;
    temporary = (aml_handle_t)aml_node_count;
    aml_nodes[temporary] = (aml_node_t){
        .parent = execution->scope,
        .kind = AML_NODE_FIELD,
        .object.field = object->field,
    };
    aml_node_count++;
    bool result = aml_read_field(execution, temporary, value);
    aml_node_count--;
    return result;
}

static bool aml_write_dynamic(aml_execution_t *execution,
                              aml_dynamic_object_t *object,
                              const aml_value_t *value)
{
    aml_handle_t temporary;

    if (!execution || !object || !value)
        return false;
    if (object->kind == AML_NODE_NAME)
        return aml_value_copy(&object->value, value);
    if (object->kind != AML_NODE_FIELD || aml_node_count >= AML_MAX_NODES)
        return false;
    temporary = (aml_handle_t)aml_node_count;
    aml_nodes[temporary] = (aml_node_t){
        .parent = execution->scope,
        .kind = AML_NODE_FIELD,
        .object.field = object->field,
    };
    aml_node_count++;
    bool result = aml_write_field(execution, temporary, value);
    aml_node_count--;
    return result;
}

static bool aml_create_buffer_field(aml_execution_t *execution,
                                    const u8 *aml, u32 available,
                                    u8 opcode, aml_value_t *value, u32 *used)
{
    aml_value_t source = {0};
    aml_value_t resolved = {0};
    aml_value_t index_value = {0};
    aml_value_t length_value = {0};
    aml_data_t *data;
    aml_name_path_t path;
    aml_dynamic_object_t *object;
    aml_reference_t reference = {0};
    u64 index;
    u64 length;
    u32 cursor;
    u32 term_used;
    u32 name_used;
    u32 name;

    cursor = opcode == AML_EXT_CREATE_FIELD_OP ? 2U : 1U;
    if (!aml_eval_term(execution, aml + cursor, available - cursor,
                       &source, &term_used) ||
        !aml_value_resolve(execution, &source, &resolved))
        goto failed;
    cursor += term_used;
    if (!aml_eval_term(execution, aml + cursor, available - cursor,
                       &index_value, &term_used) ||
        !aml_execution_integer(execution, &index_value, &index))
        goto failed;
    cursor += term_used;
    if (opcode == AML_EXT_CREATE_FIELD_OP) {
        if (!aml_eval_term(execution, aml + cursor, available - cursor,
                           &length_value, &term_used) ||
            !aml_execution_integer(execution, &length_value, &length))
            goto failed;
        cursor += term_used;
    } else {
        length = opcode == AML_CREATE_BIT_FIELD ? 1U :
                 opcode == AML_CREATE_BYTE_FIELD ? 8U :
                 opcode == AML_CREATE_WORD_FIELD ? 16U :
                 opcode == AML_CREATE_DWORD_FIELD ? 32U : 64U;
        if (opcode != AML_CREATE_BIT_FIELD)
            index *= 8U;
    }
    if (!aml_parse_name(aml + cursor, available - cursor, &path,
                        &name_used) || !aml_dynamic_name(&path, &name) ||
        index > 0xffffffffU || !length || length > 64U ||
        index + length > 0xffffffffU)
        goto failed;
    data = (aml_data_t *)resolved.object;
    if (!data || (data->kind != AML_DATA_BUFFER &&
                  data->kind != AML_DATA_STRING) ||
        index + length > (u64)data->length * 8U)
        goto failed;
    object = aml_dynamic_create(execution, name, AML_NODE_NAME);
    if (!object)
        goto failed;
    reference.kind = AML_REF_BUFFER_FIELD;
    reference.owner = data;
    reference.bit_offset = (u32)index;
    reference.bit_length = (u32)length;
    if (!aml_reference_value(&object->value, &reference))
        goto failed;
    if (value) {
        aml_value_release(value);
        *value = aml_integer_value(0);
    }
    *used = cursor + name_used;
    aml_value_release(&source);
    aml_value_release(&resolved);
    aml_value_release(&index_value);
    aml_value_release(&length_value);
    return true;

failed:
    aml_value_release(&source);
    aml_value_release(&resolved);
    aml_value_release(&index_value);
    aml_value_release(&length_value);
    return false;
}

static bool aml_mutex_acquire(aml_execution_t *execution,
                              aml_handle_t handle, u16 timeout)
{
    aml_mutex_object_t *mutex;
    u64 timeout_us;

    handle = aml_alias_target(handle);
    if (!execution || handle == AML_HANDLE_INVALID ||
        aml_nodes[handle].kind != AML_NODE_MUTEX)
        return false;
    mutex = &aml_nodes[handle].object.mutex;
    if (__atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE) ==
        execution->budget->invocation_id) {
        if (mutex->recursion == 0xffffU)
            return false;
        mutex->recursion++;
        return true;
    }
    timeout_us = timeout == 0xffffU ?
        (u64)AML_MUTEX_TIMEOUT_CAP_MS * 1000U : (u64)timeout * 1000U;
    if (timeout_us > (u64)AML_MUTEX_TIMEOUT_CAP_MS * 1000U)
        timeout_us = (u64)AML_MUTEX_TIMEOUT_CAP_MS * 1000U;
    if (!timeout_us)
        timeout_us = 1U;
    if (!aml_owned_lock(&mutex->owner, execution->budget->invocation_id,
                        timeout_us))
        return false;
    mutex->recursion = 1U;
    return true;
}

static bool aml_mutex_release(aml_execution_t *execution,
                              aml_handle_t handle)
{
    aml_mutex_object_t *mutex;

    handle = aml_alias_target(handle);
    if (!execution || handle == AML_HANDLE_INVALID ||
        aml_nodes[handle].kind != AML_NODE_MUTEX)
        return false;
    mutex = &aml_nodes[handle].object.mutex;
    if (__atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE) !=
            execution->budget->invocation_id || !mutex->recursion)
        return false;
    if (--mutex->recursion == 0U)
        aml_owned_unlock(&mutex->owner, execution->budget->invocation_id);
    return true;
}

static bool aml_eval_name_reference(aml_execution_t *execution,
                                    const u8 *aml, u32 available,
                                    aml_value_t *value, u32 *used)
{
    aml_name_path_t path;
    aml_dynamic_object_t *dynamic;
    aml_handle_t handle;
    u32 name;
    u32 cursor;

    if (!aml_parse_name(aml, available, &path, &cursor))
        return false;
    if (aml_dynamic_name(&path, &name) &&
        (dynamic = aml_dynamic_find(execution, name))) {
        *used = cursor;
        return aml_read_dynamic(execution, dynamic, value);
    }
    handle = aml_resolve(execution->scope, &path);
    if (handle == AML_HANDLE_INVALID) {
        aml_note_failure(AML_FAILURE_UNRESOLVED_NAME, aml[0],
                         execution->scope, &path, 0, 0);
        return false;
    }
    handle = aml_alias_target(handle);
    if (handle == AML_HANDLE_INVALID)
        return false;
    if (aml_nodes[handle].kind == AML_NODE_METHOD ||
        aml_nodes[handle].kind == AML_NODE_NATIVE_METHOD) {
        aml_value_t arguments[7] = {{0}};
        u8 count = aml_nodes[handle].object.method.argument_count;
        bool result = false;

        for (u8 i = 0; i < count; i++) {
            u32 argument_used;
            if (cursor >= available ||
                !aml_eval_term(execution, aml + cursor,
                               available - cursor, &arguments[i],
                               &argument_used))
                goto invocation_finished;
            cursor += argument_used;
        }
        result = aml_invoke_method(execution, handle, arguments, count, value);
invocation_finished:
        for (u8 i = 0; i < count; i++)
            aml_value_release(&arguments[i]);
        if (!result)
            return false;
    } else if (!aml_read_node(execution, handle, value)) {
        return false;
    }
    *used = cursor;
    return true;
}

static bool aml_eval_divide(aml_execution_t *execution, const u8 *aml,
                            u32 available, aml_value_t *value, u32 *used)
{
    aml_value_t dividend_value = {0};
    aml_value_t divisor_value = {0};
    aml_reference_t remainder_target = {0};
    aml_reference_t quotient_target = {0};
    aml_value_t remainder_value = {0};
    aml_value_t quotient_value = {0};
    u64 dividend;
    u64 divisor = 0;
    u32 cursor = 1U;
    u32 term_used;
    bool success = false;

    if (!aml_eval_term(execution, aml + cursor, available - cursor,
                       &dividend_value, &term_used) ||
        !aml_execution_integer(execution, &dividend_value, &dividend))
        goto finished;
    cursor += term_used;
    if (!aml_eval_term(execution, aml + cursor, available - cursor,
                       &divisor_value, &term_used) ||
        !aml_execution_integer(execution, &divisor_value, &divisor) ||
        !divisor)
        goto finished;
    cursor += term_used;
    if (!aml_parse_target(execution, aml + cursor, available - cursor,
                          &remainder_target, &term_used))
        goto finished;
    cursor += term_used;
    if (!aml_parse_target(execution, aml + cursor, available - cursor,
                          &quotient_target, &term_used))
        goto finished;
    cursor += term_used;
    remainder_value = aml_integer_value(dividend % divisor);
    quotient_value = aml_integer_value(dividend / divisor);
    if (!aml_write_reference(execution, &remainder_target, &remainder_value) ||
        !aml_write_reference(execution, &quotient_target, &quotient_value))
        goto finished;
    aml_value_release(value);
    *value = quotient_value;
    quotient_value = (aml_value_t){0};
    *used = cursor;
    success = true;

finished:
    if (!divisor && aml_failure.kind == AML_FAILURE_NONE)
        aml_note_failure(AML_FAILURE_DIVIDE_BY_ZERO, AML_DIVIDE_OP,
                         execution->scope, NULL, 0, 0);
    aml_reference_release(&remainder_target);
    aml_reference_release(&quotient_target);
    aml_value_release(&dividend_value);
    aml_value_release(&divisor_value);
    aml_value_release(&remainder_value);
    aml_value_release(&quotient_value);
    return success;
}

static bool aml_integer_to_string(u64 integer, u32 base, aml_value_t *value)
{
    u8 reversed[32];
    u8 output[32];
    u32 count = 0;

    do {
        u32 digit = (u32)(integer % base);
        reversed[count++] = (u8)(digit < 10U ? '0' + digit :
                                 'A' + digit - 10U);
        integer /= base;
    } while (integer && count < sizeof(reversed));
    for (u32 i = 0; i < count; i++)
        output[i] = reversed[count - i - 1U];
    return aml_string_value(value, output, count);
}

static bool aml_eval_conversion(aml_execution_t *execution, const u8 *aml,
                                u32 available, aml_value_t *value, u32 *used)
{
    aml_value_t source = {0};
    aml_value_t resolved = {0};
    aml_value_t length_value = {0};
    aml_reference_t target = {0};
    aml_data_t *data;
    u64 integer;
    u64 length = aml_integer_mask;
    u32 cursor = 1U;
    u32 term_used;
    bool success = false;

    if (!aml_eval_term(execution, aml + cursor, available - cursor,
                       &source, &term_used) ||
        !aml_value_resolve(execution, &source, &resolved))
        goto finished;
    cursor += term_used;
    if (aml[0] == AML_TO_STRING_OP) {
        if (!aml_eval_term(execution, aml + cursor, available - cursor,
                           &length_value, &term_used) ||
            !aml_execution_integer(execution, &length_value, &length))
            goto finished;
        cursor += term_used;
    }
    if (!aml_parse_target(execution, aml + cursor, available - cursor,
                          &target, &term_used))
        goto finished;
    cursor += term_used;
    if (aml[0] == AML_TO_INTEGER_OP) {
        if (!aml_value_to_integer(&resolved, &integer))
            goto finished;
        aml_value_release(value);
        *value = aml_integer_value(integer);
    } else if (aml[0] == AML_TO_BUFFER_OP) {
        if (resolved.kind == AML_PUBLIC_VALUE_BUFFER) {
            if (!aml_value_copy(value, &resolved))
                goto finished;
        } else if (resolved.kind == AML_PUBLIC_VALUE_INTEGER) {
            u32 bytes = aml_integer_mask == 0xffffffffULL ? 4U : 8U;
            if (!aml_buffer_value(value, bytes))
                goto finished;
            data = (aml_data_t *)value->object;
            for (u32 i = 0; i < bytes; i++)
                data->value.bytes[i] = (u8)(resolved.integer >> (8U * i));
        } else if (resolved.kind == AML_PUBLIC_VALUE_STRING) {
            data = (aml_data_t *)resolved.object;
            if (!aml_buffer_value(value, data->length + 1U))
                goto finished;
            memcpy(((aml_data_t *)value->object)->value.bytes,
                   data->value.bytes, data->length);
        } else goto finished;
    } else if (aml[0] == AML_TO_STRING_OP) {
        if (resolved.kind == AML_PUBLIC_VALUE_STRING) {
            if (!aml_value_copy(value, &resolved))
                goto finished;
        } else if (resolved.kind == AML_PUBLIC_VALUE_BUFFER) {
            data = (aml_data_t *)resolved.object;
            u32 count = data->length;
            if (length < count)
                count = (u32)length;
            for (u32 i = 0; i < count; i++) {
                if (!data->value.bytes[i]) {
                    count = i;
                    break;
                }
            }
            if (!aml_string_value(value, data->value.bytes, count))
                goto finished;
        } else goto finished;
    } else if (aml[0] == AML_TO_DECIMAL_STRING_OP ||
               aml[0] == AML_TO_HEX_STRING_OP) {
        if (!aml_value_to_integer(&resolved, &integer) ||
            !aml_integer_to_string(integer,
                                   aml[0] == AML_TO_HEX_STRING_OP ? 16U : 10U,
                                   value))
            goto finished;
    } else goto finished;
    if (!aml_write_reference(execution, &target, value))
        goto finished;
    *used = cursor;
    success = true;

finished:
    aml_reference_release(&target);
    aml_value_release(&source);
    aml_value_release(&resolved);
    aml_value_release(&length_value);
    if (!success)
        aml_value_release(value);
    return success;
}

static bool aml_eval_term(aml_execution_t *execution, const u8 *aml,
                          u32 available, aml_value_t *value, u32 *used)
{
    aml_name_path_t path;
    aml_dynamic_object_t *dynamic;
    aml_value_t source = {0};
    aml_value_t resolved = {0};
    aml_value_t operand = {0};
    aml_reference_t target = {0};
    aml_reference_t source_reference = {0};
    aml_handle_t handle;
    u64 integer;
    u32 cursor;
    u32 term_used;
    u32 target_used;
    u32 name_used;
    u32 encoded;
    u32 package;
    u32 name;
    bool success = false;

    if (!execution || !aml || !available || !value || !used ||
        !aml_budget_step(execution, aml[0]))
        return false;
    *used = 0;
    if (aml_integer_literal(aml, available, &integer, &term_used)) {
        aml_value_release(value);
        *value = aml_integer_value(integer);
        *used = term_used;
        return true;
    }
    if (aml[0] >= AML_LOCAL0_OP && aml[0] <= AML_LOCAL7_OP) {
        *used = 1U;
        return aml_value_copy(value,
                              &execution->local[aml[0] - AML_LOCAL0_OP]);
    }
    if (aml[0] >= AML_ARG0_OP && aml[0] <= AML_ARG6_OP) {
        *used = 1U;
        return aml_value_copy(value,
                              &execution->argument[aml[0] - AML_ARG0_OP]);
    }
    if (aml[0] == AML_STRING_PREFIX) {
        for (cursor = 1U; cursor < available && aml[cursor]; cursor++) {}
        if (cursor >= available ||
            !aml_string_value(value, aml + 1U, cursor - 1U))
            return false;
        *used = cursor + 1U;
        return true;
    }
    if (aml[0] == AML_BUFFER_OP)
        return aml_eval_buffer(execution, aml, available, value, used);
    if (aml[0] == AML_PACKAGE_OP || aml[0] == AML_VAR_PACKAGE_OP)
        return aml_eval_package(execution, aml, available, value, used);

    switch (aml[0]) {
        case AML_NAME_OP:
            if (!aml_parse_name(aml + 1U, available - 1U, &path,
                                &name_used) ||
                !aml_dynamic_name(&path, &name) ||
                !aml_eval_term(execution, aml + 1U + name_used,
                               available - 1U - name_used, &source,
                               &term_used) ||
                !(dynamic = aml_dynamic_create(execution, name,
                                                AML_NODE_NAME)) ||
                !aml_value_copy(&dynamic->value, &source))
                goto finished;
            dynamic->kind = AML_NODE_NAME;
            aml_value_release(value);
            *value = aml_integer_value(0);
            *used = 1U + name_used + term_used;
            success = true;
            goto finished;

        case AML_STORE_OP:
        case AML_COPY_OBJECT_OP:
            if (!aml_eval_term(execution, aml + 1U, available - 1U,
                               &source, &term_used) ||
                !aml_value_resolve(execution, &source, &resolved) ||
                !aml_parse_target(execution, aml + 1U + term_used,
                                  available - 1U - term_used, &target,
                                  &target_used) ||
                !aml_write_reference(execution, &target, &resolved) ||
                !aml_value_copy(value, &resolved))
                goto finished;
            *used = 1U + term_used + target_used;
            success = true;
            goto finished;

        case AML_REF_OF_OP:
            if (!aml_parse_target(execution, aml + 1U, available - 1U,
                                  &target, &target_used) || !target.kind ||
                !aml_reference_value(value, &target))
                goto finished;
            *used = 1U + target_used;
            success = true;
            goto finished;

        case AML_DEREF_OF_OP:
            if (!aml_eval_term(execution, aml + 1U, available - 1U,
                               &source, &term_used) ||
                !aml_value_resolve(execution, &source, value))
                goto finished;
            *used = 1U + term_used;
            success = true;
            goto finished;

        case AML_INCREMENT_OP:
        case AML_DECREMENT_OP:
            if (!aml_parse_target(execution, aml + 1U, available - 1U,
                                  &target, &target_used) ||
                !aml_read_reference(execution, &target, &source) ||
                !aml_execution_integer(execution, &source, &integer))
                goto finished;
            integer = aml[0] == AML_INCREMENT_OP ? integer + 1U : integer - 1U;
            aml_value_release(value);
            *value = aml_integer_value(integer);
            if (!aml_write_reference(execution, &target, value))
                goto finished;
            *used = 1U + target_used;
            success = true;
            goto finished;

        case AML_ADD_OP:
        case AML_SUBTRACT_OP:
        case AML_MULTIPLY_OP:
        case AML_SHIFT_LEFT_OP:
        case AML_SHIFT_RIGHT_OP:
        case AML_AND_OP:
        case AML_NAND_OP:
        case AML_OR_OP:
        case AML_NOR_OP:
        case AML_XOR_OP:
        case AML_MOD_OP:
        case AML_LAND_OP:
        case AML_LOR_OP:
        case AML_LEQUAL_OP:
        case AML_LGREATER_OP:
        case AML_LLESS_OP:
            return aml_eval_binary_integer(execution, aml, available,
                                           value, used);

        case AML_DIVIDE_OP:
            return aml_eval_divide(execution, aml, available, value, used);

        case AML_NOT_OP:
            if (!aml_eval_term(execution, aml + 1U, available - 1U,
                               &source, &term_used) ||
                !aml_execution_integer(execution, &source, &integer) ||
                !aml_parse_target(execution, aml + 1U + term_used,
                                  available - 1U - term_used, &target,
                                  &target_used))
                goto finished;
            aml_value_release(value);
            *value = aml_integer_value(~integer);
            if (!aml_write_reference(execution, &target, value))
                goto finished;
            *used = 1U + term_used + target_used;
            success = true;
            goto finished;

        case AML_LNOT_OP:
            if (!aml_eval_term(execution, aml + 1U, available - 1U,
                               &source, &term_used) ||
                !aml_execution_integer(execution, &source, &integer))
                goto finished;
            aml_value_release(value);
            *value = aml_integer_value(!integer);
            *used = 1U + term_used;
            success = true;
            goto finished;

        case AML_INDEX_OP:
            return aml_eval_index(execution, aml, available, value, used);

        case AML_SIZE_OF_OP:
            if (!aml_parse_target(execution, aml + 1U, available - 1U,
                                  &target, &target_used) ||
                !aml_read_reference(execution, &target, &source) ||
                !aml_value_resolve(execution, &source, &resolved))
                goto finished;
            if (resolved.kind == AML_PUBLIC_VALUE_INTEGER)
                integer = aml_integer_mask == 0xffffffffULL ? 4U : 8U;
            else if (resolved.object)
                integer = ((aml_data_t *)resolved.object)->length;
            else goto finished;
            aml_value_release(value);
            *value = aml_integer_value(integer);
            *used = 1U + target_used;
            success = true;
            goto finished;

        case AML_TO_BUFFER_OP:
        case AML_TO_DECIMAL_STRING_OP:
        case AML_TO_HEX_STRING_OP:
        case AML_TO_INTEGER_OP:
        case AML_TO_STRING_OP:
            return aml_eval_conversion(execution, aml, available,
                                       value, used);

        case AML_CREATE_DWORD_FIELD:
        case AML_CREATE_WORD_FIELD:
        case AML_CREATE_BYTE_FIELD:
        case AML_CREATE_BIT_FIELD:
        case AML_CREATE_QWORD_FIELD:
            return aml_create_buffer_field(execution, aml, available,
                                           aml[0], value, used);

        case AML_NOTIFY_OP:
            if (!aml_parse_target(execution, aml + 1U, available - 1U,
                                  &target, &target_used) ||
                !aml_eval_term(execution, aml + 1U + target_used,
                               available - 1U - target_used, &source,
                               &term_used))
                goto finished;
            if (aml_notify_handler) {
                aml_handle_t notified = AML_HANDLE_INVALID;
                u64 notification;

                if (target.kind == AML_REF_NODE) {
                    notified = aml_alias_target(target.node);
                } else if (aml_read_reference(execution, &target, &operand) &&
                           aml_value_reference(&operand) &&
                           aml_value_reference(&operand)->kind == AML_REF_NODE) {
                    notified = aml_alias_target(
                        aml_value_reference(&operand)->node);
                }
                if (notified != AML_HANDLE_INVALID &&
                    aml_execution_integer(execution, &source, &notification))
                    aml_notify_handler(notified, notification);
                aml_value_release(&operand);
            }
            aml_value_release(value);
            *value = aml_integer_value(0);
            *used = 1U + target_used + term_used;
            success = true;
            goto finished;

        case AML_NOOP_OP:
            aml_value_release(value);
            *value = aml_integer_value(0);
            *used = 1U;
            return true;
        default:
            break;
    }

    if (aml[0] == AML_EXT_OP && available >= 2U) {
        switch (aml[1]) {
            case AML_EXT_COND_REF_OF_OP: {
                bool exists = false;
                u32 source_used = 0;

                if (available < 3U)
                    goto finished;
                if (aml[2] >= AML_LOCAL0_OP && aml[2] <= AML_ARG6_OP) {
                    if (!aml_parse_target(execution, aml + 2U,
                                          available - 2U,
                                          &source_reference, &source_used))
                        goto finished;
                    exists = true;
                } else if (aml_parse_name(aml + 2U, available - 2U,
                                          &path, &source_used)) {
                    if (aml_dynamic_name(&path, &name) &&
                        (dynamic = aml_dynamic_find(execution, name))) {
                        source_reference.kind = AML_REF_DYNAMIC;
                        source_reference.dynamic = dynamic;
                        exists = true;
                    } else {
                        handle = aml_resolve(execution->scope, &path);
                        if (handle != AML_HANDLE_INVALID) {
                            source_reference.kind = AML_REF_NODE;
                            source_reference.node = handle;
                            exists = true;
                        }
                    }
                } else goto finished;
                if (!aml_parse_target(execution, aml + 2U + source_used,
                                      available - 2U - source_used,
                                      &target, &target_used))
                    goto finished;
                if (exists) {
                    if (!aml_reference_value(&operand, &source_reference) ||
                        !aml_write_reference(execution, &target, &operand))
                        goto finished;
                }
                aml_value_release(value);
                *value = aml_integer_value(exists ? aml_integer_mask : 0U);
                *used = 2U + source_used + target_used;
                success = true;
                goto finished;
            }

            case AML_EXT_CREATE_FIELD_OP:
                return aml_create_buffer_field(execution, aml, available,
                                               AML_EXT_CREATE_FIELD_OP,
                                               value, used);

            case AML_EXT_STALL_OP:
            case AML_EXT_SLEEP_OP:
                if (!aml_eval_term(execution, aml + 2U, available - 2U,
                                   &source, &term_used) ||
                    !aml_execution_integer(execution, &source, &integer))
                    goto finished;
                if (integer &&
                    ((aml[1] == AML_EXT_SLEEP_OP &&
                      integer > AML_METHOD_TIMEOUT_US / 1000U) ||
                     (aml[1] == AML_EXT_STALL_OP &&
                      integer > AML_METHOD_TIMEOUT_US) ||
                     !timer_monotonic_delay_us(
                         aml[1] == AML_EXT_SLEEP_OP ? integer * 1000U :
                                                     integer))) {
                    aml_note_failure(AML_FAILURE_TIMEOUT, aml[1],
                                     execution->scope, NULL, 0, 0);
                    goto finished;
                }
                aml_value_release(value);
                *value = aml_integer_value(0);
                *used = 2U + term_used;
                success = true;
                goto finished;

            case AML_EXT_ACQUIRE_OP:
                if (!aml_parse_target(execution, aml + 2U, available - 2U,
                                      &target, &target_used) ||
                    available - 2U - target_used < 2U)
                    goto finished;
                integer = (u64)((u16)aml[2U + target_used] |
                                ((u16)aml[3U + target_used] << 8));
                aml_value_release(value);
                if (target.kind != AML_REF_NODE ||
                    !aml_mutex_acquire(execution, target.node, (u16)integer))
                    *value = aml_integer_value(aml_integer_mask);
                else
                    *value = aml_integer_value(0);
                *used = 4U + target_used;
                success = true;
                goto finished;

            case AML_EXT_RELEASE_OP:
                if (!aml_parse_target(execution, aml + 2U, available - 2U,
                                      &target, &target_used) ||
                    target.kind != AML_REF_NODE ||
                    !aml_mutex_release(execution, target.node))
                    goto finished;
                aml_value_release(value);
                *value = aml_integer_value(0);
                *used = 2U + target_used;
                success = true;
                goto finished;

            case AML_EXT_OP_REGION_OP:
                cursor = 2U;
                if (!aml_parse_name(aml + cursor, available - cursor,
                                    &path, &name_used) ||
                    !aml_dynamic_name(&path, &name))
                    goto finished;
                cursor += name_used;
                if (available - cursor < 1U)
                    goto finished;
                dynamic = aml_dynamic_create(execution, name,
                                             AML_NODE_REGION);
                if (!dynamic)
                    goto finished;
                dynamic->kind = AML_NODE_REGION;
                dynamic->region = (aml_region_object_t){
                    .space = aml[cursor++],
                };
                dynamic->region.offset_expression = aml + cursor;
                if (!aml_term_span(execution->scope, aml + cursor,
                                   available - cursor, &term_used))
                    goto finished;
                dynamic->region.offset_length = term_used;
                cursor += term_used;
                dynamic->region.length_expression = aml + cursor;
                if (!aml_term_span(execution->scope, aml + cursor,
                                   available - cursor, &term_used))
                    goto finished;
                dynamic->region.length_length = term_used;
                cursor += term_used;
                aml_value_release(value);
                *value = aml_integer_value(0);
                *used = cursor;
                success = true;
                goto finished;

            case AML_EXT_FIELD_OP:
                if (!aml_package_span(aml + 2U, available - 2U,
                                      &encoded, &package))
                    goto finished;
                cursor = 2U + encoded;
                if (!aml_parse_name(aml + cursor,
                                    2U + package - cursor,
                                    &path, &name_used))
                    goto finished;
                cursor += name_used;
                if (cursor >= 2U + package)
                    goto finished;
                aml_region_object_t *runtime_region = NULL;
                aml_handle_t static_region = AML_HANDLE_INVALID;
                if (aml_dynamic_name(&path, &name) &&
                    (dynamic = aml_dynamic_find(execution, name)) &&
                    dynamic->kind == AML_NODE_REGION)
                    runtime_region = &dynamic->region;
                else
                    static_region = aml_resolve(execution->scope, &path);
                if (!runtime_region && static_region == AML_HANDLE_INVALID)
                    goto finished;
                if (!aml_dynamic_field_list(
                        execution, runtime_region, static_region,
                        aml + cursor + 1U, 2U + package - cursor - 1U,
                        aml[cursor]))
                    goto finished;
                aml_value_release(value);
                *value = aml_integer_value(0);
                *used = 2U + package;
                success = true;
                goto finished;
            default:
                break;
        }
    }

    if (aml_name_starts(aml, available))
        return aml_eval_name_reference(execution, aml, available,
                                       value, used);

    aml_note_failure(AML_FAILURE_UNSUPPORTED_OPCODE, aml[0],
                     execution->scope, NULL, 0, 0);

finished:
    aml_reference_release(&target);
    aml_reference_release(&source_reference);
    aml_value_release(&source);
    aml_value_release(&resolved);
    aml_value_release(&operand);
    if (!success && aml_failure.kind == AML_FAILURE_NONE)
        aml_note_failure(AML_FAILURE_OBJECT_TYPE, aml[0], execution->scope,
                         NULL, 0, 0);
    return success;
}
