/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <bootinfo.h>
#include <uefi.h>

#define BOOT_GOP_SELECTION_MODE_LIMIT 64U

/* Select the highest-area validated mode supported by Pith's framebuffer
 * renderer. A valid firmware mode remains the fallback if switching fails.
 * The final active mode and geometry are copied to boot_info on success. */
EFI_STATUS boot_gop_select_mode(EFI_SYSTEM_TABLE *system_table,
                                EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                                BOOT_INFO *boot_info);
