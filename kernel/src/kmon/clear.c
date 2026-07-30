#include <kmon/clear.h>
#include <terminal.h>

void kmon_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    terminal_clear();
}
