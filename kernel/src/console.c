/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <console.h>
#include <terminal.h>
#include <scheduler.h>
#include <process.h>
#include <mangrove_errors.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#define CONSOLE_QUEUE_SIZE 2048U
#define CONSOLE_RAW_QUEUE_SIZE 64U
#define CONSOLE_WAIT_FOREVER 0xFFFFFFFFU

static char input_queue[CONSOLE_QUEUE_SIZE];
static u32 input_head;
static u32 input_tail;
static u32 input_count;
static kernel_thread_t *input_waiter;
static terminal_key_event_t raw_input_queue[CONSOLE_RAW_QUEUE_SIZE];
static u32 raw_input_head;
static u32 raw_input_tail;
static u32 raw_input_count;
static bool raw_input_enabled;
static kernel_thread_t *raw_input_owner;
static kernel_thread_t *raw_input_waiter;
#if XHCI_DEBUG
static u8 hid_queue_put_log_count;
static u8 hid_queue_get_log_count;
#endif

static u64 console_irq_save(void)
{
    u64 flags;

    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void console_irq_restore(u64 flags)
{
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
}

static void console_queue_byte(char c)
{
    if (input_count == CONSOLE_QUEUE_SIZE) return;
    input_queue[input_tail] = c;
    input_tail = (input_tail + 1U) % CONSOLE_QUEUE_SIZE;
    input_count++;
}

static void console_queue_key_event(u32 keycode, u32 modifiers,
                                    u32 codepoint, u32 flags)
{
    terminal_key_event_t *event;

    /* A slow raw client must not accumulate an unbounded typematic tail.
     * There is at most one pending repeat for a physical key; a later repeat
     * will be generated after the client consumes it. */
    if ((flags & TERMINAL_KEY_EVENT_REPEAT) != 0) {
        u32 index = raw_input_head;

        for (u32 count = 0; count < raw_input_count; count++) {
            terminal_key_event_t *queued = &raw_input_queue[index];

            if ((queued->flags & TERMINAL_KEY_EVENT_REPEAT) != 0 &&
                queued->keycode == keycode)
                return;
            index = (index + 1U) % CONSOLE_RAW_QUEUE_SIZE;
        }
    }
    if (raw_input_count == CONSOLE_RAW_QUEUE_SIZE) {
        return;
    }
    event = &raw_input_queue[raw_input_tail];
    event->version = 1U;
    event->keycode = keycode;
    event->modifiers = modifiers;
    event->codepoint = codepoint;
    event->flags = flags & TERMINAL_KEY_EVENT_REPEAT;
    raw_input_tail = (raw_input_tail + 1U) % CONSOLE_RAW_QUEUE_SIZE;
    raw_input_count++;
}

void console_discard_key_repeats(u32 keycode)
{
    u64 saved_flags;
    u32 read_index;
    u32 write_index;
    u32 kept = 0;
    u32 queued;

    if (!keycode) return;
    saved_flags = console_irq_save();
    read_index = raw_input_head;
    write_index = raw_input_head;
    queued = raw_input_count;
    for (u32 count = 0; count < queued; count++) {
        terminal_key_event_t event = raw_input_queue[read_index];

        read_index = (read_index + 1U) % CONSOLE_RAW_QUEUE_SIZE;
        if ((event.flags & TERMINAL_KEY_EVENT_REPEAT) != 0 &&
            event.keycode == keycode)
            continue;
        raw_input_queue[write_index] = event;
        write_index = (write_index + 1U) % CONSOLE_RAW_QUEUE_SIZE;
        kept++;
    }
    raw_input_tail = write_index;
    raw_input_count = kept;
    console_irq_restore(saved_flags);
}

void console_init(void) {
    input_head = 0;
    input_tail = 0;
    input_count = 0;
    input_waiter = NULL;
    raw_input_head = 0;
    raw_input_tail = 0;
    raw_input_count = 0;
    raw_input_enabled = false;
    raw_input_owner = NULL;
    raw_input_waiter = NULL;
#if XHCI_DEBUG
    hid_queue_put_log_count = 0;
    hid_queue_get_log_count = 0;
#endif
}

void console_input(char c) {
    u64 saved_flags = console_irq_save();

    if (c == '\r') c = '\n';
    if (raw_input_enabled) {
        if (raw_input_count < CONSOLE_RAW_QUEUE_SIZE) {
            kernel_thread_t *waiter;

            console_queue_key_event(0, 0, (u32)(u8)c, 0);
            waiter = raw_input_waiter;
            raw_input_waiter = NULL;
            if (waiter) (void)scheduler_unblock(waiter);
        }
    } else if (input_count < CONSOLE_QUEUE_SIZE) {
        kernel_thread_t *waiter;

        console_queue_byte(c);
#if XHCI_DEBUG
        if (hid_queue_put_log_count < 4) {
            kprint("[HID-Q] put=%02x depth=%u\n", (u8)c, input_count);
            hid_queue_put_log_count++;
        }
#endif
        waiter = input_waiter;
        input_waiter = NULL;
        if (waiter) {
            (void)scheduler_unblock(waiter);
        }
    }
    console_irq_restore(saved_flags);
}

void console_input_key(u32 key)
{
    u64 saved_flags = console_irq_save();

    if (raw_input_enabled) {
        if (raw_input_count < CONSOLE_RAW_QUEUE_SIZE) {
            kernel_thread_t *waiter;

            console_queue_key_event(0, 0, key, 0);
            waiter = raw_input_waiter;
            raw_input_waiter = NULL;
            if (waiter) (void)scheduler_unblock(waiter);
        }
    } else if (key <= 0xFFU) {
        /* Keep this helper safe for keyboard paths that do not need raw
         * mode; normal line input retains its existing byte semantics. */
        console_input((char)key);
        console_irq_restore(saved_flags);
        return;
    }
    console_irq_restore(saved_flags);
}

void console_input_key_event(u32 keycode, u32 modifiers, u32 codepoint)
{
    console_input_key_event_flags(keycode, modifiers, codepoint, 0);
}

void console_input_key_event_flags(u32 keycode, u32 modifiers,
                                   u32 codepoint, u32 flags)
{
    u64 saved_flags = console_irq_save();

    if (raw_input_enabled) {
        kernel_thread_t *waiter;

        console_queue_key_event(keycode, modifiers, codepoint, flags);
        waiter = raw_input_waiter;
        raw_input_waiter = NULL;
        if (waiter) (void)scheduler_unblock(waiter);
    } else if (codepoint <= 0xFFU) {
        console_input((char)codepoint);
        console_irq_restore(saved_flags);
        return;
    }
    console_irq_restore(saved_flags);
}

bool console_raw_input_active(void)
{
    return raw_input_enabled;
}

bool console_set_raw_input(bool enabled, struct kernel_thread *owner)
{
    u64 saved_flags = console_irq_save();

    if (enabled) {
        if (!owner || (raw_input_enabled && raw_input_owner != owner)) {
            console_irq_restore(saved_flags);
            return false;
        }
        raw_input_head = 0;
        raw_input_tail = 0;
        raw_input_count = 0;
        raw_input_owner = owner;
        raw_input_enabled = true;
    } else {
        raw_input_enabled = false;
        raw_input_owner = NULL;
        raw_input_waiter = NULL;
        raw_input_head = 0;
        raw_input_tail = 0;
        raw_input_count = 0;
    }
    console_irq_restore(saved_flags);
    return true;
}

i64 console_read_key_event(u32 timeout_ms, terminal_key_event_t *event)
{
    kernel_thread_t *self = thread_current();
    u64 saved_flags;
    bool waited = false;

    if (!self || !event) return MG_ERR_ACCESS_DENIED;
    saved_flags = console_irq_save();
    if (!raw_input_enabled || raw_input_owner != self) {
        console_irq_restore(saved_flags);
        return MG_ERR_ACCESS_DENIED;
    }

    while (raw_input_count == 0) {
        if (timeout_ms == 0U) {
            console_irq_restore(saved_flags);
            return MG_ERR_WOULD_BLOCK;
        }
        if (raw_input_waiter && raw_input_waiter != self) {
            if (raw_input_waiter->state == THREAD_STATE_TERMINATED) {
                raw_input_waiter = NULL;
            } else {
                console_irq_restore(saved_flags);
                return MG_ERR_BUSY;
            }
        }
        raw_input_waiter = self;
        waited = true;
        if (timeout_ms == CONSOLE_WAIT_FOREVER) {
            if (!scheduler_block()) {
                if (raw_input_waiter == self) raw_input_waiter = NULL;
                console_irq_restore(saved_flags);
                return MG_ERR_BUSY;
            }
        } else if (!scheduler_sleep((u64)timeout_ms)) {
            if (raw_input_waiter == self) raw_input_waiter = NULL;
            console_irq_restore(saved_flags);
            return MG_ERR_BUSY;
        }
        if (raw_input_waiter == self) raw_input_waiter = NULL;
        /* A key wakeup removes the sleep entry early.  A timer wakeup leaves
         * the queue empty and is therefore the defined timeout result. */
        if (raw_input_count == 0 && waited &&
            timeout_ms != CONSOLE_WAIT_FOREVER) {
            console_irq_restore(saved_flags);
            return MG_ERR_TIMEOUT;
        }
    }

    *event = raw_input_queue[raw_input_head];
    raw_input_head = (raw_input_head + 1U) % CONSOLE_RAW_QUEUE_SIZE;
    raw_input_count--;
    console_irq_restore(saved_flags);
    return MG_OK;
}

i64 console_read_key(u32 timeout_ms)
{
    terminal_key_event_t event;
    i64 result = console_read_key_event(timeout_ms, &event);

    if (result < 0) return result;
    return (i64)(event.codepoint ? event.codepoint : event.keycode);
}

i64 console_read_bytes_checked(void *buffer, u64 length)
{
    u8 *out = (u8 *)buffer;
    kernel_thread_t *self;
    u64 copied;
    u64 saved_flags;

    if ((length && !buffer) || !thread_current()) return MG_ERR_BAD_ARGUMENT;
    if (length == 0) return 0;

    self = thread_current();
    saved_flags = console_irq_save();

    while (input_count == 0) {
        if (!self) {
            console_irq_restore(saved_flags);
            return MG_ERR_CANCELLED;
        }
        if (input_waiter && input_waiter != self) {
            if (input_waiter->state == THREAD_STATE_TERMINATED) {
                input_waiter = NULL;
            } else {
                console_irq_restore(saved_flags);
                return MG_ERR_WOULD_BLOCK;
            }
        }
        input_waiter = self;
        if (!scheduler_block()) {
            if (input_waiter == self) input_waiter = NULL;
            console_irq_restore(saved_flags);
            return MG_ERR_CANCELLED;
        }
    }

    if (input_waiter == self) {
        input_waiter = NULL;
    }

    copied = length < input_count ? length : input_count;
    for (u64 i = 0; i < copied; i++) {
        out[i] = (u8)input_queue[input_head];
        input_head = (input_head + 1U) % CONSOLE_QUEUE_SIZE;
    }
    input_count -= (u32)copied;
#if XHCI_DEBUG
    if (copied && hid_queue_get_log_count < 4) {
        kprint("[HID-Q] get=%02x depth=%u\n", out[0], input_count);
        hid_queue_get_log_count++;
    }
#endif
    console_irq_restore(saved_flags);
    return (i64)copied;
}

u64 console_read_bytes(void *buffer, u64 length)
{
    i64 result = console_read_bytes_checked(buffer, length);
    return result > 0 ? (u64)result : 0;
}

void console_cancel_waiter(struct kernel_thread *thread)
{
    u64 saved_flags = console_irq_save();

    if (input_waiter == thread)
        input_waiter = NULL;
    if (raw_input_waiter == thread)
        raw_input_waiter = NULL;
    console_irq_restore(saved_flags);
}
