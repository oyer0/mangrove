#include <console.h>
#include <terminal.h>
#include <scheduler.h>
#include <process.h>
#include <kprint.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#define CONSOLE_QUEUE_SIZE 2048U

static char input_queue[CONSOLE_QUEUE_SIZE];
static u32 input_head;
static u32 input_tail;
static u32 input_count;
static kernel_thread_t *input_waiter;
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

void console_init(void) {
    input_head = 0;
    input_tail = 0;
    input_count = 0;
    input_waiter = NULL;
#if XHCI_DEBUG
    hid_queue_put_log_count = 0;
    hid_queue_get_log_count = 0;
#endif
}

void console_input(char c) {
    u64 saved_flags = console_irq_save();

    if (c == '\r') c = '\n';
    if (input_count < CONSOLE_QUEUE_SIZE) {
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

u64 console_read_bytes(void *buffer, u64 length)
{
    u8 *out = (u8 *)buffer;
    kernel_thread_t *self;
    u64 copied;
    u64 saved_flags;

    if ((length && !buffer) || !thread_current()) return 0;
    if (length == 0) return 0;

    self = thread_current();
    saved_flags = console_irq_save();

    while (input_count == 0) {
        if (!self) {
            console_irq_restore(saved_flags);
            return 0;
        }
        if (input_waiter && input_waiter != self) {
            if (input_waiter->state == THREAD_STATE_TERMINATED) {
                input_waiter = NULL;
            } else {
                console_irq_restore(saved_flags);
                return 0;
            }
        }
        input_waiter = self;
        if (!scheduler_block()) {
            if (input_waiter == self) input_waiter = NULL;
            console_irq_restore(saved_flags);
            return 0;
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
    return copied;
}
