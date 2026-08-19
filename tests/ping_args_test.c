#include <assert.h>

#include "userspace/ping/ping_args.h"

int main(void)
{
    u32 count;
    const char *host;
    char *literal[] = { "ping", "10.0.2.2" };
    char *named[] = { "ping", "-c", "8", "example.com" };
    char *long_named[] = { "ping", "--count", "2", "example.com" };
    char *zero[] = { "ping", "-c", "0", "example.com" };
    char *large[] = { "ping", "-c", "17", "example.com" };
    char *missing[] = { "ping", "-c", "4" };

    assert(ping_parse_arguments(2, literal, &count, &host));
    assert(count == PING_DEFAULT_COUNT && host == literal[1]);
    assert(ping_parse_arguments(4, named, &count, &host));
    assert(count == 8 && host == named[3]);
    assert(ping_parse_arguments(4, long_named, &count, &host));
    assert(count == 2 && host == long_named[3]);
    assert(!ping_parse_arguments(4, zero, &count, &host));
    assert(!ping_parse_arguments(4, large, &count, &host));
    assert(!ping_parse_arguments(3, missing, &count, &host));
    return 0;
}
