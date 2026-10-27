#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production ring scrolling, overlay rendering/clearing and input positioning.
Font/framebuffer operations target an in-memory cell grid; no QEMU is involved.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'kernel/src/terminal.c').read_text()
# Check the actual syscall admission range against every public operation.
# Rendering fixtures alone cannot catch a new operation rejected by dispatch.
header=(ROOT/'libc/include/mg/terminal.h').read_text()
operations={name:int(value) for name,value in re.findall(r'(MG_TERMINAL_OP_\w+)\s*=\s*(\d+)',header)}
dispatch=(ROOT/'kernel/src/syscall.c').read_text()
upper=re.search(r'operation > (MG_TERMINAL_OP_\w+)',dispatch).group(1)
assert max(operations.values()) <= operations[upper], 'terminal syscall rejects a public operation'
reset_case=re.search(r'case MG_TERMINAL_OP_SESSION_HISTORY_RESET:(.*?break;)',dispatch,re.S)
assert reset_case, 'session history reset has no terminal syscall case'
body=reset_case.group(1)
assert re.search(r'frame->rsi\s*\|\|\s*frame->rdx',body), 'history reset accepts unexpected arguments'
assert 'syscall_is_logind(process)' in body, 'history reset is not restricted to logind'
assert 'MG_ERR_PRIVILEGE_REQUIRED' in body, 'unauthorized history reset lacks a privilege error'
assert 'terminal_reset_session_history_authorized()' in body, 'authorized reset does not reach the ring reset'
def function(name):
    match=next(m for m in re.finditer(r'^(?:static )?[A-Za-z_][^\n;{}]*\b'+name+r'\s*\(',source,re.M) if ';' not in source[m.end():source.index('{',m.end())])
    brace=source.index('{',match.end());end,depth=brace+1,1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[match.start():end]+'\n'
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef uint64_t u64;typedef uint32_t u32;typedef unsigned char u8;
#define TERMINAL_MAX_OVERLAY_ROWS 128U
#define TERMINAL_MAX_COLS 256U
#define TERMINAL_BUFFER_ROWS 256U
#define TERMINAL_MARGIN_X 0U
#define TERMINAL_MARGIN_Y 0U
typedef struct {u32 codepoint;u8 foreground,background;unsigned short reserved;} terminal_overlay_cell_t;
typedef struct {u32 codepoint;u8 fg_color,bg_color;} terminal_cell_t;
static struct {u32 visible_rows,visible_cols,cursor_phys_row,cursor_col,top_row_idx,live_top_row_idx,history_rows,fg_color,bg_color,pending_scroll_rows;bool history_view_cleared,viewport_redraw_pending,batch_active;} terminal;
static struct {bool valid;u64 owner_pid;u32 physical_row,extent;} terminal_input;
static struct {bool active;u64 owner_pid;u32 first_screen_row,rows,columns;terminal_cell_t cells[128*256];} terminal_overlay;
static struct {u64 glyph_render_count,ram_bytes_shifted;} stats;
static terminal_cell_t backing[256*80];static u32 framebuffer[25*80];static int terminal_lock;static unsigned scroll_count;
static u64 spin_lock_irqsave(int*l){assert(!*l);*l=1;return 0;}
static void spin_unlock_irqrestore(int*l,u64 f){(void)f;assert(*l);*l=0;}
static bool terminal_process_controls_locked(u64 pid){return pid==1;}
static bool terminal_color_valid(u8 c){return c<16;}
static u32 phys_to_screen_row(u32 r){return(r+256-terminal.top_row_idx)%256;}
static u32 screen_to_phys_row(u32 r){return(r+terminal.top_row_idx)%256;}
static u32 phys_to_live_screen_row(u32 r){return(r+256-terminal.live_top_row_idx)%256;}
static bool terminal_view_is_live_locked(void){return terminal.top_row_idx==terminal.live_top_row_idx;}
static u32 terminal_history_capacity_locked(void){return terminal.visible_rows<256?256-terminal.visible_rows:0;}
static terminal_cell_t*terminal_cell_at(u32 r,u32 c){return &backing[r*80+c];}
static void terminal_clear_phys_row(u32 r,u32 bg){for(u32 c=0;c<80;c++)backing[r*80+c]=(terminal_cell_t){' ',0,(u8)bg};}
static u32 terminal_line_height(void){return 1;}
static u32 font_width(void){return 1;}
static u32 framebuffer_pitch(void){return 80;}
static u32 terminal_color_pixel(u8 c){return c;}
static void draw_codepoint(u32 cp,u32 x,u32 y,u32 fg,u32 bg){(void)fg;(void)bg;assert(x<80&&y<25);framebuffer[y*80+x]=cp;}
static void framebuffer_copy_rows(u32 dest,u32 from,u32 count){memmove(framebuffer+dest*80,framebuffer+from*80,count*80*sizeof(u32));}
static void framebuffer_fill_rows(u32 first,u32 count,u32 color){(void)color;for(u32 i=first*80;i<(first+count)*80;i++)framebuffer[i]=' ';}
static void terminal_mark_dirty(u32 row){(void)row;}
static void terminal_mark_all_dirty(void){scroll_count++;}
static void terminal_cursor_hide_locked(void){}
static void terminal_cursor_show_locked(void){}
static void terminal_flush_dirty_locked(void){}
static void terminal_render_screen_row(u32 row);
static void terminal_redraw_locked(void){for(u32 row=0;row<terminal.visible_rows;row++)terminal_render_screen_row(row);}
'''
for name in ['terminal_render_screen_row','terminal_render_overlay_row_locked','terminal_overlay_render_locked',
             'terminal_overlay_clear_locked','terminal_scroll','terminal_overlay_set_for_process',
             'terminal_input_cursor_for_process']:
    pre+=function(name)
pre+=r'''
static void reset(void){terminal.visible_rows=25;terminal.visible_cols=80;terminal.top_row_idx=250;terminal.live_top_row_idx=250;terminal.history_rows=0;terminal.history_view_cleared=false;terminal.viewport_redraw_pending=false;terminal.batch_active=false;terminal.pending_scroll_rows=0;scroll_count=0;terminal_input.valid=false;terminal_overlay.active=false;for(u32 r=0;r<256;r++)terminal_clear_phys_row(r,0);for(u32 r=0;r<25;r++)terminal_render_screen_row(r);}
int main(void){
 terminal_overlay_cell_t cells[128*80];for(unsigned i=0;i<128*80;i++)cells[i]=(terminal_overlay_cell_t){'X',7,0,0};
 for(u32 cursor=0;cursor<25;cursor++)for(u32 rows=1;rows<=128;rows++){
  reset();terminal.cursor_phys_row=screen_to_phys_row(cursor);u32 original=terminal.cursor_phys_row;
  assert(terminal_overlay_set_for_process(1,rows,80,cells));assert(terminal.cursor_phys_row==original);
  u32 shown=rows<25?rows:24,available=24-cursor;
  assert(scroll_count==(shown>available?shown-available:0));assert(terminal_overlay.rows==shown);
  unsigned before=scroll_count;assert(terminal_overlay_set_for_process(1,rows,80,cells));assert(scroll_count==before);
 }
 reset();terminal_input.valid=true;terminal_input.owner_pid=1;terminal_input.physical_row=screen_to_phys_row(23);terminal_input.extent=120;
 for(u32 i=0;i<120;i++){u32 row=(terminal_input.physical_row+i/80)%256;backing[row*80+i%80].codepoint='a'+i%26;}
 for(u32 r=0;r<25;r++)terminal_render_screen_row(r);
 assert(terminal_input_cursor_for_process(1,10,120));u32 cursor_phys=terminal.cursor_phys_row;
 assert(terminal_overlay_set_for_process(1,3,80,cells));assert(scroll_count==3&&phys_to_screen_row(terminal_input.physical_row)==20);
 assert(terminal.cursor_phys_row==cursor_phys&&terminal.cursor_col==10);assert(terminal_overlay.first_screen_row==22);
 for(u32 i=0;i<120;i++)assert(framebuffer[20*80+i]=='a'+i%26);
 for(u32 i=22*80;i<25*80;i++)assert(framebuffer[i]=='X');
 terminal_overlay_clear_locked(true);for(u32 i=22*80;i<25*80;i++)assert(framebuffer[i]==' ');
 assert(terminal_input_cursor_for_process(1,100,120));assert(phys_to_screen_row(terminal.cursor_phys_row)==21&&terminal.cursor_col==20);
 assert(terminal_input_cursor_for_process(1,80,120));assert(phys_to_screen_row(terminal.cursor_phys_row)==20&&terminal.cursor_col==80);
 assert(terminal_input_cursor_for_process(1,0,120));assert(terminal.cursor_phys_row==terminal_input.physical_row&&terminal.cursor_col==0);
 assert(!terminal_input_cursor_for_process(2,0,120));assert(!terminal_input_cursor_for_process(1,121,120));
 assert(!terminal_overlay_set_for_process(2,3,80,cells));assert(!terminal_overlay_set_for_process(1,3,81,cells));
 terminal_input.extent=25*80;assert(!terminal_overlay_set_for_process(1,3,80,cells));
 reset();terminal.visible_rows=1;assert(!terminal_overlay_set_for_process(1,3,80,cells));
 puts("terminal production scrolling/rendering: placement, ring wrap, wrapped input, cursor positions and overlay restoration passed (in-memory framebuffer)");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-overlay-') as directory:
    path=Path(directory)/'test.c';binary=path.with_suffix('');path.write_text(pre)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
