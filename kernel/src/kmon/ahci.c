#include <kmon/ahci.h>
#include <ahci.h>
#include <kprint.h>

void kmon_ahci(int argc, char **argv) {
    (void)argc; (void)argv;
    kprint("AHCI Controller Status:\n");
    if (ahci_present()) {
        kprint("  Status: Active\n");
    } else {
        kprint("  Status: Not Present\n");
    }
}
