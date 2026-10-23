/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <console.h>
#include <terminal.h>
#include <scheduler.h>
#include <process.h>
#include <mangrove_errors.h>
#include <mg/utf8.h>
#include <spinlock.h>
#include <input_trace.h>

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
static bool raw_input_include_cooked;
static kernel_thread_t *raw_input_owner;
static kernel_thread_t *raw_input_waiter;
static spinlock_t input_lock;
#if XHCI_DEBUG
static u8 hid_queue_put_log_count;
static u8 hid_queue_get_log_count;
#endif

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
    input_trace_record(INPUT_TRACE_QUEUE, keycode, flags, raw_input_count);
}

/* Called with input_lock held. Queue a complete encoded scalar so a normal
 * byte reader never sees an interleaved or partially queued key. */
static void console_input_codepoint_locked(u32 codepoint)
{
    u8 encoded[4];
    usize length;

    if (codepoint == '\r') codepoint = '\n';
    if (mg_utf8_encode_one(codepoint, encoded, sizeof(encoded), &length) !=
        MG_UTF8_OK || input_count > CONSOLE_QUEUE_SIZE - length)
        return;
    for (usize index = 0; index < length; index++)
        console_queue_byte((char)encoded[index]);
    if (input_waiter) {
        kernel_thread_t *waiter = input_waiter;
        input_waiter = NULL;
        (void)scheduler_wake_waiter(waiter);
    }
}

void console_discard_key_repeats(u32 keycode)
{
    u64 saved_flags;
    u32 read_index;
    u32 write_index;
    u32 kept = 0;
    u32 queued;

    if (!keycode) return;
    saved_flags = spin_lock_irqsave(&input_lock);
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
    input_trace_record(INPUT_TRACE_CANCEL, keycode, queued - kept, kept);
    spin_unlock_irqrestore(&input_lock, saved_flags);
}

void console_init(void) {
    spinlock_init(&input_lock);
    input_head = 0;
    input_tail = 0;
    input_count = 0;
    input_waiter = NULL;
    raw_input_head = 0;
    raw_input_tail = 0;
    raw_input_count = 0;
    raw_input_enabled = false;
    raw_input_include_cooked = false;
    raw_input_owner = NULL;
    raw_input_waiter = NULL;
#if XHCI_DEBUG
    hid_queue_put_log_count = 0;
    hid_queue_get_log_count = 0;
#endif
}

void console_input(char c) {
    u64 saved_flags = spin_lock_irqsave(&input_lock);
#if XHCI_DEBUG
    bool log_input = false;
    u32 log_depth = 0;
#endif

    if (c == '\r') c = '\n';
    if (raw_input_enabled) {
        if (raw_input_count < CONSOLE_RAW_QUEUE_SIZE) {
            kernel_thread_t *waiter;

            console_queue_key_event(0, 0, (u32)(u8)c, 0);
            waiter = raw_input_waiter;
            raw_input_waiter = NULL;
            if (waiter) (void)scheduler_wake_waiter(waiter);
        }
    } else if (input_count < CONSOLE_QUEUE_SIZE) {
        kernel_thread_t *waiter;

        console_queue_byte(c);
#if XHCI_DEBUG
        if (hid_queue_put_log_count < 4) {
            log_input = true;
            log_depth = input_count;
            hid_queue_put_log_count++;
        }
#endif
        waiter = input_waiter;
        input_waiter = NULL;
        if (waiter) (void)scheduler_wake_waiter(waiter);
    }
    spin_unlock_irqrestore(&input_lock, saved_flags);
#if XHCI_DEBUG
    if (log_input) kprint("[HID-Q] put=%02x depth=%u\n", (u8)c, log_depth);
#endif
}

void console_input_key(u32 key)
{
    u64 saved_flags = spin_lock_irqsave(&input_lock);

    if (raw_input_enabled) {
        if (raw_input_count < CONSOLE_RAW_QUEUE_SIZE) {
            kernel_thread_t *waiter;

            console_queue_key_event(0, 0, key, 0);
            waiter = raw_input_waiter;
            raw_input_waiter = NULL;
            if (waiter) (void)scheduler_wake_waiter(waiter);
        }
    } else {
        console_input_codepoint_locked(key);
    }
    spin_unlock_irqrestore(&input_lock, saved_flags);
}

void console_input_key_event(u32 keycode, u32 modifiers, u32 codepoint)
{
    console_input_key_event_flags(keycode, modifiers, codepoint, 0);
}

