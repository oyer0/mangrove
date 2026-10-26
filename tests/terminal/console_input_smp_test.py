#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise production console queue code with deterministic host SMP races."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
scheduler = (ROOT / 'kernel/src/scheduler.c').read_text()


def function(name):
    match = next(m for m in re.finditer(
        r'^(?:static )?[A-Za-z_][^\n;{}]*\b' + name + r'\s*\(',
        scheduler, re.M) if ';' not in scheduler[m.end():scheduler.index('{', m.end())])
    brace = scheduler.index('{', match.end())
    end, depth = brace + 1, 1
    while depth:
        depth += (scheduler[end] == '{') - (scheduler[end] == '}')
        end += 1
    return scheduler[match.start():end] + '\n'


pre = r'''
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sched.h>
#include <types.h>
#include <terminal.h>
#include <console.h>
#include <scheduler.h>
#include <process.h>
#include <spinlock.h>
#include <drivers/input/keyboard.h>

static spinlock_t scheduler_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local kernel_thread_t *current;
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_condition = PTHREAD_COND_INITIALIZER;
static bool pause_before_block;
static bool block_entered;
static bool release_block;
static _Atomic u64 repeat_test_time;

void spinlock_init(spinlock_t *lock) { assert(!pthread_mutex_init(lock, NULL)); }
u64 spin_lock_irqsave(spinlock_t *lock) { assert(!pthread_mutex_lock(lock)); return 0; }
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags) {
    (void)flags; assert(!pthread_mutex_unlock(lock));
}
kernel_thread_t *thread_current(void) { return current; }
process_t *process_current(void) { return current ? current->process : NULL; }
bool process_shutdown_requested(const process_t *process) {
    return process && __atomic_load_n(&process->shutdown_requested,
                                      __ATOMIC_ACQUIRE);
}
static unsigned foreground_interrupt_calls;
bool process_is_session_shell(const process_t *process) {
    return process && process->session_shell;
}
bool process_take_interrupt(process_t *process) {
    return process && __atomic_exchange_n(&process->interrupt_requested,
                                           false, __ATOMIC_ACQ_REL);
}
bool process_interrupt_foreground_session(void) {
    foreground_interrupt_calls++;
    return true;
}
u64 timer_uptime_ms(void) { return atomic_load(&repeat_test_time); }
u8 inb(u16 port) { (void)port; return 0; }
void outb(u16 port, u8 value) { (void)port; (void)value; }

static bool scheduler_unblock_locked(kernel_thread_t *thread) {
    if (!thread || thread->state != THREAD_STATE_BLOCKED) return false;
    thread->state = THREAD_STATE_READY;
    assert(!pthread_cond_signal(&thread->condition));
    return true;
}
'''

