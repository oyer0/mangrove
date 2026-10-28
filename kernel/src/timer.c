/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <timer.h>
#include <preemption_count.h>
#include <acpi.h>
#include <irq.h>
#include <drivers/input/keyboard.h>
#include <terminal.h>
#include <scheduler.h>
#include <kprint.h>
#include <vmm.h>
#include <lapic.h>
#include <input_trace.h>
#include <clocksource.h>
#include <io.h>
#include <spinlock.h>
#if INPUT_TRACE_DIAGNOSTICS
#include <spinlock.h>
#endif

#include <stddef.h>
#include <string.h>

#define HPET_MMIO_SIZE                    0x400U
#define HPET_GENERAL_CAPABILITIES         0x000U
#define HPET_GENERAL_CONFIGURATION        0x010U
#define HPET_MAIN_COUNTER                 0x0F0U
#define HPET_CONFIGURATION_ENABLE         (1ULL << 0)
#define HPET_CONFIGURATION_LEGACY_ROUTE   (1ULL << 1)
#define HPET_CAPABILITY_COUNTER_64        (1ULL << 13)
#define HPET_MAX_PERIOD_FS                100000000ULL
#define FEMTOSECONDS_PER_MICROSECOND      1000000000ULL
#define PM_TIMER_PERIOD_FS                279365115ULL
#define PM_TIMER_REGISTER_BYTES           4U
#define PM_TIMER_MMIO_BYTES               4096U
#define ACPI_ADDRESS_SPACE_SYSTEM_MEMORY  0U
#define ACPI_ADDRESS_SPACE_SYSTEM_IO      1U

static u64 ticks = 0;
static u64 preemptions = 0;
static volatile u8 *monotonic_hpet;
static volatile u8 *monotonic_pm_timer;
static u64 monotonic_pm_timer_address;
static u8 monotonic_pm_timer_space;
static u64 monotonic_counter_mask;
static u64 monotonic_period_fs;
static u64 monotonic_counter_origin;
static u64 monotonic_extended_origin;
static u64 monotonic_base_ms;
static volatile u64 monotonic_published_ms;
static clocksource_id_t monotonic_source = CLOCKSOURCE_PIT;
static clocksource_counter_state_t monotonic_counter_state;
static spinlock_t monotonic_counter_lock;
static bool monotonic_is_ready;

#if INPUT_TRACE_DIAGNOSTICS
#define INPUT_TRACE_CAPACITY 16384U
typedef struct {
    u64 counter, uptime_ms;
    u32 kind, key, first, second;
} input_trace_event_t;
/* Read by the diagnostic runner only while all vCPUs are stopped. The header
 * is also sampled before input to obtain the HPET clock calibration address. */
struct {
    u64 magic, count, dropped, counter_physical, period_fs, counter_mask;
    u64 capacity, event_bytes;
    input_trace_event_t events[INPUT_TRACE_CAPACITY];
} pith_input_trace = {
    .magic = 0x3145434152544E49ULL, .capacity = INPUT_TRACE_CAPACITY,
    .event_bytes = sizeof(input_trace_event_t)
};
static spinlock_t input_trace_lock;
static bool input_trace_raw_active;
#endif

static u64 hpet_read64(u32 offset)
{
    u64 value = *(volatile u64 *)(monotonic_hpet + offset);
    __asm__ volatile("" ::: "memory");
    return value;
}

static void hpet_write64(u32 offset, u64 value)
{
    __asm__ volatile("" ::: "memory");
    *(volatile u64 *)(monotonic_hpet + offset) = value;
    __asm__ volatile("" ::: "memory");
}

static u32 timer_read_pm_counter(void)
{
    if (monotonic_pm_timer_space == ACPI_ADDRESS_SPACE_SYSTEM_IO)
        return inl((u16)monotonic_pm_timer_address);
    __asm__ volatile("" ::: "memory");
    u32 value = *(volatile u32 *)monotonic_pm_timer;
    __asm__ volatile("" ::: "memory");
    return value;
}

static u64 timer_monotonic_counter(void)
{
    if (monotonic_source == CLOCKSOURCE_HPET) {
        if (monotonic_counter_mask == 0xFFFFFFFFULL) {
            u32 value = *(volatile u32 *)(monotonic_hpet + HPET_MAIN_COUNTER);
            __asm__ volatile("" ::: "memory");
            return value;
        }
        return hpet_read64(HPET_MAIN_COUNTER);
    }
    if (monotonic_source == CLOCKSOURCE_ACPI_PM_TIMER)
        return (u64)timer_read_pm_counter() & monotonic_counter_mask;
    return timer_ticks();
}

