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
static u8 hid_queue_put_log_count;
static u8 hid_queue_get_log_count;

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
    hid_queue_put_log_count = 0;
    hid_queue_get_log_count = 0;
}

void console_input(char c) {
    if (c == '\r') c = '\n';
    if (input_count < CONSOLE_QUEUE_SIZE) {
        console_queue_byte(c);
        if (hid_queue_put_log_count < 4) {
            kprint("[HID-Q] put=%02x depth=%u\n", (u8)c, input_count);
            hid_queue_put_log_count++;
        }
        if (input_waiter) {
            kernel_thread_t *waiter = input_waiter;
            input_waiter = NULL;
            (void)scheduler_unblock(waiter);
        }
    }
}

u64 console_read_bytes(void *buffer, u64 length)
{
    u8 *out = (u8 *)buffer;
    kernel_thread_t *self;
    u64 copied;

    if ((length && !buffer) || !thread_current()) return 0;
    if (length == 0) return 0;

    self = thread_current();

    while (input_count == 0) {
        if (!self) return 0;
        if (input_waiter && input_waiter != self) {
            if (input_waiter->state == THREAD_STATE_TERMINATED) {
                input_waiter = NULL;
            } else {
                return 0;
            }
        }
        input_waiter = self;
        if (!scheduler_block()) {
            if (input_waiter == self) input_waiter = NULL;
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
    if (copied && hid_queue_get_log_count < 4) {
        kprint("[HID-Q] get=%02x depth=%u\n", out[0], input_count);
        hid_queue_get_log_count++;
    }
    return copied;
}
