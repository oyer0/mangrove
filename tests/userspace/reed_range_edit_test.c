/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static int fail_after=-1;
static void *allocate(size_t size) {
    if(fail_after==0){fail_after=-1;return NULL;}
    if(fail_after>0)fail_after--;
    return malloc(size);
}
static void *resize(void *p,size_t size) {
    if(fail_after==0){fail_after=-1;return NULL;}
    if(fail_after>0)fail_after--;
    return realloc(p,size);
}
#define malloc allocate
#define realloc resize
#include "../../userspace/reed/buffer.c"
#undef malloc
#undef realloc
int main(void)
{
    char text[4096];memset(text,'x',sizeof(text));
    for(int stage=0;stage<3;stage++) {
        reed_buffer_t b;reed_buffer_edit_t edit={0};
        assert(reed_buffer_init(&b));
        assert(reed_buffer_replace(&b,"original",8));
        assert(reed_buffer_mark_saved(&b));
        fail_after=stage;
        assert(!reed_buffer_prepare_edit(&b,0,8,text,sizeof(text),&edit));
        assert(fail_after==-1 && !edit.buffer && !b.undo.count && !b.redo.count);
        char actual[32];assert(reed_buffer_copy(&b,actual,sizeof(actual),NULL));
        assert(!strcmp(actual,"original") && !reed_buffer_is_modified(&b));
        assert(reed_buffer_prepare_edit(&b,0,8,text,sizeof(text),&edit));
        reed_buffer_commit_edit(&edit);
        assert(reed_buffer_length(&b)==sizeof(text) && b.undo.count==1);
        assert(reed_buffer_undo(&b));
        assert(reed_buffer_copy(&b,actual,sizeof(actual),NULL) && !strcmp(actual,"original"));
        assert(!reed_buffer_is_modified(&b));
        assert(reed_buffer_redo(&b) && reed_buffer_length(&b)==sizeof(text));
        reed_buffer_destroy(&b);
    }
    /* Adding a newline needs a separate logical-line index reservation.
     * Exercise every prepare allocation, then the undo destination reserve. */
    text[0]='\n';
    for(int stage=0;stage<4;stage++) {
        reed_buffer_t b;reed_buffer_edit_t edit={0};
        assert(reed_buffer_init(&b));
        assert(reed_buffer_replace(&b,"original",8));
        assert(reed_buffer_mark_saved(&b));
        fail_after=stage;
        assert(!reed_buffer_prepare_edit(&b,0,8,text,sizeof(text),&edit));
        assert(fail_after==-1 && !b.undo.count && reed_buffer_line_count(&b)==1);
        char actual[32];assert(reed_buffer_copy(&b,actual,sizeof(actual),NULL));
        assert(!strcmp(actual,"original") && !reed_buffer_is_modified(&b));
        assert(reed_buffer_prepare_edit(&b,0,8,text,sizeof(text),&edit));
        reed_buffer_commit_edit(&edit);
        assert(reed_buffer_line_count(&b)==2 && b.undo.count==1);
        fail_after=0;
        assert(!reed_buffer_undo(&b));
        assert(fail_after==-1 && b.undo.count==1 && !b.redo.count &&
               reed_buffer_length(&b)==sizeof(text) && reed_buffer_line_count(&b)==2);
        assert(reed_buffer_undo(&b) && reed_buffer_line_count(&b)==1);
        assert(!reed_buffer_is_modified(&b));
        assert(reed_buffer_redo(&b) && reed_buffer_line_count(&b)==2);
        reed_buffer_destroy(&b);
    }
    puts("Reed range edit: gap, line-index, undo-array, delta and replay allocation failures preserve text/history; commit and undo/redo passed");
}
