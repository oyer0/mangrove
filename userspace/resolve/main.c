#include <mangrove.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    mg_ipv4_addr_t address;
    char text[16];
    mg_result_t result;
    if (argc == 2 && argv[1] && !strcmp(argv[1], "--help")) {
        printf("Usage: resolve <hostname>\nResolve an IPv4 address.\n");
        return 0;
    }
    if (argc != 2 || !argv[1] || !argv[1][0] ||
        (argv[1][0] == '-' && argv[1][1])) {
        printf("Usage: resolve <hostname>\n");
        return 1;
    }
    result = mg_net_resolve_a(argv[1], &address, MG_NET_TIMEOUT_DEFAULT);
    if (result < 0) {
        printf("Could not resolve \"%s\": %s.\n", argv[1],
               result == MG_ERR_TIMEOUT ? "request timed out" : "name not found");
        return 1;
    }
    if (!mg_ipv4_format(&address, text, sizeof(text))) return 1;
    printf("%s\n  %s\n", argv[1], text);
    return 0;
}
