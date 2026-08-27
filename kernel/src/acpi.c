#include <acpi.h>
#include <address_space.h>
#include <kprint.h>
#include <stddef.h>
#include <string.h>

#define ACPI_MAX_CPUS 256
#define ACPI_MAX_IO_APICS 16
#define ACPI_MAX_ISOS 64
#define ACPI_MAX_DEFINITION_TABLES 64
#define ACPI_MAX_TABLE_LENGTH (1024U * 1024U)

#define FADT_FIRMWARE_CONTROL       36U
#define FADT_DSDT                   40U
#define FADT_SCI_INTERRUPT          46U
#define FADT_SMI_COMMAND            48U
#define FADT_ACPI_ENABLE            52U
#define FADT_ACPI_DISABLE           53U
#define FADT_PM1A_EVENT_BLOCK       56U
#define FADT_PM1B_EVENT_BLOCK       60U
#define FADT_PM1A_CONTROL_BLOCK     64U
#define FADT_PM1B_CONTROL_BLOCK     68U
#define FADT_PM_TIMER_BLOCK         76U
#define FADT_GPE0_BLOCK             80U
#define FADT_GPE1_BLOCK             84U
#define FADT_PM1_EVENT_LENGTH       88U
#define FADT_PM1_CONTROL_LENGTH     89U
#define FADT_PM_TIMER_LENGTH        91U
#define FADT_GPE0_LENGTH             92U
#define FADT_GPE1_LENGTH             93U
#define FADT_GPE1_BASE               94U
#define FADT_FLAGS                  112U
#define FADT_RESET_REGISTER         116U
#define FADT_RESET_VALUE            128U
#define FADT_X_FIRMWARE_CONTROL     132U
#define FADT_X_DSDT                 140U
#define FADT_X_PM1A_EVENT_BLOCK     148U
#define FADT_X_PM1B_EVENT_BLOCK     160U
#define FADT_X_PM1A_CONTROL_BLOCK   172U
#define FADT_X_PM1B_CONTROL_BLOCK   184U
#define FADT_X_PM_TIMER_BLOCK       208U
#define FADT_X_GPE0_BLOCK            220U
#define FADT_X_GPE1_BLOCK            232U

#define FADT_FLAG_TIMER_32BIT       (1U << 8)
#define FADT_FLAG_RESET_REGISTER    (1U << 10)
#define FADT_FLAG_HARDWARE_REDUCED  (1U << 20)

#define ECDT_CONTROL                 36U
#define ECDT_DATA                    48U
#define ECDT_UID                     60U
#define ECDT_GPE                     64U
#define ECDT_MIN_LENGTH              65U

static bool present = false;
static void *rsdp = NULL;
static acpi_madt_t *madt = NULL;
static acpi_hpet_t *hpet = NULL;
static acpi_fadt_info_t fadt_info;
static bool fadt_available = false;
static acpi_s5_info_t s5_info;
static bool s5_available = false;
static acpi_ec_info_t ec_info;
static bool ec_info_available = false;
static acpi_sdt_header_t *ecdt = NULL;
static acpi_sdt_header_t *definition_tables[ACPI_MAX_DEFINITION_TABLES];
static u32 definition_table_count = 0;
static acpi_cpu_t cpus[ACPI_MAX_CPUS];
static u32 cpu_count = 0;
static acpi_io_apic_t io_apics[ACPI_MAX_IO_APICS];
static u32 io_apic_count = 0;
static acpi_iso_t isos[ACPI_MAX_ISOS];
static u32 iso_count = 0;

bool acpi_present(void)
{
    return present;
}

void *acpi_rsdp(void)
{
    return rsdp;
}

acpi_madt_t *acpi_madt(void)
{
    return madt;
}

const acpi_hpet_t *acpi_hpet(void)
{
    return hpet;
}

bool acpi_fadt_available(void)
{
    return fadt_available;
}

const acpi_fadt_info_t *acpi_fadt_get(void)
{
    return fadt_available ? &fadt_info : NULL;
}

bool acpi_fadt_has_reset(void)
{
    return fadt_available && fadt_info.reset_supported;
}

bool acpi_fadt_has_pm_timer(void)
{
    return fadt_available && fadt_info.pm_timer_available;
}

bool acpi_s5_available(void)
{
    return s5_available;
}

const acpi_s5_info_t *acpi_s5_get(void)
{
    return s5_available ? &s5_info : NULL;
}

bool acpi_ec_info_available(void)
{
    return ec_info_available;
}

const acpi_ec_info_t *acpi_ec_info_get(void)
{
    return ec_info_available ? &ec_info : NULL;
}

const acpi_sdt_header_t *acpi_dsdt(void)
{
    if (!fadt_available || !fadt_info.x_dsdt)
        return NULL;
    return (const acpi_sdt_header_t *)phys_to_virt(
        (phys_addr_t)fadt_info.x_dsdt);
}

u32 acpi_definition_table_count(void)
{
    return definition_table_count;
}

const acpi_sdt_header_t *acpi_definition_table(u32 index)
{
    if (index >= definition_table_count)
        return NULL;
    return definition_tables[index];
}

static bool acpi_checksum_valid(const void *table, u32 length)
{
    const u8 *bytes = (const u8 *)table;
    u8 sum = 0;

    if (!table || length < sizeof(acpi_sdt_header_t))
        return false;
    for (u32 i = 0; i < length; i++)
        sum += bytes[i];
    return sum == 0;
}

static bool acpi_table_valid(const acpi_sdt_header_t *table,
                             u32 minimum_length)
{
    if (!table || table->length < minimum_length ||
        table->length > ACPI_MAX_TABLE_LENGTH)
        return false;
    return acpi_checksum_valid(table, table->length);
}

static bool fadt_field_present(u32 length, u32 offset, u32 bytes)
{
    return offset <= length && bytes <= length - offset;
}

