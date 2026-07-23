#include <kmon/version.h>
#include <kprint.h>

void kmon_version(void)
{
    kprint("%s %s (%s)\n",
        MANGROVE_NAME,
        MANGROVE_VERSION,
        MANGROVE_ARCH
    );
}
