/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Production command helpers. No filesystem or successful syscall is mocked. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <mangrove.h>
#include "../../userspace/common/path.h"
bool result_is_error(mg_result_t result){return result<0;}
mg_result_t process_get_identity(mg_identity_t *out)
{
    memset(out,0,sizeof(*out));out->uid=1000;
    strcpy(out->home,"/users/test");return MG_OK;
}
/* There is deliberately no getcwd implementation: relative authority must
 * never be converted to a pathname by these helpers. */
int main(void)
{
    const char *paths[]={".","..","../file","missing/../file","/missing/../file",
                         "a//./b/","/","relative","./native-app"};
    char out[256];
    for(usize i=0;i<sizeof(paths)/sizeof(paths[0]);i++){
        assert(command_resolve_path(paths[i],out,sizeof(out)));
        assert(!strcmp(out,paths[i]));
    }
    assert(!command_resolve_path("",out,sizeof(out)));
    assert(!command_resolve_path("long",out,4));
    assert(command_build_executable_path("./a",out,4)&&!strcmp(out,"./a"));
    assert(command_build_executable_path("fstest",out,sizeof(out))&&!strcmp(out,"/bin/fstest"));
    assert(command_build_executable_path("./app",out,sizeof(out))&&!strcmp(out,"./app"));
    assert(command_build_executable_path("../app",out,sizeof(out))&&!strcmp(out,"../app"));
    assert(command_build_executable_path("sub/app",out,sizeof(out))&&!strcmp(out,"sub/app"));
    assert(command_expand_home_path("~/x",true,out,sizeof(out))&&!strcmp(out,"/users/test/x"));
    puts("Command paths preserve relative authority and exact native traversal; explicit executable paths remain anchored");
    return 0;
}
