#include <ec.h>

#include <acpi.h>
#include <aml.h>
#include <io.h>
#include <kprint.h>
#include <timer.h>
#include <vmm.h>

#include <stddef.h>

#define EC_STATUS_OBF                  (1U << 0)
#define EC_STATUS_IBF                  (1U << 1)
#define EC_COMMAND_READ                0x80U
#define EC_COMMAND_WRITE               0x81U
#define EC_TRANSACTION_TIMEOUT_US      500000ULL

typedef struct
{
    acpi_generic_address_t gas;
    volatile u8 *memory;
    bool valid;
} ec_register_t;

typedef struct
{
    bool available;
    ec_register_t data;
    ec_register_t control;
    volatile bool transaction_locked;
} ec_controller_t;

static ec_controller_t controller;

static bool ec_register_prepare(ec_register_t *register_info,
                                const acpi_generic_address_t *gas)
{
    if (!register_info || !gas || !gas->address ||
        gas->register_bit_width != 8 || gas->register_bit_offset != 0 ||
        (gas->access_size != 0 && gas->access_size != 1) ||
        (gas->address_space_id != ACPI_ADDRESS_SPACE_SYSTEM_IO &&
         gas->address_space_id != ACPI_ADDRESS_SPACE_SYSTEM_MEMORY))
        return false;

    register_info->gas = *gas;
    register_info->memory = NULL;
    if (gas->address_space_id == ACPI_ADDRESS_SPACE_SYSTEM_IO) {
        if (gas->address > 0xffffU)
            return false;
    } else {
        register_info->memory = (volatile u8 *)vmm_map_mmio(
            (phys_addr_t)gas->address, 1);
        if (!register_info->memory ||
            !vmm_ioremap_contains((const void *)register_info->memory))
            return false;
    }
    register_info->valid = true;
    return true;
}

static u8 ec_register_read(const ec_register_t *register_info)
{
    u8 value;

    if (register_info->gas.address_space_id ==
        ACPI_ADDRESS_SPACE_SYSTEM_IO)
        return inb((u16)register_info->gas.address);
    value = *register_info->memory;
    __asm__ volatile("" ::: "memory");
    return value;
}

static void ec_register_write(const ec_register_t *register_info, u8 value)
{
    if (register_info->gas.address_space_id ==
        ACPI_ADDRESS_SPACE_SYSTEM_IO) {
        outb((u16)register_info->gas.address, value);
        return;
    }
    __asm__ volatile("" ::: "memory");
    *register_info->memory = value;
    __asm__ volatile("" ::: "memory");
}

static bool ec_lock(timer_monotonic_deadline_t *deadline)
{
    u64 flags;

    if (!deadline || !timer_monotonic_deadline_start(
            deadline, EC_TRANSACTION_TIMEOUT_US))
        return false;

    for (;;) {
        /* Only the ownership bit is protected with IF clear.  The EC wait
         * itself leaves interrupts enabled so timer/SCI delivery is not
         * blocked by a firmware transaction. */
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
        if (!__atomic_test_and_set(&controller.transaction_locked,
                                   __ATOMIC_ACQUIRE)) {
            if (flags & (1ULL << 9))
                __asm__ volatile("sti" ::: "memory");
            return true;
        }
        if (flags & (1ULL << 9))
            __asm__ volatile("sti" ::: "memory");
        if (timer_monotonic_deadline_expired(deadline))
            return false;
        __asm__ volatile("pause");
    }
}

static void ec_unlock(void)
{
    __atomic_clear(&controller.transaction_locked, __ATOMIC_RELEASE);
}

static bool ec_wait_status(u8 mask, bool set,
                           timer_monotonic_deadline_t *deadline)
{
    while (((ec_register_read(&controller.control) & mask) != 0) != set) {
        if (timer_monotonic_deadline_expired(deadline))
            return false;
        __asm__ volatile("pause");
    }
    return true;
}

static bool ec_status_locked(u8 *status)
{
    if (!status || !controller.available || !controller.control.valid)
        return false;
    *status = ec_register_read(&controller.control);
    return true;
}

static bool ec_read_locked(u8 offset, u8 *value,
                           timer_monotonic_deadline_t *deadline)
{
    u8 status;

    if (!value || !ec_status_locked(&status) || (status & EC_STATUS_OBF))
        return false;
    if (!ec_wait_status(EC_STATUS_IBF, false, deadline))
        return false;
    ec_register_write(&controller.control, EC_COMMAND_READ);
    if (!ec_wait_status(EC_STATUS_IBF, false, deadline))
        return false;
    ec_register_write(&controller.data, offset);
    if (!ec_wait_status(EC_STATUS_OBF, true, deadline))
        return false;
    *value = ec_register_read(&controller.data);
    return true;
}

