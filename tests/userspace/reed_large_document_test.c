/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <time.h>
#include "../../userspace/reed/buffer.h"
static usize decoded;
static bool measured_codepoint(const reed_buffer_t *b,usize p,u32 *cp,usize *next)
{ decoded++;return reed_buffer_codepoint(b,p,cp,next); }
#define reed_buffer_codepoint measured_codepoint
#define main interaction_fixture_main
#include "reed_interaction_test.c"
#undef main
#undef reed_buffer_codepoint
static double milliseconds(void)
{
    struct timespec now;assert(timespec_get(&now,TIME_UTC)==TIME_UTC);
    return (double)now.tv_sec*1000.0+(double)now.tv_nsec/1000000.0;
}
int main(int argc,char **argv)
{
    bool baseline=argc>1 && !strcmp(argv[1],"baseline");
    usize bytes=baseline?2U*1024U*1024U-128U:16U*1024U*1024U;
    char *text=malloc(bytes+1);assert(text);
    for(usize i=0;i<bytes;i++)text[i]=(i%80==79)?'\n':'a'+i%26;
    text[bytes]=0;reed_editor_t editor;
    double begin=milliseconds();editor_with_text(&editor,text);
    assert(reed_buffer_set_final_newline(&editor.buffer,false));
    assert(reed_buffer_mark_saved(&editor.buffer));
    set_cursor(&editor,bytes/2);
    double loaded=milliseconds();decoded=0;
    for(unsigned i=0;i<100;i++)move_vertical(&editor,(i%2)==0);
    double navigated=milliseconds();usize navigation_decodes=decoded;
    editor.search_active=true;editor.search_length=4;memcpy(editor.search_text,text+bytes/2,4);
    usize match=0;double search_begin=milliseconds();
    assert(search_find_closest(&editor,bytes/2,&match) && match==bytes/2);
    double searched=milliseconds();editor.search_active=false;
    printf("Reed nearest search bytes=%zu ms=%.3f\n",bytes,searched-search_begin);
    assert(reed_buffer_record_edit(&editor.buffer));
    assert(reed_buffer_insert(&editor.buffer,bytes/2,"X",1));
    assert(reed_buffer_is_modified(&editor.buffer));
    set_cursor(&editor,bytes/2+1); /* Include edit-triggered layout work. */
    usize retained=editor.buffer.undo.bytes;
    assert(reed_buffer_undo(&editor.buffer) && !reed_buffer_is_modified(&editor.buffer));
    assert(reed_buffer_redo(&editor.buffer) && reed_buffer_is_modified(&editor.buffer));
    assert(reed_buffer_undo(&editor.buffer));
    double edited=milliseconds();
    if(!baseline){assert(retained<256);assert(navigation_decodes<100000);}
    printf("Reed bytes=%zu load_layout_ms=%.3f navigation100_ms=%.3f decodes=%zu edit_undo_redo_ms=%.3f undo_bytes=%zu\n",bytes,loaded-begin,navigated-loaded,navigation_decodes,edited-navigated,retained);
    /* One large paste/cut remains atomic and undoable beyond the retention
     * target; failure at every prepare allocation preserves text/history. */
    if(!baseline){
        usize prior=reed_buffer_length(&editor.buffer),undo=editor.buffer.undo.count;
        for(int at=0;at<3;at++){
            reed_buffer_edit_t edit={0};
            fail_malloc_after=at;
            bool prepared=reed_buffer_prepare_edit(&editor.buffer,0,prior,text,bytes,&edit);
            fail_malloc_after=-1;
            if(prepared)reed_buffer_discard_edit(&edit);
            assert(reed_buffer_length(&editor.buffer)==prior && editor.buffer.undo.count==undo);
            assert(!reed_buffer_is_modified(&editor.buffer));
        }
        reed_buffer_edit_t edit={0};
        assert(reed_buffer_prepare_edit(&editor.buffer,0,prior,text,bytes,&edit));
        reed_buffer_commit_edit(&edit);
        assert(reed_buffer_undo(&editor.buffer));
        assert(!reed_buffer_is_modified(&editor.buffer));
        assert(reed_buffer_redo(&editor.buffer));
    }
    editor_destroy_frames(&editor);reed_buffer_destroy(&editor.buffer);
    /* A single long logical line uses sparse cell checkpoints on each move. */
    memset(text,'a',bytes);editor_with_text(&editor,text);
    set_cursor(&editor,bytes/2);decoded=0;begin=milliseconds();
    for(unsigned i=0;i<100;i++)move_vertical(&editor,(i%2)==0);
    double ended=milliseconds();
    printf("Reed long-line bytes=%zu navigation100_ms=%.3f decodes=%zu\n",bytes,ended-begin,decoded);
    if(!baseline)assert(decoded<100000);
    editor_destroy_frames(&editor);reed_buffer_destroy(&editor.buffer);free(text);
    puts(baseline?"Reed comparison workload completed":"Reed large document: indexed navigation, delta history, atomic range edits passed");
    return 0;
}