post = r'''
bool scheduler_block(void) {
    if (pause_before_block) {
        assert(!pthread_mutex_lock(&gate_lock));
        block_entered = true;
        assert(!pthread_cond_broadcast(&gate_condition));
        while (!release_block)
            assert(!pthread_cond_wait(&gate_condition, &gate_lock));
        assert(!pthread_mutex_unlock(&gate_lock));
    }
    u64 flags = spin_lock_irqsave(&scheduler_lock);
    if (current->state == THREAD_STATE_READY)
        current->state = THREAD_STATE_RUNNING;
    if (scheduler_consume_pending_wake_locked(current, true)) {
        spin_unlock_irqrestore(&scheduler_lock, flags);
        return true;
    }
    current->state = THREAD_STATE_BLOCKED;
    while (current->state == THREAD_STATE_BLOCKED)
        assert(!pthread_cond_wait(&current->condition, &scheduler_lock));
    current->state = THREAD_STATE_RUNNING;
    spin_unlock_irqrestore(&scheduler_lock, flags);
    return true;
}
bool scheduler_sleep(u64 ticks) { (void)ticks; return scheduler_block(); }

typedef struct { kernel_thread_t thread; i64 result; char byte; } reader_t;
static void *read_one(void *opaque) {
    reader_t *reader = opaque;
    current = &reader->thread;
    reader->result = console_read_bytes_checked(&reader->byte, 1);
    return NULL;
}
static void wait_for_state(kernel_thread_t *thread, thread_state_t wanted) {
    for (unsigned spin = 0; spin < 1000000; spin++) {
        u64 flags = spin_lock_irqsave(&scheduler_lock);
        bool ready = thread->state == wanted;
        spin_unlock_irqrestore(&scheduler_lock, flags);
        if (ready) return;
        sched_yield();
    }
    assert(!"waiter did not reach expected scheduler state");
}
static void init_reader(reader_t *reader) {
    *reader = (reader_t){0};
    reader->thread.state = THREAD_STATE_RUNNING;
    assert(!pthread_cond_init(&reader->thread.condition, NULL));
}
static void finish_reader(reader_t *reader, pthread_t handle) {
    assert(!pthread_join(handle, NULL));
    assert(!pthread_cond_destroy(&reader->thread.condition));
}
static void *produce_sequence(void *opaque) {
    (void)opaque;
    for (unsigned i = 0; i < 1000; i++) {
        console_input((char)(33U + i % 90U));
        if ((i & 7U) == 0) sched_yield();
    }
    return NULL;
}
static void *consume_sequence(void *opaque) {
    char *bytes = opaque;
    current = NULL;
    for (unsigned i = 0; i < 1000; i++) {
        kernel_thread_t self = {.state = THREAD_STATE_RUNNING};
        pthread_cond_init(&self.condition, NULL);
        current = &self;
        assert(console_read_bytes_checked(&bytes[i], 1) == 1);
        pthread_cond_destroy(&self.condition);
    }
    return NULL;
}
static void *read_raw_forever(void *opaque) {
    reader_t *reader = opaque;
    terminal_key_event_t event;
    current = &reader->thread;
    reader->result = console_read_key_event(0xffffffffU, &event);
    return NULL;
}

int main(void) {
    console_init();
    kernel_thread_t main_thread = {.state = THREAD_STATE_RUNNING};
    pthread_cond_init(&main_thread.condition, NULL);
    current = &main_thread;

    /* Deterministically place the event after waiter registration but before
     * scheduler_block takes scheduler_lock. It must become a pending wake. */
    reader_t early; init_reader(&early);
    pause_before_block = true; block_entered = false; release_block = false;
    pthread_t early_handle; assert(!pthread_create(&early_handle, NULL, read_one, &early));
    assert(!pthread_mutex_lock(&gate_lock));
    while (!block_entered) assert(!pthread_cond_wait(&gate_condition, &gate_lock));
    assert(!pthread_mutex_unlock(&gate_lock));
    u64 state_flags = spin_lock_irqsave(&scheduler_lock);
    early.thread.state = THREAD_STATE_READY; /* preempted before park */
    spin_unlock_irqrestore(&scheduler_lock, state_flags);
    console_input('W');
    assert(__atomic_load_n(&early.thread.event_wake_pending, __ATOMIC_ACQUIRE));
    assert(!pthread_mutex_lock(&gate_lock));
    release_block = true; assert(!pthread_cond_broadcast(&gate_condition));
    assert(!pthread_mutex_unlock(&gate_lock));
    finish_reader(&early, early_handle);
    assert(early.result == 1 && early.byte == 'W');
    pause_before_block = false;

    /* A parked reader is awakened by the queue producer on another CPU. */
    reader_t parked; init_reader(&parked);
    pthread_t parked_handle; assert(!pthread_create(&parked_handle, NULL, read_one, &parked));
    wait_for_state(&parked.thread, THREAD_STATE_BLOCKED);
    console_input('P'); finish_reader(&parked, parked_handle);
    assert(parked.result == 1 && parked.byte == 'P');

    /* Concurrent queue production/consumption preserves byte order. */
    char consumed[1000]; pthread_t producer, consumer;
    assert(!pthread_create(&consumer, NULL, consume_sequence, consumed));
    assert(!pthread_create(&producer, NULL, produce_sequence, NULL));
    assert(!pthread_join(producer, NULL)); assert(!pthread_join(consumer, NULL));
    for (unsigned i = 0; i < 1000; i++) assert(consumed[i] == (char)(33U + i % 90U));

    /* Queue saturation drops new bytes without damaging queued data and
     * accepts input again after consumption. */
    for (unsigned i = 0; i < 2048; i++) console_input('a');
    console_input('z');
    char full[2048]; assert(console_read_bytes_checked(full, sizeof(full)) == 2048);
    for (unsigned i = 0; i < sizeof(full); i++) assert(full[i] == 'a');
    console_input('R'); char recovered = 0;
    assert(console_read_bytes_checked(&recovered, 1) == 1 && recovered == 'R');

    /* Raw queue capacity, repeat de-duplication, release discard, and mode
     * shutdown all share the same protected state and wake contract. */
    assert(console_set_raw_input(true, &main_thread));
    for (u32 i = 1; i <= 64; i++) console_input_key_event_flags(i, 0, i, 0);
    console_input_key_event_flags(100, 0, 100, 0);
    terminal_key_event_t event;
    for (u32 i = 1; i <= 64; i++) {
        assert(console_read_key_event(0, &event) == MG_OK);
        assert(event.keycode == i && event.codepoint == i);
    }
    assert(console_read_key_event(0, &event) == MG_ERR_WOULD_BLOCK);
    console_input_key_event_flags(44, 0, 'x', TERMINAL_KEY_EVENT_REPEAT);
    console_input_key_event_flags(44, 0, 'x', TERMINAL_KEY_EVENT_REPEAT);
    console_input_key_event_flags(45, 0, 'y', 0);
    console_discard_key_repeats(44);
    assert(console_read_key_event(0, &event) == MG_OK && event.keycode == 45);

    reader_t raw; init_reader(&raw);
    assert(console_set_raw_input(false, NULL));
    assert(console_set_raw_input(true, &raw.thread));
    pthread_t raw_handle; assert(!pthread_create(&raw_handle, NULL, read_raw_forever, &raw));
    wait_for_state(&raw.thread, THREAD_STATE_BLOCKED);
    assert(console_set_raw_input(false, NULL));
    finish_reader(&raw, raw_handle);
    assert(raw.result == MG_ERR_ACCESS_DENIED);

    /* Drive the production USB keyboard repeat state machine into the real
     * console raw-event queue. Replacing H with J while H has a queued repeat
     * must remove only the stale repeat, preserving J's initial keypress. */
    usb_keyboard_sources_init();
    atomic_store(&repeat_test_time, 0);
    assert(console_set_raw_input(true, &main_thread));
    const u8 key_h[1] = {0x0b};
    const u8 keys_hj[2] = {0x0b, 0x0d};
    const u8 key_j[1] = {0x0d};
    const u8 no_keys[1] = {0};
    usb_keyboard_handler(1, 11, 0, key_h, 1);
    assert(console_read_key_event(0, &event) == MG_OK);
    assert(event.keycode == 0x0b && event.flags == 0);
    atomic_store(&repeat_test_time, 400);
    keyboard_update();
    usb_keyboard_handler(1, 11, 0, keys_hj, 2);
    usb_keyboard_handler(1, 11, 0, key_j, 1);
    usb_keyboard_handler(1, 11, 0, no_keys, 0);
    assert(console_read_key_event(0, &event) == MG_OK);
    assert(event.keycode == 0x0d && event.flags == 0);
    assert(console_read_key_event(0, &event) == MG_ERR_WOULD_BLOCK);
    atomic_store(&repeat_test_time, 10000);
    keyboard_update();
    assert(console_read_key_event(0, &event) == MG_ERR_WOULD_BLOCK);

    /* Queue pressure must not make release discard ordinary keypresses or
     * leave a repeat behind after the held key is released. */
    assert(console_set_raw_input(false, NULL));
    assert(console_set_raw_input(true, &main_thread));
    for (u32 i = 0; i < 63; i++)
        console_input_key_event_flags(100 + i, 0, 100 + i, 0);
    const u8 key_k[1] = {0x0e};
    usb_keyboard_handler(1, 11, 0, key_k, 1);
    for (u32 i = 0; i < 63; i++) {
        assert(console_read_key_event(0, &event) == MG_OK);
        assert(event.keycode == 100 + i && event.flags == 0);
    }
    assert(console_read_key_event(0, &event) == MG_OK);
    assert(event.keycode == 0x0e && event.flags == 0);
    atomic_store(&repeat_test_time, 10400);
    keyboard_update();
    for (u32 i = 0; i < 63; i++)
        console_input_key_event_flags(200 + i, 0, 200 + i, 0);
    usb_keyboard_handler(1, 11, 0, no_keys, 0);
    for (u32 i = 0; i < 63; i++) {
        assert(console_read_key_event(0, &event) == MG_OK);
        assert(event.keycode == 200 + i && event.flags == 0);
    }
    assert(console_read_key_event(0, &event) == MG_ERR_WOULD_BLOCK);
    atomic_store(&repeat_test_time, 20000);
    keyboard_update();
    assert(console_read_key_event(0, &event) == MG_ERR_WOULD_BLOCK);

    /* Two quick presses of the same physical key are independent initial
     * events. Release cleanup may discard typematic events, never these. */
    usb_keyboard_handler(2, 22, 0, key_h, 1);
    usb_keyboard_handler(2, 22, 0, no_keys, 0);
    usb_keyboard_handler(2, 22, 0, key_h, 1);
    usb_keyboard_handler(2, 22, 0, no_keys, 0);
    for (u32 i = 0; i < 2; i++) {
        assert(console_read_key_event(0, &event) == MG_OK);
        assert(event.keycode == 0x0b && event.flags == 0);
    }
    assert(console_read_key_event(0, &event) == MG_ERR_WOULD_BLOCK);

    /* A Shoot line editor may claim key identities while cooked type-ahead
     * bytes are already queued. Those bytes remain first and are returned as
     * tagged byte events; ordinary text typed just after Enter is returned to
     * the cooked queue when the normal-screen event claim ends. */
    console_set_raw_input(false, NULL);
    console_input('a');
    assert(console_set_raw_input_with_cooked_bytes(true, &main_thread));
    console_input_key_event_flags(0x06, 0x01, 'c', 0);
    assert(console_read_key_event(0, &event) == MG_OK);
    assert(event.flags == TERMINAL_KEY_EVENT_COOKED_BYTE &&
           event.codepoint == 'a');
    assert(console_read_key_event(0, &event) == MG_OK);
    assert(event.keycode == 0x06 && event.modifiers == 0x01 &&
           event.codepoint == 'c');
    console_input_key_event_flags(0x1c, 0, 'z', 0);
    assert(console_set_raw_input_preserving_text(false, NULL));
    assert(console_read_bytes_checked(&recovered, 1) == 1 && recovered == 'z');

    /* Reed and other alternate-screen raw clients keep the event-only API;
     * stale cooked type-ahead must remain available to cooked readers. */
    console_input('b');
    assert(console_set_raw_input(true, &main_thread));
    console_input_key_event_flags(0x1c, 0, 'q', 0);
    assert(console_read_key_event(0, &event) == MG_OK &&
           event.keycode == 0x1c && event.codepoint == 'q');
    assert(console_read_key_event(0, &event) == MG_ERR_WOULD_BLOCK);
    assert(console_set_raw_input(false, NULL));
    assert(console_read_bytes_checked(&recovered, 1) == 1 && recovered == 'b');

    /* The process adapter follows the production thread->process lookup and
     * shutdown flag contract for readers that belong to a process. */
    process_t shutdown_process = {.shutdown_requested = true};
    kernel_thread_t shutdown_thread = {
        .state = THREAD_STATE_RUNNING, .process = &shutdown_process
    };
    current = &shutdown_thread;
    assert(console_read_bytes_checked(&recovered, 1) == MG_ERR_SHUTDOWN);
    assert(console_read_key_event(0, &event) == MG_ERR_SHUTDOWN);
    process_t interrupted_process = {0};
    kernel_thread_t interrupted_thread = {
        .state = THREAD_STATE_RUNNING, .process = &interrupted_process
    };
    interrupted_process.interrupt_requested = true;
    current = &interrupted_thread;
    assert(console_read_bytes_checked(&recovered, 1) == MG_ERR_INTERRUPTED);
    assert(!interrupted_process.interrupt_requested);
    current = &main_thread;

    /* The production HID path routes Ctrl+C to Shoot's raw line editor, but
       interrupts the foreground application when another process owns raw
       input. In cooked mode it invokes the same foreground interruption
       mechanism and never inserts a printable c byte. */
    process_t shell_process = {.session_shell = true};
    main_thread.process = &shell_process;
    assert(console_set_raw_input(true, &main_thread));
    const u8 key_c[1] = {0x06};
    usb_keyboard_handler(3, 33, 1, key_c, 1);
    assert(foreground_interrupt_calls == 0U);
    assert(console_read_key_event(0, &event) == MG_OK);
    assert(event.keycode == 0x06 && event.modifiers == 1U &&
           event.codepoint == 'c');
    usb_keyboard_handler(3, 33, 0, no_keys, 0);

    process_t foreground_process = {0};
    kernel_thread_t foreground_thread = {
        .state = THREAD_STATE_RUNNING, .process = &foreground_process
    };
    assert(console_set_raw_input(false, NULL));
    assert(console_set_raw_input(true, &foreground_thread));
    usb_keyboard_handler(3, 33, 1, key_c, 1);
    assert(foreground_interrupt_calls == 1U);
    current = &foreground_thread;
    assert(console_read_key_event(0, &event) == MG_ERR_WOULD_BLOCK);
    current = &main_thread;
    usb_keyboard_handler(3, 33, 0, no_keys, 0);
    assert(console_set_raw_input(false, NULL));

    usb_keyboard_handler(3, 33, 1, key_c, 1);
    assert(foreground_interrupt_calls == 2U);
    usb_keyboard_handler(3, 33, 0, no_keys, 0);
    console_input('Q');
    assert(console_read_bytes_checked(&recovered, 1) == 1 && recovered == 'Q');
    current = &main_thread;

    pthread_cond_destroy(&main_thread.condition);
    puts("console input: SMP queue/wakeup, saturation, raw ownership, and keyboard repeat cancellation passed");
}
'''

