#include <platform_thermal.h>

#include <kprint.h>
#include <pci.h>
#include <stddef.h>

#define AMD_VENDOR_ID                 0x1022U
#define AMD_FAMILY_19H                0x19U
#define AMD_CEZANNE_MODEL_FIRST       0x50U
#define AMD_CEZANNE_MODEL_LAST        0x5fU
#define AMD_CEZANNE_DF_F3_DEVICE_ID   0x166dU
#define AMD_CEZANNE_ROOT_DEVICE_ID    0x1630U

/* AMD Family 19h exposes the System Management Network through the matching
 * root-complex function's indirect PCI configuration aperture. */
#define AMD_SMN_ADDRESS_REGISTER      0x60U
#define AMD_SMN_DATA_REGISTER         0x64U
#define AMD_REPORTED_TEMPERATURE      0x00059800U

#define AMD_TEMPERATURE_SHIFT         21U
#define AMD_TEMPERATURE_MASK          0x7ffU
#define AMD_TEMPERATURE_RANGE_SELECT  (1U << 19)
#define AMD_TEMPERATURE_TJ_SELECT     (3U << 16)
#define AMD_TEMPERATURE_STEP_MC       125
#define AMD_TEMPERATURE_OFFSET_MC     49000

static bool thermal_initialized;
static const pci_device_t *thermal_df_device;
static const pci_device_t *thermal_smn_device;

static void read_cpuid(u32 leaf, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf));
}

static u32 processor_family(void)
{
    u32 eax;
    u32 ebx;
    u32 ecx;
    u32 edx;
    u32 family;

    read_cpuid(1U, &eax, &ebx, &ecx, &edx);
    family = (eax >> 8) & 0xfU;
    if (family == 0xfU)
        family += (eax >> 20) & 0xffU;
    return family;
}

static u32 processor_model(void)
{
    u32 eax;
    u32 ebx;
    u32 ecx;
    u32 edx;
    u32 family;
    u32 model;

    read_cpuid(1U, &eax, &ebx, &ecx, &edx);
    family = (eax >> 8) & 0xfU;
    model = (eax >> 4) & 0xfU;
    if (family == 0x6U || family == 0xfU)
        model |= ((eax >> 16) & 0xfU) << 4;
    return model;
}

#ifdef PLATFORM_THERMAL_DEBUG
static u32 processor_stepping(void)
{
    u32 eax;
    u32 ebx;
    u32 ecx;
    u32 edx;

    read_cpuid(1U, &eax, &ebx, &ecx, &edx);
    return eax & 0xfU;
}
#endif

static bool smn_read(u32 address, u32 *value)
{
    u64 flags;

    if (!thermal_smn_device || !value)
        return false;

    /* The SMN address/data pair is a single transaction.  No current IRQ
     * handler accesses this aperture, but keep the pair indivisible so a
     * future interrupt-time user cannot change the selected address between
     * the two configuration cycles. */
    __asm__ volatile("pushfq; popq %0; cli"
                     : "=r"(flags) :: "memory");
    pci_write_config32(thermal_smn_device, AMD_SMN_ADDRESS_REGISTER,
                       address);
    /* PCI configuration writes and reads are ordered I/O operations.  This
     * compiler barrier keeps the target selection visibly before its data
     * read without adding a timing delay. */
    __asm__ volatile("" ::: "memory");
    *value = pci_read_config32(thermal_smn_device,
                               AMD_SMN_DATA_REGISTER);
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
#ifdef PLATFORM_THERMAL_DEBUG
    kprint("[THERMAL] smn-select pci=%u:%u.%u addr=%08x data=%08x\n",
           thermal_smn_device->bus, thermal_smn_device->device,
           thermal_smn_device->function, address, *value);
#endif
    return *value != 0xffffffffU;
}

