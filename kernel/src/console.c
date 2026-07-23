#include <console.h>
#include <terminal.h>
#include <shell/core.h>

static char input_buffer[256];
static usize input_length = 0;

void console_init(void) {
    input_length = 0;
}

void console_input(char c) {
    if (c == '\n') {
        input_buffer[input_length] = '\0';
        terminal_putc('\n');
        shell_execute(input_buffer);
        input_length = 0;
        return;
    }

    if (c == '\b') {
        if (input_length > 0) {
            input_length--;
            terminal_backspace();
        }

        return;
    }

    if (input_length < sizeof(input_buffer) - 1) {
        input_buffer[input_length++] = c;
        terminal_putc(c);
    }
}
