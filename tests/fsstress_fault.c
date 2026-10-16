/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Controlled host transport errors; real POSIX namespace and byte I/O remain.
 * No native recovery-owner guarantee is inferred from this fixture. */
#include "../userspace/fsstress/fsstress.h"
#include <stdlib.h>
#include <string.h>
extern fs_result fs_transport_write(int64_t,const void *,uint32_t);
extern fs_result fs_transport_read(int64_t,void *,uint32_t);
static unsigned writes,reads;
fs_result io_write(int64_t h,const void *data,uint32_t count)
{
    const char *mode=getenv("FSSTRESS_FAULT");
    if(!writes++ && mode) {
        if(!strcmp(mode,"memory"))return (fs_result){-6,0};
        if(!strcmp(mode,"space"))return (fs_result){-36,0};
        if(!strcmp(mode,"io"))return (fs_result){FS_IO,0};
        if(!strcmp(mode,"short") || !strcmp(mode,"uncertain")) {
            fs_result actual=fs_transport_write(h,data,count/2);
            if(actual.status)return actual;
            return !strcmp(mode,"uncertain")?(fs_result){FS_UNCERTAIN,0}:actual;
        }
    }
    return fs_transport_write(h,data,count);
}
fs_result io_read(int64_t h,void *data,uint32_t count)
{
    const char *mode=getenv("FSSTRESS_FAULT");
    if(count>=513 && !reads++ && mode && !strcmp(mode,"read-error"))return (fs_result){FS_IO,0};
    return fs_transport_read(h,data,count);
}
