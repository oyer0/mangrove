/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <spinlock.h>

#ifndef E1000_DRIVER_SOURCE
#define E1000_DRIVER_SOURCE "../../kernel/src/net/e1000.c"
#endif
#include E1000_DRIVER_SOURCE

static u8 mmio[2][E1000_MMIO_SIZE];
static e1000_rx_descriptor_t rx_ring[2][E1000_RX_RING_COUNT];
static u8 rx_buffer[2][E1000_BUFFER_SIZE];
static u32 delivered_frames;

u64 spin_lock_irqsave(spinlock_t *lock)
{
    (void)lock;
    return 0;
}

void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    (void)lock;
    (void)flags;
}

void net_receive_frame(net_device_t *device, const u8 *frame, usize length)
{
    assert(device == &controllers[0].device);
    assert(frame == rx_buffer[0]);
    assert(length == NET_ETHERNET_HEADER_SIZE);
    delivered_frames++;

    /* Run the production link-poll path at the receive callback boundary,
     * modeling another CPU's lifecycle worker. The old shared context was
     * cleared by this path before receive accounting resumed. */
    e1000_poll_links();
}

bool net_device_set_link(net_device_t *device, bool known, bool up)
{
    (void)device;
    (void)known;
    (void)up;
    return false;
}

int main(void)
{
    const u32 status_reg = E1000_REG_STATUS / sizeof(u32);
    const u32 cause_reg = E1000_REG_ICR / sizeof(u32);
    const u32 tail_reg = E1000_REG_RDT / sizeof(u32);

    memset(controllers, 0, sizeof(controllers));
    memset(mmio, 0, sizeof(mmio));
    memset(rx_ring, 0, sizeof(rx_ring));

    controllers[0].active = true;
    controllers[0].mmio = mmio[0];
    controllers[0].rx_ring = rx_ring[0];
    controllers[0].rx_buffer[0] = rx_buffer[0];
    controllers[0].rx_next = 0;
    controllers[1].active = true;
    controllers[1].mmio = mmio[1];
    controllers[1].rx_ring = rx_ring[1];
    controllers[1].rx_next = 0;
    assert(irq_gate_open(&controllers[0].irq_gate));
    assert(irq_gate_open(&controllers[1].irq_gate));

    /* A sentinel proves the nested helper does not acknowledge or recycle
     * controller 0's descriptor through controller 1's MMIO mapping. */
    ((volatile u32 *)mmio[1])[tail_reg] = 0xA5A5U;
    ((volatile u32 *)mmio[0])[status_reg] = E1000_STATUS_LU;
    ((volatile u32 *)mmio[0])[cause_reg] = E1000_INT_RXT0;
    rx_ring[0][0].length = NET_ETHERNET_HEADER_SIZE;
    rx_ring[0][0].status = E1000_RX_STATUS_DD | E1000_RX_STATUS_EOP;

    e1000_irq_handler(NULL);

    assert(delivered_frames == 1U);
    assert(controllers[0].received_frames == 1U);
    assert(controllers[0].rx_next == 1U);
    assert(rx_ring[0][0].status == 0U);
    assert(((volatile u32 *)mmio[0])[tail_reg] == 0U);
    assert(controllers[1].received_frames == 0U);
    assert(controllers[1].rx_next == 0U);
    assert(((volatile u32 *)mmio[1])[tail_reg] == 0xA5A5U);
    e1000_irq_quiesce(&controllers[0]);
    e1000_irq_quiesce(&controllers[1]);

    puts("e1000 receive keeps controller context across interleaving");
    return 0;
}
