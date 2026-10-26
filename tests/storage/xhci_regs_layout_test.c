/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <xhci_regs.h>

int main(void)
{
    volatile xhci_op_regs_t registers = {0};
    volatile u32 *command = &registers.usbcmd;
    volatile u32 *status = &registers.usbsts;

    return ((uintptr_t)command & 3U) != 0U ||
           ((uintptr_t)status & 3U) != 0U ||
           (uintptr_t)status - (uintptr_t)command != 4U;
}
