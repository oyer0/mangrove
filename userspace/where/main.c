#include <mangrove.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    char cwd[256];
    usize cwd_size = 0;
    mg_result_t result;

    if (argc != 1) {
        printf("Usage: where\n");
        return 1;
    }
    result = process_getcwd(cwd, sizeof(cwd), &cwd_size);
    if (result_is_error(result)) {
        printf("Could not read current directory: %s.\n", error_string(result));
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}
