#include <console.h>

static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;

void console_init(EFI_SYSTEM_TABLE *SystemTable)
{
    ConOut = SystemTable->ConOut;
}

void console_write(const CHAR16 *String)
{
    ConOut->OutputString(ConOut, (CHAR16 *)String);
}
