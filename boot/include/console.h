// console.h

#pragma once

#include <uefi.h>

void console_init(EFI_SYSTEM_TABLE *SystemTable);
void console_write(const CHAR16 *String);
void console_clear(void);
