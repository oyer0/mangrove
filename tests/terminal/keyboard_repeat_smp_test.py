#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run production USB HID state transitions against a concurrent PIT updater."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
keyboard_path = (ROOT / 'kernel/src/drivers/input/keyboard.c').as_posix()
source = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <types.h>
#include <drivers/input/keyboard.h>
#include <console.h>
#include <timer.h>

typedef pthread_mutex_t spinlock_t;
void spinlock_init(spinlock_t *lock) { assert(!pthread_mutex_init(lock, NULL)); }
u64 spin_lock_irqsave(spinlock_t *lock) { assert(!pthread_mutex_lock(lock)); return 0; }
void spin_unlock_irqrestore(spinlock_t *lock, u64 flags) {
    (void)flags; assert(!pthread_mutex_unlock(lock));
}

static _Atomic u64 fake_time;
static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;
static char output[100000];
static _Atomic u64 output_count;
static _Atomic bool raw_mode;
static terminal_key_event_t raw_events[32];
static _Atomic u64 raw_event_count;
u8 inb(u16 port) { (void)port; return 0; }
void outb(u16 port, u8 value) { (void)port; (void)value; }
u64 timer_uptime_ms(void) { return atomic_load_explicit(&fake_time, memory_order_relaxed); }
bool console_raw_input_active(void) { return atomic_load(&raw_mode); }
void console_input(char c) {
    pthread_mutex_lock(&output_lock);
    u64 index = atomic_load_explicit(&output_count, memory_order_relaxed);
    if (index < sizeof(output)) output[index] = c;
    atomic_store_explicit(&output_count, index + 1U, memory_order_relaxed);
    pthread_mutex_unlock(&output_lock);
}
void console_input_key_event_flags(u32 k,u32 m,u32 c,u32 f) {
    u64 index = atomic_fetch_add(&raw_event_count, 1);
    if (index < sizeof(raw_events) / sizeof(raw_events[0]))
        raw_events[index] = (terminal_key_event_t){
            .version=1,.keycode=k,.modifiers=m,.codepoint=c,.flags=f};
}
void console_discard_key_repeats(u32 key) {(void)key;}