void console_input_key_event_flags(u32 keycode, u32 modifiers,
                                   u32 codepoint, u32 flags)
{
    u64 saved_flags = spin_lock_irqsave(&input_lock);

    if (raw_input_enabled) {
        kernel_thread_t *waiter;

        console_queue_key_event(keycode, modifiers, codepoint, flags);
        waiter = raw_input_waiter;
        raw_input_waiter = NULL;
        if (waiter) (void)scheduler_wake_waiter(waiter);
    } else {
        console_input_codepoint_locked(codepoint);
    }
    spin_unlock_irqrestore(&input_lock, saved_flags);
}

bool console_raw_input_active(void)
{
    u64 saved_flags = spin_lock_irqsave(&input_lock);
    bool active = raw_input_enabled;
    spin_unlock_irqrestore(&input_lock, saved_flags);
    return active;
}

static bool console_set_raw_input_mode(bool enabled,
                                       struct kernel_thread *owner,
                                       bool preserve_text,
                                       bool include_cooked)
{
    u64 saved_flags = spin_lock_irqsave(&input_lock);

    if (enabled) {
        if (!owner || (raw_input_enabled && raw_input_owner != owner)) {
            spin_unlock_irqrestore(&input_lock, saved_flags);
            return false;
        }
        raw_input_head = 0;
        raw_input_tail = 0;
        raw_input_count = 0;
        raw_input_owner = owner;
        raw_input_include_cooked = include_cooked;
        raw_input_enabled = true;
        input_trace_raw(true);
    } else {
        if (preserve_text) {
            u32 index = raw_input_head;

            for (u32 count = 0; count < raw_input_count; count++) {
                terminal_key_event_t event = raw_input_queue[index];
                u32 non_text_modifiers =
                    TERMINAL_KEY_MODIFIER_LEFT_CTRL |
                    TERMINAL_KEY_MODIFIER_RIGHT_CTRL |
                    TERMINAL_KEY_MODIFIER_LEFT_ALT |
                    TERMINAL_KEY_MODIFIER_RIGHT_ALT |
                    TERMINAL_KEY_MODIFIER_LEFT_GUI |
                    TERMINAL_KEY_MODIFIER_RIGHT_GUI;
                bool text_event =
                    (event.modifiers & non_text_modifiers) == 0 &&
                    (event.codepoint >= 0x20U || event.codepoint == '\n' ||
                     event.codepoint == '\t' || event.codepoint == '\b' ||
                     event.codepoint == 0x7fU);

                if (text_event)
                    console_input_codepoint_locked(event.codepoint);
                index = (index + 1U) % CONSOLE_RAW_QUEUE_SIZE;
            }
        }
        raw_input_enabled = false;
        raw_input_include_cooked = false;
        input_trace_raw(false);
        raw_input_owner = NULL;
        if (raw_input_waiter) {
            kernel_thread_t *waiter = raw_input_waiter;
            raw_input_waiter = NULL;
            (void)scheduler_wake_waiter(waiter);
        }
        raw_input_head = 0;
        raw_input_tail = 0;
        raw_input_count = 0;
    }
    spin_unlock_irqrestore(&input_lock, saved_flags);
    return true;
}

bool console_set_raw_input(bool enabled, struct kernel_thread *owner)
{
    return console_set_raw_input_mode(enabled, owner, false, false);
}

bool console_set_raw_input_with_cooked_bytes(bool enabled,
                                             struct kernel_thread *owner)
{
    return console_set_raw_input_mode(enabled, owner, false, enabled);
}

bool console_set_raw_input_preserving_text(bool enabled,
                                           struct kernel_thread *owner)
{
    return console_set_raw_input_mode(enabled, owner, !enabled, false);
}

