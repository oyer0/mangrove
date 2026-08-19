#include <assert.h>
#include <string.h>

/* Pull the userspace parser into this host test without exercising the
 * syscall/networking entry point.  Garbage-collecting unused sections keeps
 * the application entry path out of the test link. */
#define main fetch_program_main
#include "../userspace/fetch/main.c"
#undef main

int main(void)
{
    static const u8 response[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: gzip, Chunked\r\n"
        "X-Test: yes\r\n\r\n"
        "5\r\nhello\r\n"
        "9;extension=value\r\n from web\r\n"
        "0\r\nTrailer: ignored\r\n\r\n";
    u8 body[64];
    usize body_length = 0;
    u32 status = 0;

    assert(parse_response(response, sizeof(response) - 1U,
                           body, sizeof(body), &body_length, &status));
    assert(status == 200);
    assert(body_length == 14);
    assert(memcmp(body, "hello from web", body_length) == 0);
    return 0;
}