static u16 fadt_u16(const u8 *table, u32 offset)
{
    return (u16)table[offset] | ((u16)table[offset + 1] << 8);
}

static u32 fadt_u32(const u8 *table, u32 offset)
{
    return (u32)table[offset] |
           ((u32)table[offset + 1] << 8) |
           ((u32)table[offset + 2] << 16) |
           ((u32)table[offset + 3] << 24);
}

static u64 fadt_u64(const u8 *table, u32 offset)
{
    u64 value = 0;
    for (u32 i = 0; i < 8; i++)
        value |= (u64)table[offset + i] << (i * 8);
    return value;
}

static void fadt_read_gas(acpi_generic_address_t *gas,
                          const u8 *table, u32 offset)
{
    gas->address_space_id = table[offset];
    gas->register_bit_width = table[offset + 1];
    gas->register_bit_offset = table[offset + 2];
    gas->access_size = table[offset + 3];
    gas->address = fadt_u64(table, offset + 4);
}

static bool fadt_gas_valid(const acpi_generic_address_t *gas)
{
    return gas->address != 0 && gas->register_bit_width != 0 &&
           (gas->address_space_id == ACPI_ADDRESS_SPACE_SYSTEM_MEMORY ||
            gas->address_space_id == ACPI_ADDRESS_SPACE_SYSTEM_IO);
}

static bool aml_pkg_length(const u8 *aml, u32 available,
                           u32 *encoded_length, u32 *package_length);
static bool aml_integer(const u8 *aml, u32 available,
                        u64 *value, u32 *used);
static bool aml_name_string_length(const u8 *aml, u32 available,
                                   u32 *used);
static bool aml_name_is(const u8 *aml, u32 available, const char name[4]);

static bool aml_ec_term_span(const u8 *aml, u32 available, u32 *span)
{
    u64 value;
    u32 used;

    if (!aml || !available || !span)
        return false;
    if (aml_integer(aml, available, &value, &used)) {
        *span = used;
        return true;
    }
    if (aml[0] == 0x0d) {
        used = 1;
        while (used < available && aml[used] != 0)
            used++;
        if (used >= available)
            return false;
        *span = used + 1U;
        return true;
    }
    if (aml[0] == 0x11 || aml[0] == 0x12 || aml[0] == 0x13) {
        u32 encoded;
        u32 package;
        if (!aml_pkg_length(aml + 1U, available - 1U,
                            &encoded, &package))
            return false;
        *span = 1U + package;
        return true;
    }
    return aml_name_string_length(aml, available, span);
}

static bool aml_ec_namespace_object_span(const u8 *aml, u32 available,
                                         u32 *span)
{
    u32 encoded;
    u32 package;
    u32 name_used;
    u32 value_span;

    if (!aml || !available || !span)
        return false;
    if (aml[0] == 0x10 || aml[0] == 0x14) {
        if (!aml_pkg_length(aml + 1U, available - 1U,
                            &encoded, &package))
            return false;
        *span = 1U + package;
        return true;
    }
    if (aml[0] == 0x08) {
        if (!aml_name_string_length(aml + 1U, available - 1U,
                                    &name_used) ||
            name_used > available - 1U ||
            !aml_ec_term_span(aml + 1U + name_used,
                              available - 1U - name_used, &value_span))
            return false;
        *span = 1U + name_used + value_span;
        return true;
    }
    if (aml[0] == 0x06) {
        if (!aml_name_string_length(aml + 1U, available - 1U,
                                    &name_used) ||
            name_used > available - 1U ||
            !aml_name_string_length(aml + 1U + name_used,
                                    available - 1U - name_used,
                                    &value_span))
            return false;
        *span = 1U + name_used + value_span;
        return true;
    }
    if (aml[0] == 0x5b && available >= 2U) {
        switch (aml[1]) {
            case 0x80: /* OperationRegion */
            case 0x81: /* Field */
            case 0x82: /* Device */
            case 0x83: /* Processor */
            case 0x84: /* PowerResource */
            case 0x85: /* ThermalZone */
            case 0x86: /* IndexField */
            case 0x87: /* BankField */
            case 0x88: /* DataTableRegion */
                if (!aml_pkg_length(aml + 2U, available - 2U,
                                    &encoded, &package))
                    return false;
                *span = 2U + package;
                return true;
            default:
                break;
        }
    }
    return false;
}

static bool aml_ec_find_property(const u8 *body, u32 body_length,
                                 const char name[4],
                                 const u8 **value, u32 *value_available,
                                 bool *is_method)
{
    u32 offset = 0;

    if (!body || !name || !value || !value_available || !is_method)
        return false;
    while (offset < body_length) {
        u32 name_used;
        u32 span;

        if (body[offset] == 0x08 &&
            aml_name_string_length(body + offset + 1U,
                                   body_length - offset - 1U,
                                   &name_used)) {
            if (name_used == 4U &&
                aml_name_is(body + offset + 1U, name_used, name)) {
                *value = body + offset + 1U + name_used;
                *value_available = body_length - offset - 1U - name_used;
                *is_method = false;
                return true;
            }
        }
        if (body[offset] == 0x14 &&
            aml_pkg_length(body + offset + 1U,
                           body_length - offset - 1U,
                           &name_used, &span)) {
            u32 method_name_used;
            if (aml_name_string_length(body + offset + 1U + name_used,
                                       span - name_used,
                                       &method_name_used) &&
                method_name_used == 4U &&
                aml_name_is(body + offset + 1U + name_used,
                            method_name_used, name)) {
                *value = body + offset;
                *value_available = 1U + span;
                *is_method = true;
                return true;
            }
        }
        if (!aml_ec_namespace_object_span(body + offset,
                                          body_length - offset, &span))
            span = 1U;
        offset += span;
    }
    return false;
}

