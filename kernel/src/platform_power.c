#include <platform_power.h>

#include <acpi.h>
#include <io.h>
#include <kprint.h>
#include <mangrove_errors.h>
#include <timer.h>
#include <vmm.h>
#include <stddef.h>

#define ACPI_SCI_EN       (1U << 0)
#define ACPI_SLP_TYP_MASK (7U << 10)
#define ACPI_SLP_EN       (1U << 13)

#define ACPI_ADDRESS_SPACE_SYSTEM_MEMORY 0U
#define ACPI_ADDRESS_SPACE_SYSTEM_IO     1U

typedef struct {
    const acpi_generic_address_t *gas;
    u8 width;
    volatile u8 *memory;
} power_register_t;

static bool power_transition_requested;

static bool power_register_prepare(power_register_t *register_info,
                                   const acpi_generic_address_t *gas)
{
    u64 bytes;

    if (!register_info || !gas || !gas->address || gas->register_bit_offset != 0 ||
        (gas->register_bit_width != 8 && gas->register_bit_width != 16 &&
         gas->register_bit_width != 32))
        return false;
    bytes = gas->register_bit_width / 8U;
    if (gas->access_size != 0 && gas->access_size != 1 &&
        gas->access_size != 2 && gas->access_size != 3)
        return false;

    register_info->gas = gas;
    register_info->width = gas->register_bit_width;
    register_info->memory = NULL;
    if (gas->address_space_id == ACPI_ADDRESS_SPACE_SYSTEM_IO) {
        if (gas->address > 0xffffU)
            return false;
        return true;
    }
    if (gas->address_space_id != ACPI_ADDRESS_SPACE_SYSTEM_MEMORY)
        return false;

    register_info->memory = (volatile u8 *)vmm_map_mmio(
        (phys_addr_t)gas->address, bytes);
    return register_info->memory != NULL &&
           vmm_ioremap_contains((const void *)register_info->memory);
}

static bool power_register_read(const power_register_t *register_info,
                                u32 *value)
{
    const acpi_generic_address_t *gas;

    if (!register_info || !register_info->gas || !value)
        return false;
    gas = register_info->gas;
    if (gas->address_space_id == ACPI_ADDRESS_SPACE_SYSTEM_IO) {
        if (register_info->width == 8)
            *value = inb((u16)gas->address);
        else if (register_info->width == 16)
            *value = inw((u16)gas->address);
        else
            *value = inl((u16)gas->address);
        return true;
    }
    if (register_info->width == 8)
        *value = *(volatile u8 *)register_info->memory;
    else if (register_info->width == 16)
        *value = *(volatile u16 *)register_info->memory;
    else
        *value = *(volatile u32 *)register_info->memory;
    __asm__ volatile("" ::: "memory");
    return true;
}

static bool power_register_write(const power_register_t *register_info,
                                 u32 value)
{
    const acpi_generic_address_t *gas;

    if (!register_info || !register_info->gas)
        return false;
    gas = register_info->gas;
    if (gas->address_space_id == ACPI_ADDRESS_SPACE_SYSTEM_IO) {
        if (register_info->width == 8)
            outb((u16)gas->address, (u8)value);
        else if (register_info->width == 16)
            outw((u16)gas->address, (u16)value);
        else
            outl((u16)gas->address, value);
        return true;
    }
    if (register_info->width == 8)
        *(volatile u8 *)register_info->memory = (u8)value;
    else if (register_info->width == 16)
        *(volatile u16 *)register_info->memory = (u16)value;
    else
        *(volatile u32 *)register_info->memory = value;
    __asm__ volatile("" ::: "memory");
    return true;
}

static bool acpi_enable_if_needed(const power_register_t *pm1a,
                                  const power_register_t *pm1b,
                                  bool has_pm1b,
                                  u32 pm1a_value, u32 pm1b_value)
{
    const acpi_fadt_info_t *fadt = acpi_fadt_get();
    timer_monotonic_deadline_t deadline;

    if ((pm1a_value & ACPI_SCI_EN) &&
        (!has_pm1b || (pm1b_value & ACPI_SCI_EN))) {
        kprint("[ACPI] mode already enabled\n");
        return true;
    }
    if (!fadt || !fadt->smi_command || !fadt->acpi_enable ||
        fadt->smi_command > 0xffffU) {
        kprint("[ACPI] shutdown unavailable: ACPI mode cannot be enabled\n");
        return false;
    }

    kprint("[ACPI] enabling ACPI mode\n");
    outb((u16)fadt->smi_command, fadt->acpi_enable);
    if (!timer_monotonic_deadline_start(&deadline, 100000)) {
        kprint("[ACPI] shutdown unavailable: monotonic deadline unavailable\n");
        return false;
    }
    do {
        u32 a;
        u32 b = 0;
        if (!power_register_read(pm1a, &a) ||
            (has_pm1b && !power_register_read(pm1b, &b)))
            return false;
        if ((a & ACPI_SCI_EN) && (!has_pm1b || (b & ACPI_SCI_EN)))
            return true;
        __asm__ volatile("pause");
    } while (!timer_monotonic_deadline_expired(&deadline));

    kprint("[ACPI] shutdown failed: ACPI mode did not enable\n");
    return false;
}