static bool timer_prepare_hpet(u64 *out_period_fs, u64 *out_mask)
{
    const acpi_hpet_t *table = acpi_hpet();
    u64 capabilities;

    if (!out_period_fs || !out_mask || !table ||
        table->base_address.address_space_id !=
            ACPI_ADDRESS_SPACE_SYSTEM_MEMORY ||
        table->base_address.register_bit_offset != 0 ||
        !table->base_address.address ||
        (table->base_address.address & (HPET_MMIO_SIZE - 1U)))
        return false;

    monotonic_hpet = (volatile u8 *)vmm_map_mmio(
        (phys_addr_t)table->base_address.address, HPET_MMIO_SIZE);
    if (!monotonic_hpet ||
        !vmm_ioremap_contains((const void *)monotonic_hpet)) {
        monotonic_hpet = NULL;
        return false;
    }

    capabilities = hpet_read64(HPET_GENERAL_CAPABILITIES);
    *out_period_fs = capabilities >> 32;
    if (!*out_period_fs || *out_period_fs > HPET_MAX_PERIOD_FS) {
        monotonic_hpet = NULL;
        return false;
    }
    *out_mask = (capabilities & HPET_CAPABILITY_COUNTER_64) ?
        ~(u64)0 : 0xFFFFFFFFULL;

    return true;
}

static bool timer_enable_hpet_counter(void)
{
    u64 configuration;
    if (!monotonic_hpet)
        return false;
    configuration = hpet_read64(HPET_GENERAL_CONFIGURATION);
    /* This kernel reads only the free-running counter. Preserve the firmware
     * legacy-route setting because changing timer routing is not required. */
    hpet_write64(HPET_GENERAL_CONFIGURATION,
                 configuration | HPET_CONFIGURATION_ENABLE);
    configuration = hpet_read64(HPET_GENERAL_CONFIGURATION);
    if (!(configuration & HPET_CONFIGURATION_ENABLE)) {
        monotonic_hpet = NULL;
        return false;
    }
    return true;
}

static bool timer_prepare_pm_timer(u64 *out_period_fs, u64 *out_mask,
                                   bool *out_32bit)
{
    const acpi_fadt_info_t *fadt = acpi_fadt_get();
    const acpi_generic_address_t *gas;

    if (!out_period_fs || !out_mask || !out_32bit || !fadt ||
        !fadt->pm_timer_available)
        return false;
    gas = &fadt->pm_timer_block;
    if (gas->register_bit_offset != 0 || gas->register_bit_width < 24U ||
        gas->register_bit_width > 32U)
        return false;

    if (gas->address_space_id == ACPI_ADDRESS_SPACE_SYSTEM_IO) {
        if (gas->address > 0xFFFFU - (PM_TIMER_REGISTER_BYTES - 1U))
            return false;
        monotonic_pm_timer = NULL;
    } else if (gas->address_space_id == ACPI_ADDRESS_SPACE_SYSTEM_MEMORY) {
        u64 page = gas->address & ~(u64)0xFFFU;
        u64 offset = gas->address - page;
        if ((gas->address & (PM_TIMER_REGISTER_BYTES - 1U)) != 0 ||
            offset > PM_TIMER_MMIO_BYTES - PM_TIMER_REGISTER_BYTES)
            return false;
        void *mapping = vmm_map_mmio((phys_addr_t)page,
                                     PM_TIMER_MMIO_BYTES);
        if (!mapping || !vmm_ioremap_contains(mapping))
            return false;
        monotonic_pm_timer = (volatile u8 *)mapping + offset;
    } else {
        return false;
    }

    monotonic_pm_timer_address = gas->address;
    monotonic_pm_timer_space = gas->address_space_id;
    *out_period_fs = PM_TIMER_PERIOD_FS;
    *out_32bit = fadt->pm_timer_32bit;
    *out_mask = *out_32bit ? 0xFFFFFFFFULL : 0x00FFFFFFULL;
    (void)timer_read_pm_counter();
    return true;
}

#if INPUT_TRACE_DIAGNOSTICS
void input_trace_raw(bool enabled)
{
    __atomic_store_n(&input_trace_raw_active, enabled, __ATOMIC_RELEASE);
}

void input_trace_record(u32 kind, u32 key, u32 first, u32 second)
{
    if (!monotonic_is_ready) return;
    if (kind >= INPUT_TRACE_FLUSH_BEGIN &&
        !__atomic_load_n(&input_trace_raw_active, __ATOMIC_ACQUIRE)) return;
    if (kind <= INPUT_TRACE_CANCEL && key != 0x0BU && key != 0x0DU &&
        key != 0x0EU && key != 0x0FU) return;
    u64 flags = spin_lock_irqsave(&input_trace_lock);
    u64 index = pith_input_trace.count;
    if (index == INPUT_TRACE_CAPACITY) {
        pith_input_trace.dropped++;
    } else {
        pith_input_trace.events[index] = (input_trace_event_t) {
            timer_monotonic_counter(), timer_uptime_ms(),
            kind, key, first, second
        };
        pith_input_trace.count = index + 1U;
    }
    spin_unlock_irqrestore(&input_trace_lock, flags);
}
#endif