static bool aml_name_string_length(const u8 *aml, u32 available,
                                   u32 *used)
{
    u32 offset = 0;
    u8 prefix;
    u8 count;

    if (!aml || !available || !used)
        return false;
    if (aml[offset] == '\\') {
        if (++offset >= available)
            return false;
    }
    while (offset < available && aml[offset] == '^')
        offset++;
    if (offset >= available)
        return false;

    prefix = aml[offset];
    if (prefix == 0x2e) { /* DualNamePrefix */
        if (available - offset < 1U + 8U)
            return false;
        offset += 1U + 8U;
    } else if (prefix == 0x2f) { /* MultiNamePrefix */
        if (available - offset < 2U)
            return false;
        count = aml[offset + 1U];
        if (!count || count > (available - offset - 2U) / 4U)
            return false;
        offset += 2U + (u32)count * 4U;
    } else {
        if (available - offset < 4U)
            return false;
        offset += 4U;
    }
    *used = offset;
    return true;
}

static bool aml_name_is(const u8 *aml, u32 available, const char name[4])
{
    return aml && name && available >= 4U &&
           aml[0] == (u8)name[0] && aml[1] == (u8)name[1] &&
           aml[2] == (u8)name[2] && aml[3] == (u8)name[3];
}

static bool aml_eisa_id_is_ec(u32 value)
{
    /* AML stores the four-byte EISA encoding as a little-endian integer.
     * The ACPI encoding of EISAID("PNP0C09") is therefore 0x090CD041. */
    return value == 0x090CD041U;
}

static bool aml_ec_hid(const u8 *aml, u32 available)
{
    u64 value;
    u32 used;

    if (!aml || !available)
        return false;
    if (aml[0] == 0x0d) { /* StringPrefix */
        return available >= 9U &&
               aml[1] == 'P' && aml[2] == 'N' && aml[3] == 'P' &&
               aml[4] == '0' && aml[5] == 'C' && aml[6] == '0' &&
               aml[7] == '9' && aml[8] == 0;
    }
    if (!aml_integer(aml, available, &value, &used) || used != 5U)
        return false;
    return aml_eisa_id_is_ec((u32)value);
}

static bool aml_ec_parse_crs(const u8 *aml, u32 available,
                             acpi_ec_info_t *result)
{
    u32 encoded_length;
    u32 package_length;
    u32 body_length;
    u32 used;
    u64 buffer_length;
    const u8 *resource;
    u32 resource_length;
    acpi_generic_address_t ports[2] = {{0}};
    u32 port_count = 0;

    if (!aml || !result || available < 2U || aml[0] != 0x11 ||
        !aml_pkg_length(aml + 1U, available - 1U,
                        &encoded_length, &package_length) ||
        package_length < encoded_length)
        return false;
    body_length = package_length - encoded_length;
    if (!aml_integer(aml + 1U + encoded_length, body_length,
                     &buffer_length, &used) ||
        buffer_length > body_length - used)
        return false;
    resource = aml + 1U + encoded_length + used;
    resource_length = (u32)buffer_length;

    for (u32 offset = 0; offset < resource_length;) {
        u8 tag = resource[offset];
        u32 descriptor_length;

        if (tag == 0x79) /* EndTag */
            break;
        if (tag & 0x80U) {
            if (resource_length - offset < 3U)
                return false;
            descriptor_length = (u32)resource[offset + 1U] |
                                ((u32)resource[offset + 2U] << 8);
            if (descriptor_length > resource_length - offset - 3U)
                return false;
            /* Large address descriptors are intentionally not interpreted in
             * this small parser; accepting only the standard one-byte IO
             * resources avoids guessing at a runtime _CRS result. */
            offset += 3U + descriptor_length;
            continue;
        }

        descriptor_length = tag & 0x07U;
        if (descriptor_length > resource_length - offset - 1U)
            return false;
        if ((tag >> 3) == 0x08U && descriptor_length == 7U &&
            port_count < 2U) { /* I/O port descriptor */
            u8 information = resource[offset + 1U];
            u16 minimum = (u16)resource[offset + 2U] |
                          ((u16)resource[offset + 3U] << 8);
            u16 maximum = (u16)resource[offset + 4U] |
                          ((u16)resource[offset + 5U] << 8);
            u8 length = resource[offset + 7U];

            if ((information & 0xfeU) != 0 || minimum != maximum ||
                length != 1U)
                return false;
            ports[port_count].address_space_id =
                ACPI_ADDRESS_SPACE_SYSTEM_IO;
            ports[port_count].register_bit_width = 8;
            ports[port_count].register_bit_offset = 0;
            ports[port_count].access_size = 0;
            ports[port_count].address = minimum;
            port_count++;
        }
        offset += 1U + descriptor_length;
    }

    if (port_count != 2U || ports[0].address == ports[1].address)
        return false;
    result->data = ports[0];
    result->control = ports[1];
    return true;
}

