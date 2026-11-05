/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <drivers/input/keyboard.h>
#include <acpi.h>
#include <console.h>
#include <idt.h>
#include <irq.h>
#include <lapic.h>
#include <spinlock.h>
#include <timer.h>

void spinlock_init(spinlock_t *lock) { __atomic_store_n(&lock->value,0,__ATOMIC_RELAXED); }
u64 spin_lock_irqsave(spinlock_t *lock)
{
    while (__atomic_exchange_n(&lock->value,1,__ATOMIC_ACQUIRE)) sched_yield();
    return 0;
}
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{ (void)flags; __atomic_store_n(&lock->value,0,__ATOMIC_RELEASE); }

static acpi_fadt_info_t fake_fadt;
const acpi_fadt_info_t *acpi_fadt_get(void) { return &fake_fadt; }
u32 acpi_irq_to_gsi(u8 irq) { assert(irq == 1 || irq == 12); return irq == 1 ? 9 : 18; }
u16 acpi_irq_flags(u8 irq) { assert(irq == 1 || irq == 12); return irq == 1 ? 15 : 0; }
u8 lapic_current_id(void) { return 4; }
static bool route_ok;
static unsigned routes;
static u8 route_fail_irq;
bool ioapic_route_gsi(u32 gsi, u8 vector, u8 apic_id, u16 flags)
{
    assert(apic_id == 4);
    assert((gsi == 9 && vector == IRQ_VECTOR_PS2 && flags == 15) ||
           (gsi == 18 && vector == IRQ_VECTOR_PS2_AUX && flags == 0));
    routes++;
    return route_ok && route_fail_irq != (gsi == 9 ? 1 : 12);
}
static u64 eoi_calls, preemption_calls;
bool lapic_enabled(void) { return true; }
void lapic_eoi(void) { eoi_calls++; }
bool scheduler_prepare_preemption(struct cpu_registers *regs)
{ (void)regs; assert(eoi_calls == preemption_calls + 1); preemption_calls++; return false; }

static _Atomic u64 clock_us;
static bool clock_ready;
static bool clock_frozen;
bool timer_monotonic_ready(void) { return clock_ready; }
u64 timer_uptime_ms(void) { return clock_us / 1000; }
bool timer_monotonic_deadline_start(timer_monotonic_deadline_t *d, u64 us)
{ d->start_ticks = clock_us; d->duration_ticks = us; return clock_ready; }
bool timer_monotonic_deadline_expired(timer_monotonic_deadline_t *d)
{ return clock_us - d->start_ticks >= d->duration_ticks; }

static struct {u8 value, status;} fifo[4096];
static unsigned head, tail, reads, writes, status_reads, data_commands;
static bool absent, stuck, bad_config, silent_config, scanning;
static u8 config, pending, port_test, resend_command;
static u16 silence_command;
static unsigned resends;
static u8 idle_status;
static unsigned reply_delay, delayed_reads;
static u8 commands[64];
static u8 ack, ack_status;
static bool interleave_aux;
static void enqueue(u8 value, u8 status)
{
    if (head == tail) head = tail = 0;
    assert(tail < sizeof(fifo)/sizeof(fifo[0]));
    fifo[tail].value=value; fifo[tail++].status=status;
    delayed_reads=reply_delay;
}
u8 inb(u16 port)
{
    if (port == 0x64) {
        status_reads++; if (!clock_frozen) clock_us += 10;
        if (absent) return 0xFF;
        if (stuck) return 2;
        if (delayed_reads) { delayed_reads--; return idle_status; }
        return head < tail ? 1U | fifo[head].status : idle_status;
    }
    assert(port == 0x60 && head < tail);
    reads++;
    return fifo[head++].value;
}
void outb(u16 port, u8 byte)
{
    writes++;
    if (port == 0x64) {
        /* These commands could change/reset AUX or the whole controller. */
        assert(byte != 0xA7 && byte != 0xA8 && byte != 0xAA && byte != 0xD4);
        if (byte == 0xAD) config |= 0x10;
        else if (byte == 0xAE) config &= ~0x10;
        else if (byte == 0x20) {
            if (!silent_config) enqueue(config ^ (bad_config && scanning ? 0x40 : 0), 0);
        } else if (byte == 0x60) pending = byte;
        else if (byte == 0xAB) enqueue(port_test, 0);
        else assert(false);
        return;
    }
    assert(port == 0x60);
    if (pending == 0x60) {
        config=bad_config ? byte & ~0x40U : byte; pending=0; return;
    }
    assert(data_commands < sizeof(commands)); commands[data_commands++] = byte;
    if (byte == silence_command) return;
    if (byte == resend_command && resends) {
        resends--; enqueue(0xFE,0); return;
    }
    if (interleave_aux) enqueue(0x1E,0x20);
    enqueue(ack,ack_status);
    if (byte == 0xF4) scanning = true;
}

