/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define main time_command_main
#include "../../userspace/time/main.c"
#undef main
static bool fail_spawn;
static usize calls, waits, closes;
static i32 child_status;
bool command_help_requested(int argc,char **argv){(void)argc;(void)argv;return false;}
int command_print_help(const char *name){(void)name;return 0;}
void command_usage_error(const char *name,const char *usage,const char *option)
{(void)name;(void)usage;(void)option;}
bool command_build_executable_path_alloc(const char *name,char **out)
{
 assert(!strcmp(name,"fixture") && out);*out=NULL;
 *out=malloc(sizeof("/bin/fixture"));assert(*out);
 strcpy(*out,"/bin/fixture");return true;
}
void command_path_release(char **path)
{if(path&&*path){free(*path);*path=NULL;}}
const char *error_string(mg_result_t r){(void)r;return "test error";}
bool result_is_error(mg_result_t r){return r<0;}
mg_result_t mg_clock_monotonic(mg_monotonic_time_t *out)
{out->milliseconds=calls*5U;return MG_OK;}
mg_result_t process_spawn_argv(const char *const *argv,usize argc)
{
 calls++;assert(argc==4096U && !strcmp(argv[0],"/bin/fixture"));
 for(usize i=1;i<argc;i++)assert(!strcmp(argv[i],i%2 ? "" : "中文 space"));
 return fail_spawn ? MG_ERR_NO_MEMORY : 77;
}
mg_result_t process_wait(mg_handle_t h,i32 *status)
{assert(h==77);waits++;*status=child_status;return MG_OK;}
mg_result_t handle_close(mg_handle_t h){assert(h==77);closes++;return MG_OK;}
int main(void)
{
 char **argv=malloc(4097U*sizeof(*argv));assert(argv);
 argv[0]="time";argv[1]="fixture";
 for(usize i=2;i<4097;i++)argv[i]=(i-1)%2 ? "" : "中文 space";
 assert(time_command_main(4097,argv)==0 && calls==1 && waits==1 && closes==1);
 assert(!strcmp(argv[1],"fixture"));
 fail_spawn=true;
 assert(time_command_main(4097,argv)==1 && waits==1 && closes==1);
 assert(!strcmp(argv[1],"fixture"));
 fail_spawn=false;child_status=17;
 assert(time_command_main(4097,argv)==17 && waits==2 && closes==2);
 free(argv);puts("time: large argv forwarding, parent restoration, launch failure and child status PASS");return 0;
}
