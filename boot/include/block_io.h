/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <uefi.h>

EFI_STATUS block_io_read_blocks(EFI_BOOT_SERVICES *boot_services,
                                EFI_BLOCK_IO_PROTOCOL *block_io,
                                u32 media_id, u64 lba,
                                usize buffer_size, void *buffer);
