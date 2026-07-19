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

void console_write_hex(u64 Value)
{
    CHAR16 Buffer[19];

    Buffer[0] = L'0';
    Buffer[1] = L'x';

    for (usize i = 2; i < 18; i++)
    {
        Buffer[i] = L'0';
    }

    Buffer[18] = L'\0';

    CHAR16 Digit;

    for (usize i = 0; i < 16; i++)
    {
        u8 Nibble = Value & 0xF;

        if (Nibble < 10)
        {
            Digit = L'0' + Nibble;
        }
        else
        {
            Digit = L'A' + (Nibble - 10);
        }

        Buffer[17 - i] = Digit;

        Value >>= 4;
    }

    console_write(Buffer);
}
