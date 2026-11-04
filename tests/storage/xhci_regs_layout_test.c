/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <xhci_regs.h>

int main(void)
{
    volatile xhci_op_regs_t registers = {0};
    volatile u32 *command = &registers.usbcmd;
    volatile u32 *status = &registers.usbsts;
    u32 next_offset = 0;

    return ((uintptr_t)command & 3U) != 0U ||
           ((uintptr_t)status & 3U) != 0U ||
           (uintptr_t)status - (uintptr_t)command != 4U ||
           XHCI_MMIO_APERTURE_SIZE != 0x40000U ||
           XHCI_MMIO_BASE_APERTURE_SIZE != 0x4000U ||
           (XHCI_HCCPARAMS1_XECP(0xFFFF0000U) << 2) !=
               XHCI_MMIO_APERTURE_SIZE - XHCI_EXT_CAP_HEADER_SIZE ||
           !xhci_mmio_base_range_valid(0x3FFCU,
                                       XHCI_EXT_CAP_HEADER_SIZE) ||
           xhci_mmio_base_range_valid(0x4000U,
                                      XHCI_EXT_CAP_HEADER_SIZE) ||
           !xhci_ext_cap_range_valid(0x4000U,
                                     XHCI_EXT_CAP_HEADER_SIZE) ||
           !xhci_ext_cap_range_valid(0x3fffCU,
                                    XHCI_EXT_CAP_HEADER_SIZE) ||
           xhci_ext_cap_range_valid(0x3fffCU, 2U * sizeof(u32)) ||
           xhci_ext_cap_range_valid(0x40000U,
                                    XHCI_EXT_CAP_HEADER_SIZE) ||
           xhci_ext_cap_range_valid(0x101U,
                                    XHCI_EXT_CAP_HEADER_SIZE) ||
           !xhci_ext_cap_next_offset(0x1000U, 0x20U, &next_offset) ||
           next_offset != 0x1080U ||
           xhci_ext_cap_next_offset(0x3ff00U, 0x40U, &next_offset) ||
           xhci_ext_cap_next_offset(0x1000U, 0U, &next_offset);
}
