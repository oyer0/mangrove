#include <keyboard.h>
#include <console.h>
#include <irq.h>
#include <idt.h>
#include <io.h>
#include <stdbool.h>

static bool shift_pressed = false;

static const char scancode_set1[128] = {
    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',

    [0x0C] = '-',
    [0x0D] = '=',
    [0x1A] = '[',
    [0x1B] = ']',
    [0x27] = ';',
    [0x28] = '\'',
    [0x29] = '`',
    [0x2B] = '\\',
    [0x33] = ',',
    [0x34] = '.',
    [0x35] = '/',

    [0x10] = 'q',
    [0x11] = 'w',
    [0x12] = 'e',
    [0x13] = 'r',
    [0x14] = 't',
    [0x15] = 'y',
    [0x16] = 'u',
    [0x17] = 'i',
    [0x18] = 'o',
    [0x19] = 'p',

    [0x1E] = 'a',
    [0x1F] = 's',
    [0x20] = 'd',
    [0x21] = 'f',
    [0x22] = 'g',
    [0x23] = 'h',
    [0x24] = 'j',
    [0x25] = 'k',
    [0x26] = 'l',

    [0x2C] = 'z',
    [0x2D] = 'x',
    [0x2E] = 'c',
    [0x2F] = 'v',
    [0x30] = 'b',
    [0x31] = 'n',
    [0x32] = 'm',

    [0x39] = ' ',
    [0x1C] = '\n',
    [0x0E] = '\b',
};

static const char scancode_set1_shift[128] = {
    [0x02] = '!',
    [0x03] = '@',
    [0x04] = '#',
    [0x05] = '$',
    [0x06] = '%',
    [0x07] = '^',
    [0x08] = '&',
    [0x09] = '*',
    [0x0A] = '(',
    [0x0B] = ')',

    [0x0C] = '_',
    [0x0D] = '+',
    [0x1A] = '{',
    [0x1B] = '}',
    [0x27] = ':',
    [0x28] = '"',
    [0x29] = '~',
    [0x2B] = '|',
    [0x33] = '<',
    [0x34] = '>',
    [0x35] = '?',

    [0x10] = 'Q',
    [0x11] = 'W',
    [0x12] = 'E',
    [0x13] = 'R',
    [0x14] = 'T',
    [0x15] = 'Y',
    [0x16] = 'U',
    [0x17] = 'I',
    [0x18] = 'O',
    [0x19] = 'P',

    [0x1E] = 'A',
    [0x1F] = 'S',
    [0x20] = 'D',
    [0x21] = 'F',
    [0x22] = 'G',
    [0x23] = 'H',
    [0x24] = 'J',
    [0x25] = 'K',
    [0x26] = 'L',

    [0x2C] = 'Z',
    [0x2D] = 'X',
    [0x2E] = 'C',
    [0x2F] = 'V',
    [0x30] = 'B',
    [0x31] = 'N',
    [0x32] = 'M',

    [0x39] = ' ',
    [0x1C] = '\n',
    [0x0E] = '\b',
};

static void keyboard_irq_handler(struct cpu_registers *regs)
{
    (void)regs;

    u8 scancode = inb(0x60);

    /* Ignore extended keys for now. */
    if (scancode == 0xE0)
        return;

    /* Shift pressed. */
    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = true;
        return;
    }

    /* Shift released. */
    if (scancode == 0xAA || scancode == 0xB6) {
        shift_pressed = false;
        return;
    }

    /* Ignore key releases. */
    if (scancode & 0x80)
        return;

    const char *layout =
        shift_pressed ? scancode_set1_shift : scancode_set1;

    char c = layout[scancode];

    if (c)
        console_input(c);
}

void keyboard_init(void)
{
    irq_register_handler(1, keyboard_irq_handler);
}
