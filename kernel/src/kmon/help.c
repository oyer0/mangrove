#include <kmon/help.h>
#include <kprint.h>

void shell_help(void){
    kprint("Available commands:\n\n");
    kprint("help     Show this help message\n");
    kprint("version  Display Mangrove version\n");
    kprint("clear    Clear the terminal\n");
    kprint("mem      Show memory information\n");
    kprint("uptime   Show system uptime\n");
}
