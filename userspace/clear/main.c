#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc != 1) {
        printf("Usage: clear\n");
        return 1;
    }
    printf("\x1b[2J");
    return 0;
}
