/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHILD_COUNT 4096U
#define LONG_BYTES (2U * 1024U * 1024U)
#define FILE_COUNT 512U
static void require(bool ok, const char *stage)
{ if (!ok) { printf("ARGUMENTS FAIL %s\n", stage); exit(1); } }
static void run(const char *const *argv, usize argc)
{
    mg_result_t result = process_spawn_argv(argv, argc);
    require(result > 0, "spawn");
    i32 status = -1;
    require(process_wait((mg_handle_t)result, &status) == MG_OK, "wait");
    require(handle_close((mg_handle_t)result) == MG_OK, "close child");
    require(status == 0, "child status");
}
static void filename(char *out, usize capacity, unsigned index)
{ (void)snprintf(out, capacity, "/temp/argv-files/f%04u space café.tmp", index); }
static int child(int argc, char **argv)
{
    require(argc == CHILD_COUNT && argv[argc] == NULL, "argc and sentinel");
    require(strlen(argv[2]) == LONG_BYTES, "long length");
    for (usize i=0;i<LONG_BYTES;i++) require(argv[2][i]=='x', "long bytes");
    for (unsigned i=3;i<CHILD_COUNT;i++)
        require(!strcmp(argv[i], i%2 ? "" : "space\n中文 café"), "argument boundaries");
    puts("ARGUMENTS native argc=4096 long=2097152 PASS");
    return 0;
}
static void verify_files(void)
{
    mg_path_info_t info;
    require(path_info("/temp/argv-files/.keep",&info)==MG_OK, "hidden file");
    require(path_info("/temp/argv-tree/.keep",&info)==MG_OK, "hidden tree");
    for(unsigned i=0;i<FILE_COUNT;i++) {
        char path[256]; filename(path,sizeof(path),i);
        require(path_info(path,&info)==MG_ERR_NOT_FOUND,"deleted file");
    }
    for(unsigned d=0;d<16U;d++) {
        char path[256]; (void)snprintf(path,sizeof(path),"/temp/argv-tree/d%02u",d);
        require(path_info(path,&info)==MG_ERR_NOT_FOUND,"deleted tree");
    }
    puts("ARGUMENTS deletion files=512 trees=16 hidden=preserved PASS");
}
int main(int argc,char **argv)
{
    if(argc>1 && !strcmp(argv[1],"--child")) return child(argc,argv);
    if(argc>1 && !strcmp(argv[1],"verify-glob")) {
        require(argc==FILE_COUNT+2U && argv[argc]==NULL,"glob argc");
        for(unsigned i=0;i<FILE_COUNT;i++) {
            char path[256];filename(path,sizeof(path),i);
            require(!strcmp(argv[i+2U],path),"glob sorted UTF-8 filenames");
        }
        puts("ARGUMENTS Shoot glob=512 sorted UTF-8 PASS");return 0;
    }
    if(argc>1 && !strcmp(argv[1],"verify-files")){verify_files();return 0;}
    require(argc==1,"parent invocation");
    require(process_spawn_argv(NULL,0)==MG_ERR_BAD_ARGUMENT,"zero args");
    require(process_spawn_argv((const char *const *)0x1000U,1)==MG_ERR_BAD_ARGUMENT,"bad vector");
    const char *bad[]={"/bin/argvtest",(const char *)0x1000U};
    require(process_spawn_argv(bad,2)==MG_ERR_BAD_ARGUMENT,"bad string");
    require(process_spawn_argv(bad,~(usize)0)==MG_ERR_BAD_ARGUMENT,"overflow count");
    require(process_spawn_argv((const char *const *)(uintptr_t)(~(uintptr_t)0-8U),2)==MG_ERR_BAD_ARGUMENT,"overflow vector");
    const char *absent[]={"/bin/no-such-argv-test"};
    for(unsigned i=0;i<32U;i++)require(process_spawn_argv(absent,1)<0,"failed executable");
    const char **vector=malloc(CHILD_COUNT*sizeof(*vector));
    char *large=malloc(LONG_BYTES+1U);require(vector && large,"parent allocation");
    memset(large,'x',LONG_BYTES);large[LONG_BYTES]=0;
    vector[0]="/bin/argvtest";vector[1]="--child";vector[2]=large;
    for(unsigned i=3;i<CHILD_COUNT;i++)vector[i]=i%2 ? "" : "space\n中文 café";
    run(vector,CHILD_COUNT);free(large);free(vector);
    require(directory_create("/temp/argv-files")==MG_OK,"create files directory");
    require(file_create("/temp/argv-files/.keep")==MG_OK,"create hidden file");
    for(unsigned i=0;i<FILE_COUNT;i++) {
        char path[256];filename(path,sizeof(path),i);
        require(file_create(path)==MG_OK,"create glob file");
    }
    require(directory_create("/temp/argv-tree")==MG_OK,"create tree directory");
    require(file_create("/temp/argv-tree/.keep")==MG_OK,"create hidden tree");
    for(unsigned d=0;d<16U;d++) {
        char path[256];(void)snprintf(path,sizeof(path),"/temp/argv-tree/d%02u",d);
        require(directory_create(path)==MG_OK,"create subtree");
        for(unsigned f=0;f<16U;f++) {
            (void)snprintf(path,sizeof(path),"/temp/argv-tree/d%02u/f%02u",d,f);
            require(file_create(path)==MG_OK,"create tree file");
        }
    }
    static const char script[]=
        "/bin/argvtest verify-glob /temp/argv-files/*.tmp\n"
        "del /temp/argv-files/*.tmp\n"
        "del -r /temp/argv-tree/*\n"
        "/bin/argvtest verify-files\n";
    mg_handle_t script_handle;
    require(file_create_contents("/temp/argv-workflow.shoot",script,sizeof(script)-1U,&script_handle)==MG_OK,"write script");
    require(handle_close(script_handle)==MG_OK,"close script");
    const char *shell[]={"/bin/shoot","/temp/argv-workflow.shoot"};run(shell,2);
    puts("ARGUMENTS complete PASS");return 0;
}
