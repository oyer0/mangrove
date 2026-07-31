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
#include <kmon/ahci.h>
#include <kmon/block.h>
#include <kmon/fs.h>
#include <kmon/scheduler.h>

#include <kprint.h>
#include <terminal.h>
#include <string.h>

static const builtin_t builtins[] = {
    { "help",    kmon_help    },
    { "version", kmon_version },
    { "clear",   kmon_clear   },
    { "mem",     kmon_mem     },
    { "uptime",  kmon_uptime  },
    { "scheduler", kmon_scheduler },
    { "heap",    kmon_heap    },
    { "panic",   kmon_panic   },
    { "pci",     kmon_pci     },
    { "ahci",    kmon_ahci    },
    { "block",   kmon_block   },
    { "pwd",     kmon_pwd     },
    { "cd",      kmon_cd      },
    { "ls",      kmon_ls      },
    { "cat",     kmon_cat     },
    { "touch",   kmon_touch   },
    { "mkdir",   kmon_mkdir   },
    { "write",   kmon_write   },
    { "rm",      kmon_rm      },
    { "rmdir",   kmon_rmdir   },
    { "mv",      kmon_mv      },
};

void kmon_init(void) {
    kprint("Welcome to Mangrove OS!\n");
    kprint("Type 'help' to get started.\n\n");
    kprint("%s > ", kmon_get_cwd());
}

void kmon_execute(const char *command) {
    if (!command || command[0] == '\0') {
        kprint("%s > ", kmon_get_cwd());
        return;
    }

    char buf[256];
    strncpy(buf, command, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *argv[16];
    int argc = 0;
    char *ptr = buf;

    while (*ptr && argc < 16) {
        while (*ptr == ' ' || *ptr == '\t') ptr++;
        if (*ptr == '\0') break;

        argv[argc++] = ptr;

        while (*ptr && *ptr != ' ' && *ptr != '\t') ptr++;
        if (*ptr) {
            *ptr++ = '\0';
        }
    }

    if (argc == 0) {
        kprint("%s > ", kmon_get_cwd());
        return;
    }

    usize count = sizeof(builtins) / sizeof(builtins[0]);
    for (usize i = 0; i < count; i++) {
        if (strcmp(argv[0], builtins[i].name) == 0) {
            builtins[i].handler(argc, argv);
            kprint("%s > ", kmon_get_cwd());
            return;
        }
    }

    kprint("Unknown command: %s\n", argv[0]);
    kprint("%s > ", kmon_get_cwd());
}
