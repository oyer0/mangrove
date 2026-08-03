#include <stdio.h>
#include <mangrove_version.h>
#include <kernel/include/version.h>
#include "version.h"

void shoot_print_version(void)
{
    printf("%s %s\nPart of %s\nLicense: %s\n",
           SHOOT_NAME, SHOOT_VERSION, MANGROVE_NAME, MANGROVE_LICENSE);
}

void shoot_print_system_version(void)
{
    printf("%s %s\nKernel: %s %s\nShell: %s %s\nArchitecture: %s\nLicense: %s\n",
           MANGROVE_NAME, MANGROVE_VERSION,
           RHIZOME_NAME, RHIZOME_VERSION,
           SHOOT_NAME, SHOOT_VERSION,
           RHIZOME_ARCH, MANGROVE_LICENSE);
}
