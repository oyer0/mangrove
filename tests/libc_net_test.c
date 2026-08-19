#include <assert.h>
#include <string.h>
#include <mg/net.h>

long mg_syscall(unsigned long number, unsigned long a, unsigned long b, unsigned long c)
{
    (void)number; (void)a; (void)b; (void)c;
    return -1;
}

int main(void)
{
    mg_ipv4_addr_t address;
    char text[16];
    assert(mg_ipv4_parse("10.0.2.15", &address));
    assert(address.octet[0] == 10 && address.octet[3] == 15);
    assert(mg_ipv4_format(&address, text, sizeof(text)));
    assert(strcmp(text, "10.0.2.15") == 0);
    assert(!mg_ipv4_parse("10.0.2.256", &address));
    assert(!mg_ipv4_parse("10.0.2", &address));
    assert(!mg_ipv4_parse("10.0.2.15x", &address));
    return 0;
}