static bool raw = true;
static pthread_mutex_t events_lock = PTHREAD_MUTEX_INITIALIZER;
static terminal_key_event_t events[32768];
static unsigned event_count, discard_count, interrupts, cooked_count;
static char cooked[8192];
bool console_raw_input_active(void) { return raw; }
bool console_control_c(void) { interrupts++; return true; }
void console_discard_key_repeats(u32 key) { assert(key); discard_count++; }
void console_input(char c) { assert(cooked_count < sizeof(cooked)); cooked[cooked_count++]=c; }
void console_input_key_event_flags(u32 key, u32 mods, u32 cp, u32 flags)
{
    pthread_mutex_lock(&events_lock);
    assert(event_count < sizeof(events)/sizeof(events[0]));
    events[event_count++] = (terminal_key_event_t){
        .keycode=key,.modifiers=mods,.codepoint=cp,.flags=flags};
    pthread_mutex_unlock(&events_lock);
}

#include "../../kernel/src/irq.c"
#include "../../kernel/src/drivers/input/keyboard.c"
#include "../../kernel/src/drivers/input/ps2_keyboard.c"
#include "irq_path.h"

static void reset(void)
{
    active = false;
    irq_unregister_vector(IRQ_VECTOR_PS2);
    irq_unregister_vector(IRQ_VECTOR_PS2_AUX);
    fake_fadt=(acpi_fadt_info_t){.i8042_known=true,.i8042_present=true};
    head=tail=reads=writes=status_reads=data_commands=routes=0;
    eoi_calls=preemption_calls=route_fail_irq=0;
    absent=stuck=bad_config=silent_config=scanning=false;
    pending=port_test=resend_command=0; silence_command=0xFFFF;
    idle_status=0; reply_delay=delayed_reads=0;
    clock_us=0; clock_ready=route_ok=true;
    clock_frozen=false;ack=0xFA;ack_status=0;
    interleave_aux=false;
    /* Preserve AUX IRQ enable, clock-disable, system/reserved bits exactly. */
    config=0xAE;
    event_count=discard_count=interrupts=cooked_count=0; raw=true;
    __atomic_store_n(&keyboard_locks,0,__ATOMIC_RELAXED);
#if PS2_INPUT_DIAGNOSTICS
    pith_input_diagnostics=(input_diagnostics_t){.version=1};
#endif
}
static void start(void)
{
    const char *reason;
    assert(ps2_keyboard_init(&reason));
    assert(!strcmp(reason,"translated Set 1 on IRQ1"));
    assert(config == 0xEF && active && scanning && routes == 2);
    const u8 expected[]={0xF5,0xF0,2,0xED,0,0xF4};
    assert(data_commands == sizeof(expected) && !memcmp(commands,expected,sizeof(expected)));
    assert(head==tail); /* all command ACKs consumed before IRQ admission */
}
static void dispatch(void)
{
    struct cpu_registers regs={.vec_no=IRQ_VECTOR_PS2};
    irq_handler(&regs);
}
static void dispatch_aux(void)
{
    struct cpu_registers regs={.vec_no=IRQ_VECTOR_PS2_AUX};
    irq_handler(&regs);
}
static void bytes(const u8 *data, unsigned count)
{
    for (unsigned i=0;i<count;i++) enqueue(data[i],0);
    dispatch();
    assert(head==tail);
}
#define SEND(...) do { const u8 input[]={__VA_ARGS__}; bytes(input,sizeof(input)); } while (0)
static void failed(const char *part)
{
    const char *reason;
    assert(!ps2_keyboard_init(&reason) && strstr(reason,part));
    assert(!active);
    unsigned before=event_count;
    enqueue(0x23,0);dispatch(); assert(event_count==before);
}
static void occupied_irq(struct cpu_registers *r) { (void)r; }
static void *usb_reports(void *unused)
{
    (void)unused;
    const u8 key[]={0x05};
    for (unsigned i=0;i<1000;i++) {
        usb_keyboard_handler(3,33,0,key,1);
        usb_keyboard_handler(3,33,0,key,0);
    }
    return NULL;
}
int main(void)
{
    keyboard_init();
    assert(writes==0); /* early logical setup must not disable either port */
    reset();start();
    /* Source/error flags do not describe a byte until OBF is set. */
    reset();idle_status=0x20;reply_delay=3;start();
    idle_status=0;reply_delay=delayed_reads=0;
    SEND(0x23,0xA3,0x12,0x92,0x26,0xA6,0x19,0x99,0x1C,0x9C);
    assert(event_count==5 && events[0].codepoint=='h' &&
           events[1].codepoint=='e' && events[2].codepoint=='l' &&
           events[3].codepoint=='p' && events[4].codepoint=='\n');
    assert(discard_count==5);
    SEND(0x2A,0x36,0x1E,0x9E,0xAA,0x1E,0x9E,0xB6,0x1E,0x1E,0x9E);
    assert(events[5].modifiers==0x22 && events[5].codepoint=='A');
    assert(events[6].modifiers==0x20 && events[6].codepoint=='A');
    assert(events[7].modifiers==0 && events[7].codepoint=='a');
    assert(events[8].flags==TERMINAL_KEY_EVENT_REPEAT);
    SEND(0x3A,0x3A,0xBA,0x1E,0x9E,0x2A,0x1E,0x9E,0xAA);
    assert(keyboard_get_lock_state()==KEYBOARD_LOCK_CAPS);
    assert(events[9].codepoint=='A' && events[10].codepoint=='a');
    SEND(0xE0,0x1D,0xE0,0x38,0xE0,0x4B,0xE0,0xCB,0xE0,0x9D,0xE0,0xB8);
    assert(events[11].keycode==0x50 && events[11].modifiers==0x50);
    SEND(0xE0,0x2A,0xE0,0x37,0xE0,0xB7,0xE0,0xAA);
    assert(events[12].keycode==0x46 && !events[12].modifiers);
    SEND(0xE1,0x1D,0x45,0xE1,0x9D,0xC5);
    assert(events[13].keycode==0x48 && !events[13].modifiers);
    assert(keyboard_get_lock_state()==KEYBOARD_LOCK_CAPS);
    SEND(0x1D,0x2E,0x2E,0xAE,0x9D);
    assert(interrupts==1); /* Ctrl+C repeat never interrupts twice */
    SEND(0xFA,0xFE,0,0xFF,0xE0,0x7F,0xE1,0x23,0xA3);
    assert(event_count==15 && events[14].codepoint=='H');
    unsigned before=event_count;
    enqueue(0x1E,0x80);dispatch(); assert(event_count==before);
    unsigned read_before=reads;
    enqueue(0x1E,0x20);dispatch(); assert(reads==read_before+1 && head==tail);
    assert(event_count==before); /* AUX cannot masquerade as a keyboard make */
    for (unsigned i=0;i<40;i++) enqueue(0x1E,0);
    dispatch();assert(tail-head==8);dispatch();assert(head==tail);
    reset();start();raw=false;
    SEND(0x23,0xA3,0x12,0x92,0x26,0xA6,0x19,0x99,0x1C,0x9C);
    assert(cooked_count==5 && !memcmp(cooked,"help\n",5));
    reset();start();
    pthread_t usb;assert(!pthread_create(&usb,NULL,usb_reports,NULL));
    for (unsigned i=0;i<1000;i++) { SEND(0x1E,0x9E); }
    assert(!pthread_join(usb,NULL));
    assert(event_count==2000); /* one event per transport make, no duplication */

    /* One AUX byte must not wedge the shared output buffer after g. IRQ12
     * retires AUX and any already queued keyboard bytes; IRQ1 may be stale. */
    reset();start();SEND(0x22,0xA2);
    enqueue(0x08,0x20);enqueue(0x25,0);enqueue(0xA5,0);
    dispatch_aux();dispatch();
    assert(event_count==2 && events[0].codepoint=='g' && events[1].codepoint=='k');
    assert(head==tail && eoi_calls==3 && preemption_calls==3);

    /* Sustained rapid make/break traffic, interleaved with unsupported AUX
     * packets and received through both real IRQ dispatch/EOI paths. */
    reset();start();
    for (unsigned i=0;i<12000;i++) {
        if (i%7==0) {
            enqueue(0x08,0x20);enqueue(0x1E,0x20);enqueue(0xFA,0x20);
        }
        u8 scan=i%2 ? 0x25 : 0x22;
        enqueue(scan,0);enqueue(scan|0x80,0);
        if (i%7==0) dispatch_aux();else dispatch();
        assert(head==tail);
    }
    assert(event_count==12000 && eoi_calls==12000 && preemption_calls==12000);
    struct cpu_registers spurious={.vec_no=LAPIC_SPURIOUS_VECTOR};
    irq_handler(&spurious);assert(eoi_calls==12000 && preemption_calls==12000);
#if PS2_INPUT_DIAGNOSTICS
    assert(pith_input_diagnostics.kbd_bytes==24000);
    assert(pith_input_diagnostics.decoded==12000);
    assert(pith_input_diagnostics.aux_bytes==5145);
    assert(pith_input_diagnostics.irq1==pith_input_diagnostics.eoi1);
    assert(pith_input_diagnostics.irq12==pith_input_diagnostics.eoi12);
#endif
    for(unsigned i=0;i<12000;i++)
        assert(events[i].codepoint==(i%2 ? 'k' : 'g') && !events[i].flags);

    reset();start();SEND(0x2A,0xE0,0x1C,0xE0,0x9C,0xE0,0x35,0xE0,0xB5,0xAA);
    assert(event_count==2 && events[0].keycode==0x58 &&
           events[0].codepoint=='\n' && events[1].keycode==0x54 &&
           events[1].codepoint=='/' && events[1].modifiers==2);

    reset();fake_fadt.i8042_known=false;fake_fadt.i8042_present=false;start();

    reset();fake_fadt.i8042_present=false;failed("absent");assert(!writes);
    reset();fake_fadt.hardware_reduced=true;failed("absent");assert(!writes);
    reset();absent=true;failed("absent");assert(!writes);
    reset();clock_ready=false;failed("clock");assert(!writes);
    reset();stuck=true;failed("quiesce");assert(clock_us<=100100 && !writes);
    reset();stuck=clock_frozen=true;failed("quiesce");
    assert(status_reads==PS2_POLL_LIMIT+1 && !writes);
    reset();silent_config=true;failed("configuration read");assert(clock_us<201000);
    reset();port_test=1;failed("port test");assert(!data_commands);
    reset();silence_command=0xF5;failed("command/ACK");assert(config==0xBE);
    reset();ack=0xFC;failed("command/ACK");assert(data_commands==1);
    reset();ack_status=0x80;failed("command/ACK");assert(data_commands==1);
    reset();silence_command=0xF4;failed("enable/ACK");assert(config==0xBE);
    reset();bad_config=true;failed("translation configuration");assert(config==0xBE);
    reset();resend_command=0xF5;resends=3;failed("command/ACK");assert(data_commands==3);
    reset();resend_command=0xF5;resends=1;
    const char *reason;assert(ps2_keyboard_init(&reason));assert(data_commands==7);
    reset();route_ok=false;failed("IRQ1 routing");assert(config==0xBE);
    assert(irq_slots[IRQ_VECTOR_PS2].handler==NULL);
    assert(irq_slots[IRQ_VECTOR_PS2_AUX].handler==NULL);
    reset();route_fail_irq=12;failed("IRQ12 routing");assert(config==0xBE);
    assert(irq_slots[IRQ_VECTOR_PS2].handler==NULL);
    assert(irq_slots[IRQ_VECTOR_PS2_AUX].handler==NULL);
    reset();assert(irq_register_vector(IRQ_VECTOR_PS2,occupied_irq));
    failed("IRQ1 registration/routing");assert(irq_slots[IRQ_VECTOR_PS2].handler==occupied_irq);
    reset();assert(irq_register_vector(IRQ_VECTOR_PS2_AUX,occupied_irq));
    failed("IRQ12 registration");assert(irq_slots[IRQ_VECTOR_PS2_AUX].handler==occupied_irq);
    reset();enqueue(1,0x20);start();
    reset();interleave_aux=true;start();assert(event_count==0 && head==tail);
    reset();config=0x8C;assert(ps2_keyboard_init(&reason));assert(config==0xCF);
    reset();for(unsigned i=0;i<40;i++)enqueue(0x1E,0);
    failed("output busy");assert(reads==32);
    puts("PS/2: translated make/break, modifiers, locks/repeat, special keys, IRQ delivery/budget, AUX isolation, concurrent USB, bounded init failures and rollback passed");
}
