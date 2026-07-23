#include <shell/version.h>
#include <kprint.h>

void shell_version(void)
{
    kprint("%s %s (%s)\n",
        MANGROVE_NAME,
        MANGROVE_VERSION,
        MANGROVE_ARCH
    );
}
