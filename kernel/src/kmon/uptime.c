#include <kmon/uptime.h>
#include <timer.h>
#include <kprint.h>

void kmon_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    u64 ticks = timer_ticks();
    u64 seconds = ticks / 1000;
    u64 milliseconds = ticks % 1000;

    kprint("Uptime: %llu.%03llu seconds\n", seconds, milliseconds);
}