static bool aml_ec_parse_device(const u8 *aml, u32 available,
                                acpi_ec_info_t *result,
                                bool *hid_found, bool *crs_found,
                                bool *crs_method_found)
{
    u32 encoded_length;
    u32 package_length;
    u32 name_length;
    const u8 *body;
    u32 body_length;
    const u8 *hid_value = NULL;
    const u8 *crs_value = NULL;
    const u8 *gpe_value = NULL;
    u32 hid_available;
    u32 crs_available;
    u32 gpe_available;
    bool hid_method;
    bool crs_method;
    bool gpe_method;
    bool hid;
    bool crs;
    bool crs_property;
    bool gpe;
    u64 gpe_number;
    u32 gpe_used;

    if (hid_found)
        *hid_found = false;
    if (crs_found)
        *crs_found = false;
    if (crs_method_found)
        *crs_method_found = false;

    if (!aml || !result || available < 3U || aml[0] != 0x5b ||
        aml[1] != 0x82 ||
        !aml_pkg_length(aml + 2U, available - 2U,
                        &encoded_length, &package_length) ||
        package_length < encoded_length ||
        package_length > available - 2U ||
        !aml_name_string_length(aml + 2U + encoded_length,
                                package_length - encoded_length,
                                &name_length) ||
        name_length > package_length - encoded_length)
        return false;

    body = aml + 2U + encoded_length + name_length;
    body_length = package_length - encoded_length - name_length;
    hid = aml_ec_find_property(body, body_length, "_HID", &hid_value,
                               &hid_available, &hid_method) &&
          !hid_method && aml_ec_hid(hid_value, hid_available);
    if (hid && hid_found)
        *hid_found = true;
    if (!hid)
        return false;

    crs_property = aml_ec_find_property(body, body_length, "_CRS",
                                        &crs_value, &crs_available,
                                        &crs_method);
    crs = crs_property &&
          !crs_method && aml_ec_parse_crs(crs_value, crs_available,
                                          result);
    if (crs_found && crs_property)
        *crs_found = true;
    if (crs_method && crs_method_found)
        *crs_method_found = true;
    if (!crs)
        return false;

    result->gpe_valid = false;
    gpe = aml_ec_find_property(body, body_length, "_GPE", &gpe_value,
                               &gpe_available, &gpe_method) && !gpe_method;
    if (gpe && aml_integer(gpe_value, gpe_available, &gpe_number,
                           &gpe_used) && gpe_number <= 0xffU) {
        result->gpe = (u8)gpe_number;
        result->gpe_valid = true;
    }
    result->uid = 0;
    result->discovery = ACPI_EC_DISCOVERY_NAMESPACE;
    return true;
}

static bool aml_find_ec(const acpi_sdt_header_t *table,
                        acpi_ec_info_t *result)
{
    const u8 *body;
    u32 body_length;

    if (!table || !result || table->length < sizeof(*table))
        return false;
    body = (const u8 *)table + sizeof(*table);
    body_length = table->length - sizeof(*table);
    for (u32 offset = 0; offset + 2U < body_length; offset++) {
        bool hid_found;
        bool crs_found;
        bool crs_method_found;

        if (body[offset] != 0x5b || body[offset + 1U] != 0x82)
            continue;
        if (aml_ec_parse_device(body + offset, body_length - offset,
                                result, &hid_found, &crs_found,
                                &crs_method_found))
            return true;
        (void)hid_found;
        (void)crs_found;
        (void)crs_method_found;
    }
    return false;
}

static bool acpi_parse_ecdt(const acpi_sdt_header_t *table,
                            acpi_ec_info_t *result)
{
    const u8 *bytes = (const u8 *)table;

    if (!table || !result || !acpi_table_valid(table, ECDT_MIN_LENGTH))
        return false;
    fadt_read_gas(&result->data, bytes, ECDT_DATA);
    fadt_read_gas(&result->control, bytes, ECDT_CONTROL);
    if (!fadt_gas_valid(&result->data) ||
        !fadt_gas_valid(&result->control) ||
        result->data.register_bit_width != 8 ||
        result->control.register_bit_width != 8 ||
        result->data.register_bit_offset != 0 ||
        result->control.register_bit_offset != 0 ||
        !fadt_field_present(table->length, ECDT_UID, 4) ||
        !fadt_field_present(table->length, ECDT_GPE, 1))
        return false;
    result->uid = fadt_u32(bytes, ECDT_UID);
    result->gpe = bytes[ECDT_GPE];
    result->gpe_valid = true;
    result->discovery = ACPI_EC_DISCOVERY_ECDT;
    return true;
}

static void acpi_find_ec(void)
{
    const acpi_sdt_header_t *dsdt;

    ec_info = (acpi_ec_info_t){0};
    ec_info_available = false;
    if (!fadt_available)
        return;

    /* ECDT is explicitly intended to provide EC resources before AML
     * namespace evaluation and therefore has priority over the fallback. */
    if (ecdt && acpi_parse_ecdt(ecdt, &ec_info)) {
        ec_info_available = true;
        return;
    }

    dsdt = (const acpi_sdt_header_t *)phys_to_virt(
        (phys_addr_t)fadt_info.x_dsdt);
    if (acpi_table_valid(dsdt, sizeof(acpi_sdt_header_t)) &&
        dsdt->signature[0] == 'D' && dsdt->signature[1] == 'S' &&
        dsdt->signature[2] == 'D' && dsdt->signature[3] == 'T' &&
        aml_find_ec(dsdt, &ec_info)) {
        ec_info_available = true;
        return;
    }
    for (u32 i = 0; i < definition_table_count; i++) {
        const acpi_sdt_header_t *table = definition_tables[i];
        if (table->signature[0] != 'S' || table->signature[1] != 'S' ||
            table->signature[2] != 'D' || table->signature[3] != 'T')
            continue;
        if (aml_find_ec(table, &ec_info)) {
            ec_info_available = true;
            return;
        }
    }
}

static bool aml_pkg_length(const u8 *aml, u32 available,
                           u32 *encoded_length, u32 *package_length)
{
    u8 first;
    u8 follow;
    u32 length;

    if (!aml || !available || !encoded_length || !package_length)
        return false;
    first = aml[0];
    follow = first >> 6;
    if (follow == 0) {
        length = first & 0x3fU;
    } else {
        if (follow > 3 || available < (u32)follow + 1U)
            return false;
        length = first & 0x0fU;
        for (u8 i = 0; i < follow; i++)
            length |= (u32)aml[1U + i] << (4U + 8U * i);
    }
    if (length < (u32)follow + 1U || length > available)
        return false;
    *encoded_length = (u32)follow + 1U;
    *package_length = length;
    return true;
}

