#pragma once

#include <types.h>
#include <bootinfo.h>

#define ACPI_ADDRESS_SPACE_SYSTEM_MEMORY 0U
#define ACPI_ADDRESS_SPACE_SYSTEM_IO     1U

bool acpi_present(void);
void *acpi_rsdp(void);

void acpi_init(BOOT_INFO *BootInfo);

typedef struct PACKED
{
    char signature[8];
    u8 checksum;
    char oem_id[6];
    u8 revision;
    u32 rsdt_address;

    u32 length;
    u64 xsdt_address;
    u8 extended_checksum;
    u8 reserved[3];
} acpi_rsdp_t;

typedef struct PACKED
{
    char signature[4];
    u32 length;
    u8 revision;
    u8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
} acpi_sdt_header_t;

typedef struct PACKED
{
    u8 address_space_id;
    u8 register_bit_width;
    u8 register_bit_offset;
    u8 access_size;
    u64 address;
} acpi_generic_address_t;

typedef enum
{
    ACPI_EC_DISCOVERY_NONE = 0,
    ACPI_EC_DISCOVERY_ECDT,
    ACPI_EC_DISCOVERY_NAMESPACE,
} acpi_ec_discovery_t;

typedef struct
{
    /* The ACPI EC specification defines the first _CRS resource as data and
     * the second as status/command.  ECDT names them explicitly; this
     * representation keeps the same semantic order for both sources. */
    acpi_generic_address_t data;
    acpi_generic_address_t control;
    u32 uid;
    u8 gpe;
    bool gpe_valid;
    acpi_ec_discovery_t discovery;
    char path[128];
} acpi_ec_info_t;

typedef struct
{
    u8 revision;
    u32 length;
    u32 firmware_control;
    u64 x_firmware_control;
    u32 dsdt;
    u64 x_dsdt;
    u16 sci_interrupt;
    u32 smi_command;
    u8 acpi_enable;
    u8 acpi_disable;
    acpi_generic_address_t pm1a_event_block;
    acpi_generic_address_t pm1b_event_block;
    acpi_generic_address_t pm1a_control_block;
    acpi_generic_address_t pm1b_control_block;
    acpi_generic_address_t pm_timer_block;
    acpi_generic_address_t gpe0_block;
    acpi_generic_address_t gpe1_block;
    u8 pm1_event_length;
    u8 pm1_control_length;
    u8 pm_timer_length;
    u8 gpe0_length;
    u8 gpe1_length;
    u8 gpe1_base;
    acpi_generic_address_t reset_register;
    u8 reset_value;
    u32 flags;
    bool hardware_reduced;
    bool reset_supported;
    bool legacy_pm1_control;
    bool pm_timer_available;
    bool pm_timer_32bit;
} acpi_fadt_info_t;

typedef struct
{
    u8 pm1a_sleep_type;
    u8 pm1b_sleep_type;
} acpi_s5_info_t;

typedef struct PACKED
{
    acpi_sdt_header_t header;
    u32 event_timer_block_id;
    acpi_generic_address_t base_address;
    u8 hpet_number;
    u16 minimum_clock_tick;
    u8 page_protection;
} acpi_hpet_t;

typedef struct PACKED
{
    acpi_sdt_header_t header;

    u32 local_apic_address;
    u32 flags;
} acpi_madt_t;

acpi_madt_t *acpi_madt(void);
const acpi_hpet_t *acpi_hpet(void);
bool acpi_fadt_available(void);
const acpi_fadt_info_t *acpi_fadt_get(void);
bool acpi_fadt_has_reset(void);
bool acpi_fadt_has_pm_timer(void);
bool acpi_s5_available(void);
const acpi_s5_info_t *acpi_s5_get(void);
bool acpi_ec_info_available(void);
const acpi_ec_info_t *acpi_ec_info_get(void);
const acpi_sdt_header_t *acpi_dsdt(void);
u32 acpi_definition_table_count(void);
const acpi_sdt_header_t *acpi_definition_table(u32 index);

/* The SCI handler only records firmware-enabled GPE work.  AML methods and
 * platform policy are evaluated by the normal-context service owner. */
bool acpi_events_prepare(void);
bool acpi_events_start(void);
void acpi_sci_interrupt(void);

typedef struct PACKED
{
    u8 type;
    u8 length;
} acpi_madt_entry_t;

typedef struct PACKED
{
    acpi_madt_entry_t header;

    u8 processor_id;
    u8 apic_id;
    u32 flags;
} acpi_madt_local_apic_t;

typedef struct PACKED
{
    acpi_madt_entry_t header;

    u8 io_apic_id;
    u8 reserved;
    u32 io_apic_address;
    u32 global_system_interrupt_base;
} acpi_madt_io_apic_t;

typedef struct PACKED
{
    acpi_madt_entry_t header;

    u8 bus;
    u8 source;
    u32 gsi;
    u16 flags;
} acpi_madt_iso_t;

typedef struct
{
    u8 processor_id;
    u8 apic_id;
    u32 flags;
} acpi_cpu_t;

typedef struct
{
    u8 id;
    u32 address;
    u32 gsi_base;
} acpi_io_apic_t;

typedef struct
{
    u8 bus;
    u8 source;
    u32 gsi;
    u16 flags;
} acpi_iso_t;

#define ACPI_IRQ_POLARITY_ACTIVE_LOW 0x0003U
#define ACPI_IRQ_TRIGGER_LEVEL       0x000CU
#define ACPI_IRQ_FLAGS_ACTIVE_LOW_LEVEL \
    (ACPI_IRQ_POLARITY_ACTIVE_LOW | ACPI_IRQ_TRIGGER_LEVEL)

u32 acpi_io_apic_count(void);
const acpi_io_apic_t *acpi_io_apic(u32 index);

u32 acpi_iso_count(void);
const acpi_iso_t *acpi_iso(u32 index);

u32 acpi_cpu_count(void);
const acpi_cpu_t *acpi_cpu(u32 index);

u32 acpi_irq_to_gsi(u8 irq);
u16 acpi_irq_flags(u8 irq);
