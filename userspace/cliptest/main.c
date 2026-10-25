/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Explicitly staged native clipboard qualification; not a normal image app. */
#include <mangrove.h>
#include <mg/clipboard.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LENGTH (8U * 1024U * 1024U)
static const u8 pattern[8] = {0xF0,0x9F,0x8C,0xB1,'\n',' ','\t',0};
static u8 pattern_byte(usize position)
{
    return position<5 || position>=LENGTH-3 ? 'x' : pattern[(position-5)%8];
}
#define REQUIRE(condition) do { if (!(condition)) { \
    printf("cliptest: FAIL line %u\n", (unsigned)__LINE__); return 1; } } while (0)
static int verify(mg_clipboard_transfer_t *transfer)
{
    char bytes[8192];usize offset=0;
    REQUIRE(transfer->size==LENGTH);
    while(offset<LENGTH) {
        usize got;
        REQUIRE(clipboard_read(transfer,bytes,sizeof(bytes),&got)==MG_OK && got);
        for(usize i=0;i<got;i++) REQUIRE((u8)bytes[i]==pattern_byte(offset+i));
        offset+=got;
    }
    REQUIRE(clipboard_transfer_close(transfer)==MG_OK);
    return 0;
}
static int marker(const char *path)
{
    mg_handle_t handle;
    REQUIRE(file_create_contents(path,"ready",5,&handle)==MG_OK);
    REQUIRE(handle_close(handle)==MG_OK);return 0;
}
static int wait_marker(const char *path)
{
    u64 deadline=uptime_ms()+10000;
    mg_path_info_t info;
    while(uptime_ms()<deadline) {
        mg_result_t result=path_info(path,&info);
        if(result==MG_OK)return 0;
        REQUIRE(result==MG_ERR_NOT_FOUND);
        REQUIRE(process_sleep_ms(1)==MG_OK);
    }
    REQUIRE(false);return 1;
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"publish")) {
        char *bytes=malloc(LENGTH);REQUIRE(bytes);
        for(usize i=0;i<LENGTH;i++)bytes[i]=pattern_byte(i);
        u64 begin=uptime_ms();
        REQUIRE(clipboard_set_text(bytes,LENGTH)==MG_OK);
        printf("cliptest: published %u bytes in %llu ms\n",LENGTH,
               (unsigned long long)(uptime_ms()-begin));free(bytes);return 0;
    }
    if(argc==2 && !strcmp(argv[1],"pinned")) {
        mg_clipboard_transfer_t transfer;
        REQUIRE(clipboard_read_begin(MG_CLIPBOARD_TEXT,&transfer)==MG_OK);
        REQUIRE(marker("/temp/clip-ready")==0);
        REQUIRE(wait_marker("/temp/clip-go")==0);
        return verify(&transfer);
    }
    REQUIRE(argc==2 && !strcmp(argv[1],"suite"));
    u64 publication_start=uptime_ms();
    const char *publish_argv[]={"/bin/cliptest","publish"};
    mg_result_t child=process_spawn_argv(publish_argv,
        sizeof(publish_argv)/sizeof(publish_argv[0]));i32 status;
    REQUIRE(child>0 && process_wait(child,&status)==MG_OK && status==0);
    REQUIRE(handle_close(child)==MG_OK);
    printf("cliptest: publisher spawn, 8-MiB upload and exit in %llu ms\n",
           (unsigned long long)(uptime_ms()-publication_start));
    mg_clipboard_transfer_t transfer;
    REQUIRE(clipboard_read_begin(MG_CLIPBOARD_TEXT,&transfer)==MG_OK);
    u64 begin=uptime_ms();REQUIRE(verify(&transfer)==0);
    printf("cliptest: read %u bytes after publisher exit in %llu ms\n",LENGTH,
           (unsigned long long)(uptime_ms()-begin));
    const char *pinned_argv[]={"/bin/cliptest","pinned"};
    child=process_spawn_argv(pinned_argv,
        sizeof(pinned_argv)/sizeof(pinned_argv[0]));REQUIRE(child>0);
    REQUIRE(wait_marker("/temp/clip-ready")==0);
    REQUIRE(clipboard_set_text("replacement",11)==MG_OK);
    REQUIRE(marker("/temp/clip-go")==0);
    REQUIRE(process_wait(child,&status)==MG_OK && status==0);
    REQUIRE(handle_close(child)==MG_OK);
    char *text;usize size;
    REQUIRE(clipboard_get_text(11,&text,&size)==MG_OK && size==11 &&
            !memcmp(text,"replacement",11));free(text);
    REQUIRE(clipboard_read_begin("image/png",&transfer)==MG_ERR_UNSUPPORTED);
    REQUIRE(clipboard_publish_begin(MG_CLIPBOARD_TEXT,100,&transfer)==MG_OK);
    REQUIRE(clipboard_write(&transfer,"incomplete",10)==MG_OK);
    REQUIRE(clipboard_transfer_close(&transfer)==MG_OK);
    REQUIRE(clipboard_get_text(11,&text,&size)==MG_OK && size==11 &&
            !memcmp(text,"replacement",11));free(text);
    REQUIRE(clipboard_set_text("",0)==MG_OK);
    REQUIRE(clipboard_get_text(0,&text,&size)==MG_OK && !size);free(text);
    REQUIRE(clipboard_clear()==MG_OK);
    REQUIRE(clipboard_query(&(mg_clipboard_info_t){0})==MG_ERR_NOT_FOUND);
    puts("cliptest: PASS publisher-exit, 8-MiB UTF-8, pinned replacement, cancellation, empty and absent");
    return 0;
}
