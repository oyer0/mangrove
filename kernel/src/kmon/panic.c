#include <kmon/panic.h>
#include <panic.h>

void kmon_panic(int argc, char **argv) {
    (void)argc; (void)argv;
    panic("Triggered by user command 'panic'.");
}