static bool ec_write_locked(u8 offset, u8 value,
                            timer_monotonic_deadline_t *deadline)
{
    u8 status;

    if (!ec_status_locked(&status) || (status & EC_STATUS_OBF) ||
        !ec_wait_status(EC_STATUS_IBF, false, deadline))
        return false;
    ec_register_write(&controller.control, EC_COMMAND_WRITE);
    if (!ec_wait_status(EC_STATUS_IBF, false, deadline))
        return false;
    ec_register_write(&controller.data, offset);
    if (!ec_wait_status(EC_STATUS_IBF, false, deadline))
        return false;
    ec_register_write(&controller.data, value);
    return ec_wait_status(EC_STATUS_IBF, false, deadline);
}

bool ec_init(void)
{
    const acpi_ec_info_t *info;
    acpi_ec_info_t evaluated_info;
    aml_handle_t aml_device = AML_HANDLE_INVALID;
    timer_monotonic_deadline_t deadline;
    u8 status;
    const char *source;
    bool namespace_ec_available;

    controller = (ec_controller_t){0};
    (void)aml_namespace_init();
    info = acpi_ec_info_get();
    namespace_ec_available = aml_discover_ec(&evaluated_info, &aml_device);
    if (!namespace_ec_available && info &&
        info->discovery == ACPI_EC_DISCOVERY_ECDT) {
        aml_device = AML_HANDLE_INVALID;
        namespace_ec_available =
            aml_find_device_by_hid("PNP0C09", &aml_device);
    }
    if (!info && namespace_ec_available)
        info = &evaluated_info;
    if (!info) {
        KERNEL_BOOT_DEBUG_LOG("[ACPI] EC unavailable\n");
        return false;
    }
    if (!timer_monotonic_ready()) {
        KERNEL_BOOT_DEBUG_LOG(
            "[ACPI] EC unavailable: monotonic deadline unavailable\n");
        return false;
    }
    if (!ec_register_prepare(&controller.data, &info->data) ||
        !ec_register_prepare(&controller.control, &info->control)) {
        KERNEL_BOOT_DEBUG_LOG(
            "[ACPI] EC unavailable: unsupported register space\n");
        return false;
    }
    if (controller.data.gas.address_space_id ==
            controller.control.gas.address_space_id &&
        controller.data.gas.address == controller.control.gas.address) {
        KERNEL_BOOT_DEBUG_LOG(
            "[ACPI] EC unavailable: duplicate data/control register\n");
        return false;
    }
    controller.available = true;
    if (!ec_lock(&deadline)) {
        controller.available = false;
        KERNEL_BOOT_DEBUG_LOG("[ACPI] EC unavailable: status read timeout\n");
        return false;
    }
    if (!ec_status_locked(&status)) {
        ec_unlock();
        controller.available = false;
        KERNEL_BOOT_DEBUG_LOG("[ACPI] EC unavailable: status read timeout\n");
        return false;
    }
    ec_unlock();
    if (namespace_ec_available && aml_device != AML_HANDLE_INVALID &&
        !aml_connect_ec_regions(aml_device)) {
        controller.available = false;
        KERNEL_BOOT_DEBUG_LOG(
            "[ACPI] EC unavailable: AML region connection failed\n");
        return false;
    }
    source = info->discovery == ACPI_EC_DISCOVERY_ECDT ? "ECDT" :
             info->discovery == ACPI_EC_DISCOVERY_NAMESPACE ? "namespace" :
             "unknown";
    KERNEL_BOOT_DEBUG_LOG("[ACPI] EC ready\n");
#ifdef ACPI_POWER_DEBUG
    if (info->gpe_valid) {
        kprint("[ACPI] EC available source=%s path=%s data=0x%llx cmd=0x%llx gpe=%u\n",
               source, info->path[0] ? info->path : "unknown",
               info->data.address, info->control.address, info->gpe);
    } else {
        kprint("[ACPI] EC available source=%s path=%s data=0x%llx cmd=0x%llx gpe=unknown\n",
               source, info->path[0] ? info->path : "unknown",
               info->data.address, info->control.address);
    }
#else
    (void)source;
#endif
    return true;
}

bool ec_available(void)
{
    return controller.available;
}

bool ec_status(u8 *status)
{
    timer_monotonic_deadline_t deadline;
    bool result;

    if (!status || !ec_lock(&deadline))
        return false;
    result = ec_status_locked(status);
    ec_unlock();
    return result;
}

bool ec_read(u8 offset, u8 *value)
{
    timer_monotonic_deadline_t deadline;
    bool result;

    if (!value || !ec_lock(&deadline))
        return false;
    result = ec_read_locked(offset, value, &deadline);
    ec_unlock();
    return result;
}

bool ec_write(u8 offset, u8 value)
{
    timer_monotonic_deadline_t deadline;
    bool result;

    if (!ec_lock(&deadline))
        return false;
    result = ec_write_locked(offset, value, &deadline);
    ec_unlock();
    return result;
}