void timer_init(void)
{
    (void)irq_register_permanent_vector(IRQ_VECTOR_PIT, timer_interrupt);
    (void)irq_register_permanent_vector(IRQ_VECTOR_LAPIC_TIMER,
                                       timer_lapic_interrupt);
}

bool timer_monotonic_init(void)
{
    u64 hpet_period_fs = 0, hpet_mask = 0;
    u64 pm_period_fs = 0, pm_mask = 0;
    bool pm_32bit = false;
    bool hpet_valid;
    bool pm_valid;

    if (monotonic_is_ready)
        return true;

    monotonic_is_ready = false;
    monotonic_hpet = NULL;
    monotonic_pm_timer = NULL;
    monotonic_pm_timer_address = 0;
    monotonic_pm_timer_space = 0;
    monotonic_source = CLOCKSOURCE_PIT;
    monotonic_counter_mask = 0;
    monotonic_period_fs = 0;
    hpet_valid = timer_prepare_hpet(&hpet_period_fs, &hpet_mask);
    pm_valid = !(hpet_valid && hpet_mask == ~(u64)0) &&
        timer_prepare_pm_timer(&pm_period_fs, &pm_mask, &pm_32bit);
    monotonic_source = clocksource_select(hpet_valid,
        hpet_valid && hpet_mask == ~(u64)0, pm_valid, pm_32bit);
    if (monotonic_source == CLOCKSOURCE_HPET) {
        monotonic_period_fs = hpet_period_fs;
        monotonic_counter_mask = hpet_mask;
        if (!timer_enable_hpet_counter()) {
            if (!pm_valid)
                pm_valid = timer_prepare_pm_timer(
                    &pm_period_fs, &pm_mask, &pm_32bit);
            if (!pm_valid) {
                monotonic_source = CLOCKSOURCE_PIT;
                monotonic_counter_mask = 0;
                monotonic_period_fs = 0;
                return false;
            }
            monotonic_source = CLOCKSOURCE_ACPI_PM_TIMER;
            monotonic_period_fs = pm_period_fs;
            monotonic_counter_mask = pm_mask;
        }
    } else if (monotonic_source == CLOCKSOURCE_ACPI_PM_TIMER) {
        monotonic_period_fs = pm_period_fs;
        monotonic_counter_mask = pm_mask;
    } else {
        return false;
    }

    monotonic_counter_origin = timer_monotonic_counter();
    memset(&monotonic_counter_state, 0, sizeof(monotonic_counter_state));
    if (monotonic_counter_mask != ~(u64)0) {
        if (!clocksource_extend_counter(&monotonic_counter_state,
                monotonic_counter_origin, monotonic_counter_mask,
                &monotonic_extended_origin)) {
            monotonic_source = CLOCKSOURCE_PIT;
            monotonic_counter_mask = 0;
            monotonic_period_fs = 0;
            return false;
        }
    } else {
        monotonic_extended_origin = monotonic_counter_origin;
    }
    monotonic_base_ms = timer_ticks();
    __atomic_store_n(&monotonic_published_ms, monotonic_base_ms,
                     __ATOMIC_RELEASE);
    monotonic_is_ready = true;
#if INPUT_TRACE_DIAGNOSTICS
    const acpi_hpet_t *table = acpi_hpet();
    pith_input_trace.counter_physical = table ?
        table->base_address.address + HPET_MAIN_COUNTER : 0;
    pith_input_trace.period_fs = monotonic_period_fs;
    pith_input_trace.counter_mask = monotonic_counter_mask;
#endif
    return true;
}

bool timer_monotonic_ready(void)
{
    return monotonic_is_ready;
}

bool timer_monotonic_deadline_start(timer_monotonic_deadline_t *deadline,
                                    u64 microseconds)
{
    u64 femtoseconds;
    u64 duration;
    u64 maximum_duration;

    if (!deadline || !monotonic_is_ready || !microseconds ||
        microseconds > ~(u64)0 / FEMTOSECONDS_PER_MICROSECOND) {
        return false;
    }

    femtoseconds = microseconds * FEMTOSECONDS_PER_MICROSECOND;
    duration = femtoseconds / monotonic_period_fs;
    if (femtoseconds % monotonic_period_fs)
        duration++;
    if (!duration)
        duration = 1;

    /* Modular elapsed-time arithmetic remains unambiguous across one counter
     * wrap when each bounded interval is shorter than half the counter range. */
    maximum_duration = monotonic_counter_mask >> 1;
    if (duration > maximum_duration)
        return false;

    deadline->start_ticks = timer_monotonic_counter();
    deadline->duration_ticks = duration;
    deadline->expired = false;
    return true;
}