bool platform_thermal_initialize(void)
{
    u32 family;
    u32 model;
#ifdef PLATFORM_THERMAL_DEBUG
    u32 stepping;
#endif

    if (thermal_initialized)
        return thermal_smn_device != NULL;
    thermal_initialized = true;

    family = processor_family();
    model = processor_model();
#ifdef PLATFORM_THERMAL_DEBUG
    stepping = processor_stepping();
#endif
    if (family != AMD_FAMILY_19H || model < AMD_CEZANNE_MODEL_FIRST ||
        model > AMD_CEZANNE_MODEL_LAST) {
#ifdef PLATFORM_THERMAL_DEBUG
        kprint("[THERMAL] unavailable: cpu=%x:%x:%x unsupported\n", family,
               model, stepping);
#endif
        return false;
    }

    for (u32 index = 0; index < pci_get_device_count(); index++) {
        const pci_device_t *device = pci_get_device(index);

        if (!device || device->vendor_id != AMD_VENDOR_ID ||
            device->device_id != AMD_CEZANNE_DF_F3_DEVICE_ID ||
            device->class_code != 0x06U || device->subclass != 0x00U)
            continue;
        thermal_df_device = device;
        break;
    }

    if (!thermal_df_device) {
#ifdef PLATFORM_THERMAL_DEBUG
        kprint("[THERMAL] unavailable: Cezanne Data Fabric F3 absent\n");
#endif
        return false;
    }

    /* Data Fabric Function 3 identifies the local fabric node.  The matching
     * root-complex function owns the SMN 0x60/0x64 address/data aperture. */
    for (u32 index = 0; index < pci_get_device_count(); index++) {
        const pci_device_t *device = pci_get_device(index);

        if (!device || device->vendor_id != AMD_VENDOR_ID ||
            device->device_id != AMD_CEZANNE_ROOT_DEVICE_ID ||
            device->class_code != 0x06U || device->subclass != 0x00U ||
            device->bus != thermal_df_device->bus || device->device != 0U ||
            device->function != 0U)
            continue;
        thermal_smn_device = device;
        break;
    }

    if (!thermal_smn_device) {
#ifdef PLATFORM_THERMAL_DEBUG
        kprint("[THERMAL] unavailable: Cezanne SMN root absent\n");
#endif
        return false;
    }

#ifdef PLATFORM_THERMAL_DEBUG
    kprint("[THERMAL] source=AMD Tctl cpu=%x:%x:%x df=%u:%u.%u smn=%u:%u.%u\n",
           family, model, stepping, thermal_df_device->bus, thermal_df_device->device,
           thermal_df_device->function,
           thermal_smn_device->bus, thermal_smn_device->device,
           thermal_smn_device->function);
#endif
    return true;
}

bool platform_thermal_read(platform_temperature_t *temperature)
{
    u32 register_value;
    u32 raw;
    i32 millidegrees;

    if (!temperature)
        return false;
    *temperature = (platform_temperature_t){0};
    if (!platform_thermal_initialize())
        return false;
    if (!smn_read(AMD_REPORTED_TEMPERATURE, &register_value)) {
#ifdef PLATFORM_THERMAL_DEBUG
        kprint("[THERMAL] unavailable: SMN data read failed\n");
#endif
        return false;
    }

    /* The supported Tctl encoding needs a non-zero current-temperature
     * field.  A zero data word is not a trustworthy live sample. */
    if (register_value == 0U) {
#ifdef PLATFORM_THERMAL_DEBUG
        kprint("[THERMAL] reject: zero SMN temperature register\n");
#endif
        return false;
    }

    raw = (register_value >> AMD_TEMPERATURE_SHIFT) &
          AMD_TEMPERATURE_MASK;
    if (raw == 0U) {
#ifdef PLATFORM_THERMAL_DEBUG
        kprint("[THERMAL] reject: zero temperature field reg=%08x\n",
               register_value);
#endif
        return false;
    }
    millidegrees = (i32)(raw * AMD_TEMPERATURE_STEP_MC);
    if ((register_value & AMD_TEMPERATURE_RANGE_SELECT) ||
        (register_value & AMD_TEMPERATURE_TJ_SELECT) ==
            AMD_TEMPERATURE_TJ_SELECT)
        millidegrees -= AMD_TEMPERATURE_OFFSET_MC;

    /* Reject an inaccessible or nonsensical sample rather than exposing a
     * fabricated platform value.  This does not clamp valid operating data. */
    if (millidegrees < -40000 || millidegrees > 150000) {
#ifdef PLATFORM_THERMAL_DEBUG
        kprint("[THERMAL] reject: decoded=%d mC raw=%u reg=%08x\n",
               millidegrees, raw, register_value);
#endif
        return false;
    }

    temperature->available = true;
    temperature->sensor = PLATFORM_TEMPERATURE_SENSOR_CPU_PACKAGE;
    temperature->millidegrees_celsius = millidegrees;
#ifdef PLATFORM_THERMAL_DEBUG
    kprint("[THERMAL] decode reg=%08x field=%u value=%d mC\n",
           register_value, raw, millidegrees);
#endif
    return true;
}