i64 platform_poweroff(void)
{
    const acpi_fadt_info_t *fadt;
    const acpi_s5_info_t *s5;
    power_register_t pm1a;
    power_register_t pm1b;
    bool has_pm1b;
    u32 pm1a_value;
    u32 pm1b_value = 0;
    u32 pm1a_sleep;
    u32 pm1b_sleep;

    if (power_transition_requested)
        return MG_ERR_BUSY;
    power_transition_requested = true;
    kprint("[ACPI] shutdown requested\n");

    fadt = acpi_fadt_get();
    s5 = acpi_s5_get();
    if (!fadt || !s5) {
        kprint("[ACPI] shutdown unavailable: _S5 not found\n");
        power_transition_requested = false;
        return MG_ERR_UNSUPPORTED;
    }
    if (fadt->hardware_reduced || !fadt->legacy_pm1_control ||
        !power_register_prepare(&pm1a, &fadt->pm1a_control_block)) {
        kprint("[ACPI] shutdown unavailable: PM1 control unsupported\n");
        power_transition_requested = false;
        return MG_ERR_UNSUPPORTED;
    }
    has_pm1b = fadt->pm1b_control_block.address != 0;
    if (has_pm1b && !power_register_prepare(&pm1b,
                                             &fadt->pm1b_control_block)) {
        kprint("[ACPI] shutdown unavailable: PM1b control unsupported\n");
        power_transition_requested = false;
        return MG_ERR_UNSUPPORTED;
    }
    if (!power_register_read(&pm1a, &pm1a_value) ||
        (has_pm1b && !power_register_read(&pm1b, &pm1b_value))) {
        kprint("[ACPI] shutdown unavailable: PM1 control read failed\n");
        power_transition_requested = false;
        return MG_ERR_IO;
    }
    if (!acpi_enable_if_needed(&pm1a, &pm1b, has_pm1b,
                               pm1a_value, pm1b_value)) {
        power_transition_requested = false;
        return MG_ERR_TIMEOUT;
    }

    pm1a_sleep = ((u32)s5->pm1a_sleep_type << 10) & ACPI_SLP_TYP_MASK;
    pm1b_sleep = ((u32)s5->pm1b_sleep_type << 10) & ACPI_SLP_TYP_MASK;
    pm1a_value = (pm1a_value & ~(ACPI_SLP_TYP_MASK | ACPI_SLP_EN)) |
                 pm1a_sleep | ACPI_SLP_EN;
    pm1b_value = (pm1b_value & ~(ACPI_SLP_TYP_MASK | ACPI_SLP_EN)) |
                 pm1b_sleep | ACPI_SLP_EN;

    kprint("[ACPI] entering S5 pm1a=%u%s\n",
           s5->pm1a_sleep_type, has_pm1b ? " pm1b=present" : "");
    __asm__ volatile("cli" ::: "memory");
    if (has_pm1b)
        (void)power_register_write(&pm1b, pm1b_value);
    (void)power_register_write(&pm1a, pm1a_value);

    /* SLP_EN is the terminal hardware request.  If firmware does not power
     * down, do not return to a userspace process after the request. */
    for (;;) {
        __asm__ volatile("hlt");
    }
}

i64 platform_reboot(void)
{
    const acpi_fadt_info_t *fadt;
    power_register_t reset;

    if (power_transition_requested)
        return MG_ERR_BUSY;
    power_transition_requested = true;
    kprint("[ACPI] reboot requested\n");

    fadt = acpi_fadt_get();
    if (!fadt || !acpi_fadt_has_reset()) {
        kprint("[ACPI] reboot unavailable: reset register unsupported\n");
        power_transition_requested = false;
        return MG_ERR_UNSUPPORTED;
    }
    if (!power_register_prepare(&reset, &fadt->reset_register)) {
        kprint("[ACPI] reboot unavailable: reset GAS unsupported\n");
        power_transition_requested = false;
        return MG_ERR_UNSUPPORTED;
    }

    __asm__ volatile("cli" ::: "memory");
    if (!power_register_write(&reset, fadt->reset_value)) {
        kprint("[ACPI] reboot failed: reset write failed\n");
        power_transition_requested = false;
        return MG_ERR_IO;
    }

    /* The reset write is the terminal operation. Returning means firmware did
     * not honor the reset mechanism advertised in the FADT. */
    kprint("[ACPI] reboot failed: reset did not take effect\n");
    power_transition_requested = false;
    return MG_ERR_IO;
}
