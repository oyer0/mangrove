#include <timer.h>
#include <irq.h>

static volatile u64 ticks = 0;

void timer_init(void) {
    irq_register_handler(0, timer_interrupt);
}

void timer_interrupt(struct cpu_registers *regs) { 
    (void)regs;

    ticks++; 
}

u64 timer_ticks(void) {
    return ticks;
}

u64 timer_uptime_ms(void) { 
    return ticks; 
}

void timer_sleep(u64 ms) { 
    u64 start = timer_uptime_ms();
    while (timer_uptime_ms() - start < ms) {
        __asm__ volatile("hlt"); 
    }
}

void timer_delay(u64 ms) { 
    timer_sleep(ms); 
}