static bool aml_integer(const u8 *aml, u32 available,
                        u64 *value, u32 *used)
{
    u8 prefix;

    if (!aml || !available || !value || !used)
        return false;
    prefix = aml[0];
    switch (prefix) {
        case 0x00: /* ZeroOp */
            *value = 0;
            *used = 1;
            return true;
        case 0x01: /* OneOp */
            *value = 1;
            *used = 1;
            return true;
        case 0x0a: /* BytePrefix */
            if (available < 2) return false;
            *value = aml[1];
            *used = 2;
            return true;
        case 0x0b: /* WordPrefix */
            if (available < 3) return false;
            *value = (u64)aml[1] | ((u64)aml[2] << 8);
            *used = 3;
            return true;
        case 0x0c: /* DWordPrefix */
            if (available < 5) return false;
            *value = (u64)aml[1] | ((u64)aml[2] << 8) |
                     ((u64)aml[3] << 16) | ((u64)aml[4] << 24);
            *used = 5;
            return true;
        case 0x0e: /* QWordPrefix */
            if (available < 9) return false;
            *value = 0;
            for (u8 i = 0; i < 8; i++)
                *value |= (u64)aml[1U + i] << (8U * i);
            *used = 9;
            return true;
        default:
            return false;
    }
}

static bool aml_s5_package(const u8 *aml, u32 available,
                           acpi_s5_info_t *result)
{
    u32 encoded_length;
    u32 package_length;
    u32 body_length;
    u32 used;
    u64 a;
    u64 b;

    if (!aml || !result || available < 2 || aml[0] != 0x12 ||
        !aml_pkg_length(aml + 1, available - 1,
                        &encoded_length, &package_length))
        return false;
    if (package_length < encoded_length + 1U)
        return false;
    body_length = package_length - encoded_length;
    if (!body_length || aml[1U + encoded_length] < 2)
        return false;
    body_length--;
    if (!aml_integer(aml + 2U + encoded_length, body_length,
                     &a, &used) || used > body_length)
        return false;
    body_length -= used;
    if (!aml_integer(aml + 2U + encoded_length + used, body_length,
                     &b, &used) || a > 7 || b > 7)
        return false;
    result->pm1a_sleep_type = (u8)a;
    result->pm1b_sleep_type = (u8)b;
    return true;
}

static bool aml_find_s5(const acpi_sdt_header_t *table,
                        acpi_s5_info_t *result)
{
    const u8 *body;
    u32 body_length;

    if (!table || !result || table->length < sizeof(*table))
        return false;
    body = (const u8 *)table + sizeof(*table);
    body_length = table->length - sizeof(*table);
    for (u32 offset = 0; offset + 5U < body_length; offset++) {
        if (body[offset] != 0x08 ||
            body[offset + 1] != '_' || body[offset + 2] != 'S' ||
            body[offset + 3] != '5' || body[offset + 4] != '_')
            continue;
        if (aml_s5_package(body + offset + 5U,
                           body_length - offset - 5U, result))
            return true;
    }
    return false;
}

static void acpi_find_s5(void)
{
    const acpi_sdt_header_t *dsdt;

    s5_available = false;
    if (!fadt_available || !fadt_info.x_dsdt)
        return;

    dsdt = (const acpi_sdt_header_t *)phys_to_virt(
        (phys_addr_t)fadt_info.x_dsdt);
    if (acpi_table_valid(dsdt, sizeof(acpi_sdt_header_t)) &&
        dsdt->signature[0] == 'D' && dsdt->signature[1] == 'S' &&
        dsdt->signature[2] == 'D' && dsdt->signature[3] == 'T' &&
        aml_find_s5(dsdt, &s5_info)) {
        s5_available = true;
    } else {
        for (u32 i = 0; i < definition_table_count; i++) {
            const acpi_sdt_header_t *ssdt = definition_tables[i];
            if (ssdt->signature[0] != 'S' || ssdt->signature[1] != 'S' ||
                ssdt->signature[2] != 'D' || ssdt->signature[3] != 'T')
                continue;
            if (aml_find_s5(ssdt, &s5_info)) {
                s5_available = true;
                break;
            }
        }
    }
    if (s5_available) {
        KERNEL_BOOT_DEBUG_LOG("[ACPI] S5 ready\n");
#ifdef ACPI_POWER_DEBUG
        kprint("[ACPI] _S5 pm1a=%u pm1b=%u\n",
               s5_info.pm1a_sleep_type, s5_info.pm1b_sleep_type);
#endif
    } else {
        KERNEL_BOOT_DEBUG_LOG("[ACPI] _S5 unavailable\n");
    }
}

static acpi_generic_address_t fadt_legacy_gas(u32 address, u8 length)
{
    acpi_generic_address_t gas = {0};
    gas.address_space_id = ACPI_ADDRESS_SPACE_SYSTEM_IO;
    gas.register_bit_width = (u8)(length * 8U);
    gas.access_size = length >= 8 ? 4 : (length >= 4 ? 3 :
                         (length >= 2 ? 2 : 1));
    gas.address = address;
    return gas;
}

static acpi_generic_address_t fadt_effective_gas(
    const acpi_generic_address_t *extended, u32 legacy_address, u8 length)
{
    if (fadt_gas_valid(extended))
        return *extended;
    if (legacy_address && length)
        return fadt_legacy_gas(legacy_address, length);
    return (acpi_generic_address_t){0};
}

/* GPE blocks are byte arrays, not one scalar register.  Their extended GAS
 * describes the block base, while the legacy fields contain the same base
 * address.  Normalize the scalar width to one byte so callers do not mistake
 * the total block length for a register access width. */
