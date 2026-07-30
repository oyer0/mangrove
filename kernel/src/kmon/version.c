#include <kmon/version.h>
#include <version.h>
#include <kprint.h>

void kmon_version(int argc, char **argv) {
    (void)argc; (void)argv;
    kprint("Mangrove OS %s\n", MANGROVE_VERSION);
}