bool timer_monotonic_deadline_expired(timer_monotonic_deadline_t *deadline)
{
    u64 elapsed;

    if (!deadline || !monotonic_is_ready)
        return true;
    if (deadline->expired)
        return true;

    elapsed = (timer_monotonic_counter() - deadline->start_ticks) &
              monotonic_counter_mask;
    if (elapsed >= deadline->duration_ticks)
        deadline->expired = true;
    return deadline->expired;
}

bool timer_monotonic_delay_us(u64 microseconds)
{
    timer_monotonic_deadline_t deadline;

    if (!timer_monotonic_deadline_start(&deadline, microseconds))
        return false;
    while (!timer_monotonic_deadline_expired(&deadline))
        __asm__ volatile("pause");
    return true;
}

void timer_interrupt(struct cpu_registers *regs)
{
    (void)regs;

    __atomic_fetch_add(&ticks, 1, __ATOMIC_RELAXED);
    /* Keep narrow hardware counters extended even when clients are idle. */
    if (monotonic_is_ready && monotonic_counter_mask != ~(u64)0)
        (void)timer_uptime_ms();
    terminal_cursor_blink_timer_tick();
    keyboard_update();

    (void)scheduler_global_tick();
    if (!lapic_timer_active() && scheduler_timer_tick()) {
        /* Scheduling is deferred until irq_handler has sent EOI. */
        preemption_count_increment(&preemptions);
    }
}

void timer_lapic_interrupt(struct cpu_registers *regs)
{
    (void)regs;
    if (scheduler_timer_tick()) {
        /* The common IRQ exit path performs deferred preemption after EOI. */
        preemption_count_increment(&preemptions);
    }
}

u64 timer_ticks(void)
{
    return __atomic_load_n(&ticks, __ATOMIC_RELAXED);
}

u64 timer_uptime_ms(void)
{
    u64 raw;
    u64 extended;
    u64 delta;
    u64 elapsed_ms;
    u64 candidate;

    if (!monotonic_is_ready)
        return __atomic_load_n(&ticks, __ATOMIC_RELAXED);

    if (monotonic_counter_mask == ~(u64)0) {
        raw = timer_monotonic_counter();
        delta = raw - monotonic_counter_origin;
    } else {
        u64 flags = spin_lock_irqsave(&monotonic_counter_lock);
        /* Read the counter while holding the same lock that advances its
         * wrap state. Otherwise a delayed CPU could submit an older raw
         * sample after a newer one and be mistaken for a full wrap. */
        raw = timer_monotonic_counter();
        bool extended_ok = clocksource_extend_counter(
            &monotonic_counter_state, raw, monotonic_counter_mask,
            &extended);
        spin_unlock_irqrestore(&monotonic_counter_lock, flags);
        if (!extended_ok || extended < monotonic_extended_origin)
            return __atomic_load_n(&monotonic_published_ms, __ATOMIC_ACQUIRE);
        delta = extended - monotonic_extended_origin;
    }

    elapsed_ms = clocksource_ticks_to_ms(delta, monotonic_period_fs);
    candidate = monotonic_base_ms > ~(u64)0 - elapsed_ms ?
        ~(u64)0 : monotonic_base_ms + elapsed_ms;
    return clocksource_publish_max(&monotonic_published_ms, candidate);
}

void timer_monotonic_info(mg_monotonic_info_t *out)
{
    if (!out)
        return;
    out->time.milliseconds = timer_uptime_ms();
    out->resolution_ns = 1000000ULL;
    switch (monotonic_source) {
    case CLOCKSOURCE_HPET:
        out->source = MG_CLOCKSOURCE_HPET;
        out->quality = monotonic_counter_mask == ~(u64)0 ?
            MG_CLOCK_QUALITY_HARDWARE : MG_CLOCK_QUALITY_BOUNDED;
        break;
    case CLOCKSOURCE_ACPI_PM_TIMER:
        out->source = MG_CLOCKSOURCE_ACPI_PM_TIMER;
        out->quality = MG_CLOCK_QUALITY_BOUNDED;
        break;
    default:
        out->source = MG_CLOCKSOURCE_PIT;
        out->quality = MG_CLOCK_QUALITY_DEGRADED;
        break;
    }
}

u64 timer_preemptions(void)
{
    return preemption_count_snapshot(&preemptions);
}

void timer_sleep(u64 ms)
{
    u64 start = timer_uptime_ms();

    while (timer_uptime_ms() - start < ms) {
        __asm__ volatile("pause");
    }
}

void timer_delay(u64 ms)
{
    timer_sleep(ms);
}