static acpi_generic_address_t fadt_effective_block_gas(
    const acpi_generic_address_t *extended, u32 legacy_address, u8 length)
{
    acpi_generic_address_t gas = {0};

    if (extended && extended->address &&
        (extended->address_space_id == ACPI_ADDRESS_SPACE_SYSTEM_MEMORY ||
         extended->address_space_id == ACPI_ADDRESS_SPACE_SYSTEM_IO)) {
        gas = *extended;
        gas.register_bit_width = 8U;
        gas.register_bit_offset = 0;
        gas.access_size = 1U;
        return gas;
    }
    if (legacy_address && length) {
        gas.address_space_id = ACPI_ADDRESS_SPACE_SYSTEM_IO;
        gas.register_bit_width = 8U;
        gas.access_size = 1U;
        gas.address = legacy_address;
    }
    return gas;
}

static void acpi_parse_fadt(const acpi_sdt_header_t *table)
{
    const u8 *bytes = (const u8 *)table;
    const u32 length = table->length;
    acpi_generic_address_t x_pm1a_event = {0};
    acpi_generic_address_t x_pm1b_event = {0};
    acpi_generic_address_t x_pm1a_control = {0};
    acpi_generic_address_t x_pm1b_control = {0};
    acpi_generic_address_t x_pm_timer = {0};
    acpi_generic_address_t x_gpe0 = {0};
    acpi_generic_address_t x_gpe1 = {0};

    fadt_info = (acpi_fadt_info_t){0};
    fadt_info.revision = table->revision;
    fadt_info.length = length;

    if (fadt_field_present(length, FADT_FIRMWARE_CONTROL, 4))
        fadt_info.firmware_control = fadt_u32(bytes, FADT_FIRMWARE_CONTROL);
    if (fadt_field_present(length, FADT_X_FIRMWARE_CONTROL, 8))
        fadt_info.x_firmware_control = fadt_u64(bytes, FADT_X_FIRMWARE_CONTROL);
    if (fadt_field_present(length, FADT_DSDT, 4))
        fadt_info.dsdt = fadt_u32(bytes, FADT_DSDT);
    if (fadt_field_present(length, FADT_X_DSDT, 8))
        fadt_info.x_dsdt = fadt_u64(bytes, FADT_X_DSDT);
    if (fadt_info.x_firmware_control == 0)
        fadt_info.x_firmware_control = fadt_info.firmware_control;
    if (fadt_info.x_dsdt == 0)
        fadt_info.x_dsdt = fadt_info.dsdt;

    if (fadt_field_present(length, FADT_SCI_INTERRUPT, 2))
        fadt_info.sci_interrupt = fadt_u16(bytes, FADT_SCI_INTERRUPT);
    if (fadt_field_present(length, FADT_SMI_COMMAND, 4))
        fadt_info.smi_command = fadt_u32(bytes, FADT_SMI_COMMAND);
    if (fadt_field_present(length, FADT_ACPI_ENABLE, 1))
        fadt_info.acpi_enable = bytes[FADT_ACPI_ENABLE];
    if (fadt_field_present(length, FADT_ACPI_DISABLE, 1))
        fadt_info.acpi_disable = bytes[FADT_ACPI_DISABLE];
    if (fadt_field_present(length, FADT_PM1_EVENT_LENGTH, 1))
        fadt_info.pm1_event_length = bytes[FADT_PM1_EVENT_LENGTH];
    if (fadt_field_present(length, FADT_PM1_CONTROL_LENGTH, 1))
        fadt_info.pm1_control_length = bytes[FADT_PM1_CONTROL_LENGTH];
    if (fadt_field_present(length, FADT_PM_TIMER_LENGTH, 1))
        fadt_info.pm_timer_length = bytes[FADT_PM_TIMER_LENGTH];
    if (fadt_field_present(length, FADT_GPE0_LENGTH, 1))
        fadt_info.gpe0_length = bytes[FADT_GPE0_LENGTH];
    if (fadt_field_present(length, FADT_GPE1_LENGTH, 1))
        fadt_info.gpe1_length = bytes[FADT_GPE1_LENGTH];
    if (fadt_field_present(length, FADT_GPE1_BASE, 1))
        fadt_info.gpe1_base = bytes[FADT_GPE1_BASE];
    if (fadt_field_present(length, FADT_FLAGS, 4))
        fadt_info.flags = fadt_u32(bytes, FADT_FLAGS);

    if (fadt_field_present(length, FADT_RESET_REGISTER, 12))
        fadt_read_gas(&fadt_info.reset_register, bytes, FADT_RESET_REGISTER);
    if (fadt_field_present(length, FADT_RESET_VALUE, 1))
        fadt_info.reset_value = bytes[FADT_RESET_VALUE];

    if (fadt_field_present(length, FADT_X_PM1A_EVENT_BLOCK, 12))
        fadt_read_gas(&x_pm1a_event, bytes, FADT_X_PM1A_EVENT_BLOCK);
    if (fadt_field_present(length, FADT_X_PM1B_EVENT_BLOCK, 12))
        fadt_read_gas(&x_pm1b_event, bytes, FADT_X_PM1B_EVENT_BLOCK);
    if (fadt_field_present(length, FADT_X_PM1A_CONTROL_BLOCK, 12))
        fadt_read_gas(&x_pm1a_control, bytes, FADT_X_PM1A_CONTROL_BLOCK);
    if (fadt_field_present(length, FADT_X_PM1B_CONTROL_BLOCK, 12))
        fadt_read_gas(&x_pm1b_control, bytes, FADT_X_PM1B_CONTROL_BLOCK);
    if (fadt_field_present(length, FADT_X_PM_TIMER_BLOCK, 12))
        fadt_read_gas(&x_pm_timer, bytes, FADT_X_PM_TIMER_BLOCK);
    if (fadt_field_present(length, FADT_X_GPE0_BLOCK, 12))
        fadt_read_gas(&x_gpe0, bytes, FADT_X_GPE0_BLOCK);
    if (fadt_field_present(length, FADT_X_GPE1_BLOCK, 12))
        fadt_read_gas(&x_gpe1, bytes, FADT_X_GPE1_BLOCK);

    fadt_info.pm1a_event_block = fadt_effective_gas(
        &x_pm1a_event,
        fadt_field_present(length, FADT_PM1A_EVENT_BLOCK, 4) ?
            fadt_u32(bytes, FADT_PM1A_EVENT_BLOCK) : 0,
        fadt_info.pm1_event_length);
    fadt_info.pm1b_event_block = fadt_effective_gas(
        &x_pm1b_event,
        fadt_field_present(length, FADT_PM1B_EVENT_BLOCK, 4) ?
            fadt_u32(bytes, FADT_PM1B_EVENT_BLOCK) : 0,
        fadt_info.pm1_event_length);
    fadt_info.pm1a_control_block = fadt_effective_gas(
        &x_pm1a_control,
        fadt_field_present(length, FADT_PM1A_CONTROL_BLOCK, 4) ?
            fadt_u32(bytes, FADT_PM1A_CONTROL_BLOCK) : 0,
        fadt_info.pm1_control_length);
    fadt_info.pm1b_control_block = fadt_effective_gas(
        &x_pm1b_control,
        fadt_field_present(length, FADT_PM1B_CONTROL_BLOCK, 4) ?
            fadt_u32(bytes, FADT_PM1B_CONTROL_BLOCK) : 0,
        fadt_info.pm1_control_length);
    fadt_info.pm_timer_block = fadt_effective_gas(
        &x_pm_timer,
        fadt_field_present(length, FADT_PM_TIMER_BLOCK, 4) ?
            fadt_u32(bytes, FADT_PM_TIMER_BLOCK) : 0,
        fadt_info.pm_timer_length);
    fadt_info.gpe0_block = fadt_effective_block_gas(
        &x_gpe0,
        fadt_field_present(length, FADT_GPE0_BLOCK, 4) ?
            fadt_u32(bytes, FADT_GPE0_BLOCK) : 0,
        fadt_info.gpe0_length);
    fadt_info.gpe1_block = fadt_effective_block_gas(
        &x_gpe1,
        fadt_field_present(length, FADT_GPE1_BLOCK, 4) ?
            fadt_u32(bytes, FADT_GPE1_BLOCK) : 0,
        fadt_info.gpe1_length);

    fadt_info.hardware_reduced = (fadt_info.flags & FADT_FLAG_HARDWARE_REDUCED) != 0;
    fadt_info.reset_supported =
        (fadt_info.flags & FADT_FLAG_RESET_REGISTER) != 0 &&
        fadt_gas_valid(&fadt_info.reset_register);
    fadt_info.legacy_pm1_control = !fadt_info.hardware_reduced &&
        fadt_info.pm1_control_length != 0 &&
        fadt_gas_valid(&fadt_info.pm1a_control_block);
    fadt_info.pm_timer_available = fadt_info.pm_timer_length != 0 &&
        fadt_gas_valid(&fadt_info.pm_timer_block);
    fadt_info.pm_timer_32bit = (fadt_info.flags & FADT_FLAG_TIMER_32BIT) != 0;
    fadt_available = true;

    KERNEL_BOOT_DEBUG_LOG(
        "[ACPI] FADT rev=%u pm1ctl=%u reset=%u pm_timer=%u%s reduced=%u\n",
        fadt_info.revision, fadt_info.legacy_pm1_control,
        fadt_info.reset_supported, fadt_info.pm_timer_available,
        fadt_info.pm_timer_32bit ? " (32-bit)" : "",
        fadt_info.hardware_reduced);
#ifdef ACPI_POWER_DEBUG
    kprint("[ACPI] FADT len=%u flags=%08x\n",
           fadt_info.length, fadt_info.flags);
    kprint("[ACPI] FADT DSDT=0x%llx SCI=%u SMI=0x%08x enable=0x%02x disable=0x%02x\n",
           fadt_info.x_dsdt, fadt_info.sci_interrupt, fadt_info.smi_command,
           fadt_info.acpi_enable, fadt_info.acpi_disable);
    kprint("[ACPI] FADT PM1aCtl=0x%llx PMTimer=0x%llx\n",
           fadt_info.pm1a_control_block.address,
           fadt_info.pm_timer_block.address);
    kprint("[ACPI] FADT reset=0x%llx value=0x%02x reset=%u pm_timer=%u%s reduced=%u pm1ctl=%u\n",
           fadt_info.reset_register.address, fadt_info.reset_value,
           fadt_info.reset_supported, fadt_info.pm_timer_available,
           fadt_info.pm_timer_32bit ? " (32-bit)" : "",
           fadt_info.hardware_reduced, fadt_info.legacy_pm1_control);
    kprint("[ACPI] FADT GPE0=0x%llx/%u GPE1=0x%llx/%u base=%u\n",
           fadt_info.gpe0_block.address, fadt_info.gpe0_length,
           fadt_info.gpe1_block.address, fadt_info.gpe1_length,
           fadt_info.gpe1_base);
#endif
}