'''
post = r'''
static void *report_worker(void *unused) {
    (void)unused;
    const u8 pressed[1] = {0x04};
    for (unsigned i = 0; i < 5000; i++) {
        usb_keyboard_handler(7, 77, 0, pressed, 1);
        usb_keyboard_handler(7, 77, 0, pressed, 0);
    }
    return NULL;
}
static void *repeat_worker(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 5000; i++) {
        atomic_fetch_add_explicit(&fake_time, 40, memory_order_relaxed);
        keyboard_update();
    }
    return NULL;
}
int main(void) {
    const u8 key_a[1] = {0x04};
    usb_keyboard_sources_init();

    /* Exact press/hold/release contract: one initial character, repeats only
     * after the delay, and no events after release. */
    usb_keyboard_handler(1, 11, 0, key_a, 1);
    assert(atomic_load(&output_count) == 1 && output[0] == 'a');
    atomic_store(&fake_time, 399); keyboard_update();
    assert(atomic_load(&output_count) == 1);
    atomic_store(&fake_time, 400); keyboard_update();
    assert(atomic_load(&output_count) == 2 && output[1] == 'a');
    usb_keyboard_handler(1, 11, 0, key_a, 0);
    atomic_store(&fake_time, 10000); keyboard_update();
    assert(atomic_load(&output_count) == 2);

    /* Control/application modifiers do not fall through as printable bytes
     * to cooked console readers. The event-mode path preserves key identity
     * and the complete modifier mask for the application to interpret. */
    const u8 key_c[1] = {0x06};
    usb_keyboard_handler(1, 11, 0x01, key_c, 1);
    const u8 no_keys[1] = {0};
    usb_keyboard_handler(1, 11, 0, no_keys, 0);
    const u8 key_q[1] = {0x14};
    usb_keyboard_handler(1, 11, 0x04, key_q, 1);
    usb_keyboard_handler(1, 11, 0, no_keys, 0);
    assert(atomic_load(&output_count) == 2);

    atomic_store(&raw_mode, true);
    atomic_store(&raw_event_count, 0);
    usb_keyboard_handler(2, 22, 0x01, key_c, 1);
    const u8 key_left[1] = {0x50};
    usb_keyboard_handler(2, 22, 0x04, key_left, 1);
    usb_keyboard_handler(2, 22, 0, no_keys, 0);
    assert(atomic_load(&raw_event_count) == 2);
    assert(raw_events[0].keycode == 0x06 && raw_events[0].modifiers == 0x01 &&
           raw_events[0].codepoint == 'c');
    assert(raw_events[1].keycode == 0x50 && raw_events[1].modifiers == 0x04 &&
           raw_events[1].codepoint == 0);
    atomic_store(&raw_mode, false);

    /* Rapid deliberate taps of the same key remain two distinct ordinary
     * characters; stopping typematic must not erase either make event. */
    usb_keyboard_handler(1, 11, 0, key_a, 1);
    usb_keyboard_handler(1, 11, 0, no_keys, 0);
    usb_keyboard_handler(1, 11, 0, key_a, 1);
    usb_keyboard_handler(1, 11, 0, no_keys, 0);
    assert(atomic_load(&output_count) == 4);
    assert(output[2] == 'a' && output[3] == 'a');
    atomic_store(&fake_time, 20000); keyboard_update();
    assert(atomic_load(&output_count) == 4);

    /* Timer processing and xHCI reports overlap on the same source. The
     * source lock must keep report identity and repeat deadline coherent. */
    pthread_t reports, repeats;
    assert(!pthread_create(&reports, NULL, report_worker, NULL));
    assert(!pthread_create(&repeats, NULL, repeat_worker, NULL));
    assert(!pthread_join(reports, NULL));
    assert(!pthread_join(repeats, NULL));
    u64 before_remove = atomic_load(&output_count);
    usb_keyboard_remove(7, 77);
    atomic_store(&fake_time, UINT64_MAX / 2U);
    keyboard_update();
    assert(atomic_load(&output_count) == before_remove);

    /* A stale disconnect generation must not clear a later device instance. */
    usb_keyboard_handler(9, 90, 0, key_a, 1);
    usb_keyboard_remove(9, 89);
    atomic_store(&fake_time, UINT64_MAX / 2U + 1000U);
    keyboard_update();
    assert(atomic_load(&output_count) > before_remove);
    puts("keyboard input: repeat delay/release, concurrent HID/PIT updates, detach and generation checks passed");
}
'''

with tempfile.TemporaryDirectory(prefix='mangrove-keyboard-smp-') as directory:
    temp = Path(directory)
    stub = temp / 'spinlock.h'
    stub.write_text('#pragma once\n#include <pthread.h>\n#include <types.h>\n'
                    'typedef pthread_mutex_t spinlock_t;\n'
                    'void spinlock_init(spinlock_t *);\n'
                    'u64 spin_lock_irqsave(spinlock_t *);\n'
                    'void spin_unlock_irqrestore(spinlock_t *,u64);\n')
    cfile = temp / 'test.c'
    cfile.write_text(source + f'\n#include "{keyboard_path}"\n' + post)
    binary = temp / 'test'
    command = [sys.argv[1] if len(sys.argv) > 1 else 'cc', '-std=gnu17',
               '-Wall', '-Wextra', '-Werror', '-pthread',
               '-fsanitize=address,undefined', '-ffunction-sections',
               '-fdata-sections', '-I', str(temp), '-I', str(ROOT / 'kernel/include'),
               '-I', str(ROOT / 'include'), '-Wl,--gc-sections',
               str(cfile), '-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True, timeout=30,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
