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

void console_clear(void)
{
    ConOut->ClearScreen(ConOut);
}

void console_set_attribute(usize Attribute)
{
    ConOut->SetAttribute(ConOut, Attribute);
}

void console_set_cursor(usize Column, usize Row)
{
    ConOut->SetCursorPosition(ConOut, Column, Row);
}

void console_enable_cursor(u8 Visible)
{
    ConOut->EnableCursor(ConOut, Visible);
}