u32 acpi_cpu_count(void)
{
    return cpu_count;
}

const acpi_cpu_t *acpi_cpu(u32 index)
{
    if (index >= cpu_count)
    {
        return NULL;
    }

    return &cpus[index];
}

void acpi_init(BOOT_INFO *BootInfo)
{
    present = false;
    rsdp = NULL;
    madt = NULL;
    hpet = NULL;
    fadt_info = (acpi_fadt_info_t){0};
    fadt_available = false;
    s5_info = (acpi_s5_info_t){0};
    s5_available = false;
    ec_info = (acpi_ec_info_t){0};
    ec_info_available = false;
    ecdt = NULL;
    definition_table_count = 0;
    cpu_count = 0;
    io_apic_count = 0;
    iso_count = 0;

    if (!BootInfo)
    {
        return;
    }
    
    acpi_rsdp_t *header = (acpi_rsdp_t *)BootInfo->Rsdp;
    if (!header) { return; }

    static const char signature[8] = {
        'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '
    };

    for (u32 i = 0; i < 8; i++) {
        if (header->signature[i] != signature[i]) { return; }
    }

    u8 sum = 0;

    u8 *bytes = (u8 *)header;

    for (u32 i = 0; i < 20; i++) {
        sum += bytes[i];
    }

    if (sum != 0) { return; }

    rsdp = header;
    present = true;

    acpi_sdt_header_t *xsdt = (acpi_sdt_header_t *)phys_to_virt(
        (phys_addr_t)header->xsdt_address);

    if (!xsdt)
    {
        present = false;
        rsdp = NULL;
        return;
    }
    
    static const char xsdt_signature[4] = {
        'X', 'S', 'D', 'T'
    };
    
    for (u32 i = 0; i < 4; i++)
    {
        if (xsdt->signature[i] != xsdt_signature[i])
        {
            present = false;
            rsdp = NULL;
            return;
        }
    }

    if (!acpi_table_valid(xsdt, sizeof(acpi_sdt_header_t)) ||
        (xsdt->length - sizeof(acpi_sdt_header_t)) % sizeof(u64) != 0)
    {
        present = false;
        rsdp = NULL;
        return;
    }

    u32 entry_count =
        (xsdt->length - sizeof(acpi_sdt_header_t)) / sizeof(u64);

    u64 *entries =
        (u64 *)((u8 *)xsdt + sizeof(acpi_sdt_header_t));

    for (u32 i = 0; i < entry_count; i++) {
        acpi_sdt_header_t *table = (acpi_sdt_header_t *)phys_to_virt(
            (phys_addr_t)entries[i]);

        if (!table) { continue; }

        if (table->signature[0] == 'F' &&
            table->signature[1] == 'A' &&
            table->signature[2] == 'C' &&
            table->signature[3] == 'P')
        {
            if (acpi_table_valid(table, FADT_FIRMWARE_CONTROL))
                acpi_parse_fadt(table);
        }
        if (table->signature[0] == 'A' &&
            table->signature[1] == 'P' &&
            table->signature[2] == 'I' &&
            table->signature[3] == 'C')
        {
            madt = (acpi_madt_t *)table;
        }
        if (table->signature[0] == 'H' &&
            table->signature[1] == 'P' &&
            table->signature[2] == 'E' &&
            table->signature[3] == 'T' &&
            table->length >= sizeof(acpi_hpet_t))
        {
            if (acpi_checksum_valid(table, table->length))
                hpet = (acpi_hpet_t *)table;
        }
        if (table->signature[0] == 'E' && table->signature[1] == 'C' &&
            table->signature[2] == 'D' && table->signature[3] == 'T' &&
            acpi_table_valid(table, ECDT_MIN_LENGTH))
            ecdt = table;
        if (table->signature[0] == 'S' && table->signature[1] == 'S' &&
            table->signature[2] == 'D' && table->signature[3] == 'T' &&
            acpi_table_valid(table, sizeof(acpi_sdt_header_t)) &&
            definition_table_count < ACPI_MAX_DEFINITION_TABLES)
            definition_tables[definition_table_count++] = table;
    }

    acpi_find_ec();
    acpi_find_s5();

    if (!madt) {
        present = false;
        rsdp = NULL;
        return;
    }

    acpi_madt_entry_t *entry =
        (acpi_madt_entry_t *)((u8 *)madt + sizeof(acpi_madt_t));

    u8 *end =
        (u8 *)madt + madt->header.length;

    while ((u8 *)entry < end) {
        if (entry->type == 0)
        {
            acpi_madt_local_apic_t *lapic =
                (acpi_madt_local_apic_t *)entry;
        
            if (cpu_count < ACPI_MAX_CPUS) {
                cpus[cpu_count].processor_id = lapic->processor_id;
                cpus[cpu_count].apic_id = lapic->apic_id;
                cpus[cpu_count].flags = lapic->flags;

                cpu_count++;
            }
        }

        if (entry->type == 1)
        {
            acpi_madt_io_apic_t *ioapic =
                (acpi_madt_io_apic_t *)entry;
        
            if (io_apic_count < ACPI_MAX_IO_APICS)
            {
                io_apics[io_apic_count].id = ioapic->io_apic_id;
                io_apics[io_apic_count].address = ioapic->io_apic_address;
                io_apics[io_apic_count].gsi_base =
                    ioapic->global_system_interrupt_base;
        
                io_apic_count++;
            }
        } 

        if (entry->type == 2)
        {
            acpi_madt_iso_t *iso =
                (acpi_madt_iso_t *)entry;
        
            if (iso_count < ACPI_MAX_ISOS)
            {
                isos[iso_count].bus = iso->bus;
                isos[iso_count].source = iso->source;
                isos[iso_count].gsi = iso->gsi;
                isos[iso_count].flags = iso->flags;
        
                iso_count++;
            }
        } 

        entry = (acpi_madt_entry_t *)(
            (u8 *)entry + entry->length
        );
    }
}

u32 acpi_io_apic_count(void)
{
    return io_apic_count;
}

const acpi_io_apic_t *acpi_io_apic(u32 index)
{
    if (index >= io_apic_count)
    {
        return NULL;
    }

    return &io_apics[index];
}

u32 acpi_iso_count(void)
{
    return iso_count;
}

const acpi_iso_t *acpi_iso(u32 index)
{
    if (index >= iso_count)
    {
        return NULL;
    }

    return &isos[index];
}

u32 acpi_irq_to_gsi(u8 irq)
{
    for (u32 i = 0; i < iso_count; i++)
    {
        if (isos[i].source == irq)
        {
            return isos[i].gsi;
        }
    }

    return irq;
}

u16 acpi_irq_flags(u8 irq)
{
    for (u32 i = 0; i < iso_count; i++) {
        if (isos[i].source == irq)
            return isos[i].flags;
    }
    return 0;
}