with tempfile.TemporaryDirectory(prefix='mangrove-console-input-') as directory:
    temp = Path(directory)
    (temp / 'mg').mkdir()
    (temp / 'console.h').write_text(r'''#pragma once
#include <types.h>
#include <terminal.h>
struct kernel_thread;
void console_init(void); void console_input(char); void console_input_key(u32);
void console_input_key_event_flags(u32,u32,u32,u32); void console_discard_key_repeats(u32);
bool console_raw_input_active(void); bool console_set_raw_input(bool,struct kernel_thread *);
bool console_set_raw_input_with_cooked_bytes(bool,struct kernel_thread *);
i64 console_read_key_event(u32,terminal_key_event_t *);
i64 console_read_bytes_checked(void *,u64); void console_cancel_waiter(struct kernel_thread *);
''')
    (temp / 'terminal.h').write_text(r'''#pragma once
#include <types.h>
typedef struct PACKED {u32 version,keycode,modifiers,codepoint,flags;} terminal_key_event_t;
#define TERMINAL_KEY_EVENT_REPEAT 1U
#define TERMINAL_KEY_EVENT_COOKED_BYTE 2U
#define TERMINAL_KEY_MODIFIER_LEFT_CTRL 1U
#define TERMINAL_KEY_MODIFIER_RIGHT_CTRL 16U
#define TERMINAL_KEY_MODIFIER_LEFT_ALT 4U
#define TERMINAL_KEY_MODIFIER_RIGHT_ALT 64U
#define TERMINAL_KEY_MODIFIER_LEFT_GUI 8U
#define TERMINAL_KEY_MODIFIER_RIGHT_GUI 128U
''')
    (temp / 'scheduler.h').write_text(r'''#pragma once
#include <types.h>
typedef enum {THREAD_STATE_RUNNING,THREAD_STATE_READY,THREAD_STATE_BLOCKED,THREAD_STATE_TERMINATED} thread_state_t;
typedef struct kernel_thread {thread_state_t state;bool mutex_wake_pending,event_wake_pending;pthread_cond_t condition;struct process *process;} kernel_thread_t;
kernel_thread_t *thread_current(void); bool scheduler_block(void); bool scheduler_sleep(u64);
bool scheduler_wake_waiter(kernel_thread_t *);
''')
    (temp / 'process.h').write_text(r'''#pragma once
#include <types.h>
typedef struct process { bool shutdown_requested, session_shell; volatile bool interrupt_requested; } process_t;
process_t *process_current(void);
bool process_shutdown_requested(const process_t *process);
bool process_is_session_shell(const process_t *process);
bool process_take_interrupt(process_t *process);
bool process_interrupt_foreground_session(void);
''')
    (temp / 'spinlock.h').write_text(r'''#pragma once
#include <pthread.h>
#include <types.h>
typedef pthread_mutex_t spinlock_t;
void spinlock_init(spinlock_t *); u64 spin_lock_irqsave(spinlock_t *);
void spin_unlock_irqrestore(spinlock_t *,u64);
''')
    console_path = (ROOT / 'kernel/src/console.c').as_posix()
    keyboard_path = (ROOT / 'kernel/src/drivers/input/keyboard.c').as_posix()
    cfile = temp / 'test.c'
    cfile.write_text(pre + function('scheduler_wake_waiter') +
                     function('scheduler_consume_pending_wake_locked') +
                     function('scheduler_clear_event_wake') +
                     f'\n#include "{console_path}"\n'
                     f'\n#include "{keyboard_path}"\n' + post)
    binary = temp / 'test'
    subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'cc', '-std=gnu17',
                    '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-I', str(temp),
                    '-I', str(ROOT / 'kernel/include'), '-I', str(ROOT / 'include'),
                    '-idirafter', str(ROOT / 'libc/include'),
                    str(cfile), str(ROOT / 'libc/src/utf8.c'), '-o', str(binary)],
                   check=True)
    subprocess.run([str(binary)], check=True, timeout=30,
                   env={**__import__('os').environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
