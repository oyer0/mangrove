/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <timer.h>
#include <acpi.h>
#include <irq.h>
#include <drivers/input/keyboard.h>
#include <terminal.h>
#include <scheduler.h>
#include <kprint.h>
#include <vmm.h>
#include <lapic.h>
#include <input_trace.h>
#if INPUT_TRACE_DIAGNOSTICS
#include <spinlock.h>
#endif

#include <stddef.h>

#define HPET_MMIO_SIZE                    0x400U
#define HPET_GENERAL_CAPABILITIES         0x000U
#define HPET_GENERAL_CONFIGURATION        0x010U
#define HPET_MAIN_COUNTER                 0x0F0U
#define HPET_CONFIGURATION_ENABLE         (1ULL << 0)
#define HPET_CONFIGURATION_LEGACY_ROUTE   (1ULL << 1)
#define HPET_CAPABILITY_COUNTER_64        (1ULL << 13)
#define HPET_MAX_PERIOD_FS                100000000ULL
#define FEMTOSECONDS_PER_MICROSECOND      1000000000ULL
#define ACPI_ADDRESS_SPACE_SYSTEM_MEMORY  0U

static u64 ticks = 0;
static volatile u64 preemptions = 0;
static volatile u8 *monotonic_hpet;
static u64 monotonic_counter_mask;
static u64 monotonic_period_fs;
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

static u64 timer_monotonic_counter(void)
{
    if (monotonic_counter_mask == 0xFFFFFFFFULL) {
        u32 value = *(volatile u32 *)(monotonic_hpet + HPET_MAIN_COUNTER);
        __asm__ volatile("" ::: "memory");
        return value;
    }
    return hpet_read64(HPET_MAIN_COUNTER);
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
    (void)irq_register_vector(IRQ_VECTOR_PIT, timer_interrupt);
    (void)irq_register_vector(IRQ_VECTOR_LAPIC_TIMER,
                              timer_lapic_interrupt);
}

bool timer_monotonic_init(void)
{
    const acpi_hpet_t *table = acpi_hpet();
    u64 capabilities;
    u64 configuration;

    if (monotonic_is_ready)
        return true;

    monotonic_is_ready = false;
    monotonic_hpet = NULL;
    monotonic_counter_mask = 0;
    monotonic_period_fs = 0;

    if (!table ||
        table->base_address.address_space_id !=
            ACPI_ADDRESS_SPACE_SYSTEM_MEMORY ||
        table->base_address.register_bit_offset != 0 ||
        !table->base_address.address ||
        (table->base_address.address & (HPET_MMIO_SIZE - 1U))) {
        return false;
    }

    monotonic_hpet = (volatile u8 *)vmm_map_mmio(
        (phys_addr_t)table->base_address.address, HPET_MMIO_SIZE);
    if (!monotonic_hpet ||
        !vmm_ioremap_contains((const void *)monotonic_hpet)) {
        monotonic_hpet = NULL;
        return false;
    }

    capabilities = hpet_read64(HPET_GENERAL_CAPABILITIES);
    monotonic_period_fs = capabilities >> 32;
    if (!monotonic_period_fs ||
        monotonic_period_fs > HPET_MAX_PERIOD_FS) {
        monotonic_hpet = NULL;
        monotonic_period_fs = 0;
        return false;
    }
    monotonic_counter_mask =
        (capabilities & HPET_CAPABILITY_COUNTER_64) ?
            ~(u64)0 : 0xFFFFFFFFULL;

    /* Mangrove uses neither HPET comparators nor legacy replacement routing.
     * Enable only the free-running main counter and leave PIT IRQ0 unchanged. */
    configuration = hpet_read64(HPET_GENERAL_CONFIGURATION);
    configuration |= HPET_CONFIGURATION_ENABLE;
    configuration &= ~HPET_CONFIGURATION_LEGACY_ROUTE;
    hpet_write64(HPET_GENERAL_CONFIGURATION, configuration);
    configuration = hpet_read64(HPET_GENERAL_CONFIGURATION);
    if (!(configuration & HPET_CONFIGURATION_ENABLE) ||
        (configuration & HPET_CONFIGURATION_LEGACY_ROUTE)) {
        monotonic_hpet = NULL;
        monotonic_counter_mask = 0;
        monotonic_period_fs = 0;
        return false;
    }

    monotonic_is_ready = true;
#if INPUT_TRACE_DIAGNOSTICS
    pith_input_trace.counter_physical = table->base_address.address + HPET_MAIN_COUNTER;
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
    terminal_cursor_blink_timer_tick();
    keyboard_update();

    (void)scheduler_global_tick();
    if (!lapic_timer_active() && scheduler_timer_tick()) {
        /* Scheduling is deferred until irq_handler has sent EOI. */
        preemptions++;
    }
}

void timer_lapic_interrupt(struct cpu_registers *regs)
{
    (void)regs;
    if (scheduler_timer_tick()) {
        /* The common IRQ exit path performs deferred preemption after EOI. */
        preemptions++;
    }
}

u64 timer_ticks(void)
{
    return __atomic_load_n(&ticks, __ATOMIC_RELAXED);
}

u64 timer_uptime_ms(void)
{
    return __atomic_load_n(&ticks, __ATOMIC_RELAXED);
}

u64 timer_preemptions(void)
{
    return preemptions;
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
