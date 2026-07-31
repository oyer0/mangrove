#include <timer.h>
#include <irq.h>
#include <keyboard.h>
#include <scheduler.h>

static volatile u64 ticks = 0;
static volatile u64 preemptions = 0;

void timer_init(void) {
    irq_register_handler(0, timer_interrupt);
}

void timer_interrupt(struct cpu_registers *regs) { 
    (void)regs;

    ticks++; 
    keyboard_update();

    if (scheduler_timer_tick()) {
        /* Scheduling is deferred until irq_handler has sent EOI. */
        preemptions++;
    }
}

u64 timer_ticks(void) {
    return ticks;
}

u64 timer_uptime_ms(void) { 
    return ticks; 
}

u64 timer_preemptions(void) {
    return preemptions;
}

void timer_sleep(u64 ms) { 
    u64 start = timer_uptime_ms();
    while (timer_uptime_ms() - start < ms) {
        // Just spin and let interrupts fire naturally without halting the CPU
        __asm__ volatile("pause");
    }
}

void timer_delay(u64 ms) { 
    timer_sleep(ms); 
}
