/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../../userspace/fsstress/fsstress.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
int main(void)
{
    uint64_t seed=0;
    const uint64_t known[]={0xe220a8397b1dcdafULL,0x6e789e6aa1b965f4ULL,0x06c45d188009454fULL,0xf88bb8a8724c81ecULL};
    for(unsigned i=0;i<4;i++)assert(fs_random(&seed)==known[i]);
    unsigned char bytes[4];fs_payload(0,bytes,4);assert(!memcmp(bytes,"\xaf\xf4\x4f\xec",4));
    fs_plan *a=malloc(sizeof(*a)),*b=malloc(sizeof(*b));unsigned char *wire=malloc(FS_TRACE_BYTES);
    assert(a&&b&&wire);
    const uint64_t plans[]={0x0d8b88222527a794ULL,0x3c13cc4bd02570acULL,
                            0xa725611772fd3103ULL,0xe1596d499fd7b01fULL};
    for(unsigned profile=0;profile<4;profile++)for(unsigned s=0;s<16;s++) {
        assert(fs_generate(a,profile,s,96,0));assert(fs_generate(b,profile,s,96,0));assert(!memcmp(a,b,sizeof(*a)));
        size_t n=fs_encode(a,wire);assert(fs_decode(b,wire,n));assert(!memcmp(a,b,sizeof(*a)));
        if(!s){uint64_t hash=0;for(unsigned i=0;i<8;i++)hash|=(uint64_t)wire[32+i]<<(8*i);assert(hash==plans[profile]);}
        for(size_t at=0;at<n;at+=31){wire[at]^=1;assert(!fs_decode(b,wire,n));wire[at]^=1;}
        assert(!fs_decode(b,wire,n-1));
    }
    assert(!fs_generate(a,4,0,96,0) && !fs_generate(a,0,0,63,0) && !fs_generate(a,0,0,1025,0));
    assert(fs_generate(a,0,0,96,0));a->ops[5].offset=65536;assert(!fs_validate(a));
    free(wire);free(b);free(a);puts("fsstress independent core: PRNG vectors, all profiles, deterministic plans, checked traces and bounds PASS");
}
