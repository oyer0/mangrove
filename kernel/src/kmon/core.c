#include <kmon/core.h>
#include <kmon/builtin.h>

#include <kmon/help.h>
#include <kmon/version.h>
#include <kmon/clear.h>
#include <kmon/mem.h>
#include <kmon/uptime.h>
#include <kmon/heap.h>
#include <kmon/panic.h>
#include <kmon/pci.h>

#include <kprint.h>
#include <terminal.h>

#include <string.h>

static const builtin_t builtins[] = {
    { "help",    kmon_help    },
    { "version", kmon_version },
    { "clear",   kmon_clear   },
    { "mem",     kmon_mem     },
    { "uptime",  kmon_uptime  },
    { "heap",    kmon_heap    },
    { "panic",   kmon_panic   },
    { "pci",     kmon_pci     },

};

void kmon_init(void) {
    kprint("Welcome to Mangrove OS!\n");
    kprint("Type 'help' to get started.\n\n");

    kprint("> ");
}

void kmon_execute(const char *command)
{
    usize count = sizeof(builtins) / sizeof(builtins[0]);

    for (usize i = 0; i < count; i++) {
        if (strcmp(command, builtins[i].name) == 0) {
            builtins[i].handler();
            kprint("> ");
            return;
        }
    }

    kprint("Unknown command.\n");
    kprint("> ");
}