i64 console_read_key_event(u32 timeout_ms, terminal_key_event_t *event)
{
    kernel_thread_t *self = thread_current();

    if (!self || !event) return MG_ERR_ACCESS_DENIED;
    for (;;) {
        u64 saved_flags = spin_lock_irqsave(&input_lock);

        if (!raw_input_enabled || raw_input_owner != self) {
            spin_unlock_irqrestore(&input_lock, saved_flags);
            return MG_ERR_ACCESS_DENIED;
        }
        /* A normal-screen line editor may consume cooked type-ahead first as
         * byte-tagged events. Alternate-screen raw clients retain the
         * original event-only queue contract. */
        if (raw_input_include_cooked && input_count != 0) {
            event->version = 1U;
            event->keycode = 0;
            event->modifiers = 0;
            event->codepoint = (u8)input_queue[input_head];
            event->flags = TERMINAL_KEY_EVENT_COOKED_BYTE;
            input_head = (input_head + 1U) % CONSOLE_QUEUE_SIZE;
            input_count--;
            spin_unlock_irqrestore(&input_lock, saved_flags);
            return MG_OK;
        }
        if (raw_input_count != 0) {
            *event = raw_input_queue[raw_input_head];
            raw_input_head = (raw_input_head + 1U) % CONSOLE_RAW_QUEUE_SIZE;
            raw_input_count--;
            input_trace_record(INPUT_TRACE_READ, event->keycode,
                               event->flags, raw_input_count);
            spin_unlock_irqrestore(&input_lock, saved_flags);
            return MG_OK;
        }
        if (timeout_ms == 0U) {
            spin_unlock_irqrestore(&input_lock, saved_flags);
            return MG_ERR_WOULD_BLOCK;
        }
        if (raw_input_waiter && raw_input_waiter != self) {
            spin_unlock_irqrestore(&input_lock, saved_flags);
            return MG_ERR_BUSY;
        }
        raw_input_waiter = self;
        spin_unlock_irqrestore(&input_lock, saved_flags);

        /* Do not park while holding input_lock. The event producer publishes
         * the queue change under that lock and scheduler_wake_waiter closes
         * the gap between registration and this call. */
        bool woke = timeout_ms == CONSOLE_WAIT_FOREVER ?
            scheduler_block() : scheduler_sleep((u64)timeout_ms);
        scheduler_clear_event_wake(self);
        saved_flags = spin_lock_irqsave(&input_lock);
        if (raw_input_waiter == self) raw_input_waiter = NULL;
        bool still_owned = raw_input_enabled && raw_input_owner == self;
        bool has_event = raw_input_count != 0;
        spin_unlock_irqrestore(&input_lock, saved_flags);

        if (!woke) return MG_ERR_BUSY;
        if (!still_owned) return MG_ERR_ACCESS_DENIED;
        /* A timed wait whose wake left the queue empty is the timeout result.
         * If an event arrived concurrently, the next loop consumes it. */
        if (timeout_ms != CONSOLE_WAIT_FOREVER && !has_event)
            return MG_ERR_TIMEOUT;
    }
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

    if ((length && !buffer) || !thread_current()) return MG_ERR_BAD_ARGUMENT;
    if (length == 0) return 0;

    self = thread_current();

    for (;;) {
        u64 saved_flags = spin_lock_irqsave(&input_lock);

        if (raw_input_enabled) {
            spin_unlock_irqrestore(&input_lock, saved_flags);
            return MG_ERR_BUSY;
        }

        if (input_count != 0) {
            if (input_waiter == self) input_waiter = NULL;
            copied = length < input_count ? length : input_count;
            for (u64 i = 0; i < copied; i++) {
                out[i] = (u8)input_queue[input_head];
                input_head = (input_head + 1U) % CONSOLE_QUEUE_SIZE;
            }
            input_count -= (u32)copied;
#if XHCI_DEBUG
            bool log_input = copied && hid_queue_get_log_count < 4;
            u32 log_depth = input_count;
            if (log_input) hid_queue_get_log_count++;
#endif
            spin_unlock_irqrestore(&input_lock, saved_flags);
#if XHCI_DEBUG
            if (log_input)
                kprint("[HID-Q] get=%02x depth=%u\n", out[0], log_depth);
#endif
            return (i64)copied;
        }
        if (!self) {
            spin_unlock_irqrestore(&input_lock, saved_flags);
            return MG_ERR_CANCELLED;
        }
        if (input_waiter && input_waiter != self) {
            spin_unlock_irqrestore(&input_lock, saved_flags);
            return MG_ERR_WOULD_BLOCK;
        }
        input_waiter = self;
        spin_unlock_irqrestore(&input_lock, saved_flags);

        if (!scheduler_block()) {
            scheduler_clear_event_wake(self);
            saved_flags = spin_lock_irqsave(&input_lock);
            if (input_waiter == self) input_waiter = NULL;
            spin_unlock_irqrestore(&input_lock, saved_flags);
            return MG_ERR_CANCELLED;
        }
    }
}

u64 console_read_bytes(void *buffer, u64 length)
{
    i64 result = console_read_bytes_checked(buffer, length);
    return result > 0 ? (u64)result : 0;
}

void console_cancel_waiter(struct kernel_thread *thread)
{
    u64 saved_flags = spin_lock_irqsave(&input_lock);

    if (input_waiter == thread)
        input_waiter = NULL;
    if (raw_input_waiter == thread)
        raw_input_waiter = NULL;
    spin_unlock_irqrestore(&input_lock, saved_flags);
}
