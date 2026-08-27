#include <stdio.h>
#include <mangrove_version.h>

int main(int argc, char **argv)
{
    if (argc != 1) {
        printf("Usage: version\n");
        return 1;
    }
    printf("%s %s %s %s %s\n",
           MANGROVE_NAME, MANGROVE_VERSION,
           PITH_NAME, MANGROVE_VERSION, MANGROVE_ARCH);
    return 0;
}
