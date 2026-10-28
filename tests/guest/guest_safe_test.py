#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real native safe-file and unmodified Reed qualification on disposable USB media."""
import argparse
import contextlib
import copy
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import resource
import secrets
import socket
import shutil
import sys
import time
sys.dont_write_bytecode=True
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1] / "shared"))
_TEST_MEMORY = _TestPath(__file__).resolve().parents[1] / "memory"
_test_sys.path.insert(0, str(_TEST_MEMORY))
import artifact_lifecycle as artifacts
import guest_mgfs_test as guest
import guest_secondary_test as secondary
import heap_profile_analyze

SIZES=(0,1,511,512,4096,8193,2097152)
INTERACTIVE_LONG_TEXT='L'*96

def fat_listing_names(text):
    return [line.rstrip('/').rsplit('/',1)[-1] for line in text.splitlines() if line.startswith('::')]

def work_names(backend,run,reed_from_shoot=False):
    # Mangrove's short-only FAT aliases have canonical uppercase spelling;
    # the separately imported seed has lowercase flags. Do not apply generic
    # Unicode casefolding or derive this expectation from observed names.
    application=['REED.TXT','COPIED.TXT','OCCUPIED.TXT'] if backend=='fat32' else ['reed.txt','copied.txt','occupied.txt']
    if reed_from_shoot:application.extend(('hello world!','hello quoted!'))
    return ['seed.txt',f'safe-{run}',*application]

def pattern(run,length,version=4):
    return bytes((i*29+(i>>8)*7+ord(run[i%16])+version*53)&255 for i in range(length))

def key(qmp,character):
    if character in ('U','shift-u'):
        code='shift-u'
    else:
        code={'/':'slash','.':'dot','>':'shift-dot',' ':'spc','-':'minus',
              '_':'shift-minus',
              '\x04':'ctrl-d',
              '\n':'ret','\t':'tab','\b':'backspace','\x1b':'esc',
              }.get(character,character)
        if code==character and character.isupper():code='shift-'+character.lower()
    # Match the short QEMU key pulse already used by the shared guest runner.
    # The controller advances only after a rendered screen proves the key took
    # effect; a longer hold can trigger the guest's typematic repeat.
    result=qmp.call('human-monitor-command',
                    {'command-line':f'sendkey {code} 30'})
    if result:raise ValueError('USB keyboard press rejected: '+str(result))

def reed_cursor_column(path,expected):
    """Find Reed's inverted cursor cell in one rendered ASCII text row."""
    header,dimensions,maximum,pixels=path.read_bytes().split(b'\n',3)
    if header!=b'P6' or maximum!=b'255':raise ValueError('unsupported screendump')
    width,height=map(int,dimensions.split())
    if width*height>4096*2160 or len(pixels)!=width*height*3:
        raise ValueError('screendump bounds')
    matching=[index for index,row in enumerate(guest.screen_text(path).splitlines())
              if row==expected]
    if len(matching)!=1:return None
    row=matching[0]
    backgrounds=[]
    for column in range(len(expected)):
        colors=[]
        for dy in range(16):
            start=((10+row*18+dy)*width+10+column*8)*3
            for dx in range(8):
                offset=start+dx*3
                colors.append(pixels[offset:offset+3])
        backgrounds.append(Counter(colors).most_common(1)[0][0])
    common=Counter(backgrounds).most_common(1)[0][0]
    cursor=[column for column,color in enumerate(backgrounds)
            if color!=common]
    return cursor[0] if len(cursor)==1 else None

class ReedControl:
    """One keyboard event per observed editor state, never elapsed-time success."""
    def __init__(self,run,phase,receiver=None):
        self.run,self.phase=run,phase;self.actions=[];self.stage=0;self.typed=0;self.shoot_stage=0;self.receiver=receiver;self.external_seen=False;self.channel_messages=0;self.reed_no_conflict=False;self.reed_from_shoot=False;self.reed_guest_conflict_sync=False;self.keyboard_repeat=False;self.keyboard_repeat_evidence=None;self.reed_path='';self.shell_stage=0;self.shell_typed=0;self.shell_prompt='developer ~:';self.shell_progress=time.monotonic();self.shell_active=False;self.filename_probe_stage=0;self.redo_status='not-attempted';self.redo_requested_at=0.0;self.redo_evidence_file=None;self.redo_input_diagnostic=None;self.redo_witness_screens={}
    def configure(self,invocation,part): pass
    def record(self,qmp,part,text,description,code):
        (part/f'reed-{len(self.actions):03}.txt').write_text(text)
        if description in ('edit-before-undo','undo-restores-base',
                           'redo-input-received','redo-restores-edit',
                           'redo-state-unchanged'):
            self.redo_witness_screens[description]=text
        if description in ('load','edit-before-undo','undo-restores-base','redo-input-received','redo-restores-edit','redo-state-unchanged','stale-conflict','cancel-preserved','saved-as','readonly-denied','reopen-loaded','terminal-restored','list-space-filename','space-filename-escaped-opened','space-filename-quoted-opened'):
            screenshot=f'reed-{len(self.actions):03}.ppm'
            shutil.copyfile(part/'screen.ppm',part/screenshot)
            if description=='redo-state-unchanged':self.redo_evidence_file=screenshot
        self.actions.append({'action':description,'seconds':time.monotonic(),'key':code})
        if code is not None:key(qmp,code)
        self.shell_progress=time.monotonic()
        (part/'reed-actions.json').write_text(json.dumps(self.actions,indent=2))
    def probe_keyboard_repeat(self,qmp,part,text):
        expected='editedbase'
        if '1:11' not in text:
            raise ValueError('repeat probe did not start at the end of editedbase')
        down=[{'type':'key','data':{'down':True,
              'key':{'type':'qcode','data':'h'}}}]
        up=[{'type':'key','data':{'down':False,
            'key':{'type':'qcode','data':'h'}}}]
        result=qmp.call('input-send-event',{'events':down})
        if result:raise ValueError('repeat probe key-down rejected: '+str(result))
        down_sent=time.monotonic()
        time.sleep(.55)
        keyup_started=time.monotonic()
        result=qmp.call('input-send-event',{'events':up})
        if result:raise ValueError('repeat probe key-up rejected: '+str(result))
        keyup_ack=time.monotonic()
        released_columns=[]
        for index in range(2):
            time.sleep(.10 if index == 0 else .15)
            path=part/f'reed-repeat-released-{index+1}.ppm'
            qmp.screendump(path)
            screen=guest.screen_text(path)
            if expected not in screen:
                raise ValueError('Reed text changed during navigation repeat probe')
            column=reed_cursor_column(path,expected)
            if column is None:
                # The terminal cursor may be in its blink-off interval. Retry
                # a bounded number of times without sending additional input.
                for _ in range(8):
                    time.sleep(.08)
                    qmp.screendump(path)
                    column=reed_cursor_column(path,expected)
                    if column is not None:break
            released_columns.append(column)
        self.keyboard_repeat_evidence={
            'key':'H','interface':'QMP USB HID input-send-event',
            'start_cursor_column':10,'requested_hold_ms':550,
            'key_up_delay_ms':round((keyup_started-down_sent)*1000,1),
            'key_up_call_ms':round((keyup_ack-keyup_started)*1000,1),
            'cursor_columns_after_release':released_columns,
        }
        self.actions.append({'action':'keyboard-repeat-probe',
                             'seconds':time.monotonic(),
                             'evidence':self.keyboard_repeat_evidence})
        (part/'reed-actions.json').write_text(json.dumps(self.actions,indent=2))
        if (released_columns[0] is None or
                released_columns[0] < 1 or released_columns[0] > 7 or
                any(column is None for column in released_columns) or
                released_columns[0] != released_columns[1]):
            raise ValueError('native Reed repeat did not move during hold and settle after key-up: '
                             +repr(self.keyboard_repeat_evidence))
    def command(self,qmp,text,part,command):
        # Command mode has a sole slash-prefixed status row. Require the exact
        # prefix before advancing, including the entire command before Enter.
        lines=[line.rstrip() for line in text.splitlines()]
        prefix='/'+command[:self.typed]
        if prefix.rstrip() not in lines:return False
        c=command[self.typed] if self.typed<len(command) else '\n'
        self.record(qmp,part,text,'command-prefix:'+prefix,c)
        self.typed+=1
        if c=='\n':self.typed=0;return True
        return False
    @staticmethod
    def editor_title(text,mode,name):
        return bool(re.search(r'^'+re.escape(mode)+r'\s+.*(?:/|\s)'+re.escape(name)+r'(?:\s|$)',text,re.M))
    @staticmethod
    def shell_input(text,prompt):
        at=text.rfind(prompt)
        if at<0:return None
        return ''.join(line.rstrip() for line in text[at:].splitlines())
    def shell_type(self,qmp,part,text,prefix,suffix,description):
        prompt=self.shell_prompt
        expected=(prompt+' '+prefix+suffix[:self.shell_typed]).rstrip()
        if self.shell_input(text,prompt)!=expected:return False
        if self.shell_typed==len(suffix):return True
        value=suffix[self.shell_typed]
        self.record(qmp,part,text,description+':'+str(self.shell_typed),value)
        self.shell_typed+=1
        return False
    def shell_key(self,qmp,part,text,buffer,description,code):
        prompt=self.shell_prompt
        if self.shell_input(text,prompt)!=(prompt+' '+buffer).rstrip():return False
        self.record(qmp,part,text,description,code)
        return True
    def shell_submit(self,qmp,part,text,buffer,description):
        if not self.shell_key(qmp,part,text,buffer,description,'\n'):return False
        self.shell_typed=0
        return True
    def observe_interactive_shell(self,qmp,text,part):
        label=getattr(self,'volume_label','QUAL_MGFS')
        work=f'/vol/{label}/work'
        home='developer ~:'
        # The root test script itself is launched from this prompt and may
        # spend time preparing files before its nested interactive Shoot
        # child starts. Do not mistake that original command line for the
        # child's prompt or apply the interaction watchdog before readiness.
        if not self.shell_active:
            if self.shell_input(text,home)!=home:return
            self.shell_active=True
            self.shell_progress=time.monotonic()
            return
        if self.shell_stage==0:
            if self.shell_type(qmp,part,text,'','/bin/say shot','shell-edited-command'):
                self.shell_stage=1
        elif self.shell_stage==1:
            if self.shell_key(qmp,part,text,'/bin/say shot','shell-cursor-left','left'):
                self.shell_stage=2
        elif self.shell_stage==2:
            if self.shell_key(qmp,part,text,'/bin/say shot','shell-insert-at-cursor','o'):
                self.shell_stage=3;self.shell_typed=0
        elif self.shell_stage==3:
            if self.shell_key(qmp,part,text,'/bin/say shoot','shell-cursor-end','end'):
                self.shell_stage=4;self.shell_typed=0
        elif self.shell_stage==4:
            suffix=' > /temp/shoot.txt'
            if self.shell_type(qmp,part,text,'/bin/say shoot',suffix,'shell-redirection'):
                if self.shell_submit(qmp,part,text,'/bin/say shoot'+suffix,'shell-run-redirected-command'):
                    self.shell_stage=5
        elif self.shell_stage==5:
            if self.shell_input(text,home)==home and 'Could not run' not in text:
                self.shell_stage=6;self.shell_typed=0
        elif self.shell_stage==6:
            command='/bin/say '+INTERACTIVE_LONG_TEXT+' > /temp/interactive-long.txt'
            if self.shell_type(qmp,part,text,'',command,'shell-long-wrapped-command'):
                if self.shell_submit(qmp,part,text,command,'shell-run-long-command'):
                    self.shell_stage=7
        elif self.shell_stage==7:
            if self.shell_input(text,home)==home and 'Could not run' not in text:
                self.shell_stage=8;self.shell_typed=0
        elif self.shell_stage==8:
            if self.shell_type(qmp,part,text,'','history -a','shell-history-persist'):
                if self.shell_submit(qmp,part,text,'history -a','shell-history-persist-submit'):
                    self.shell_stage=9
        elif self.shell_stage==9:
            if self.shell_input(text,home)==home:
                self.shell_stage=10;self.shell_typed=0
        elif self.shell_stage==10:
            if self.shell_type(qmp,part,text,'','ty','shell-history-draft'):
                if self.shell_key(qmp,part,text,'ty','shell-history-previous','up'):
                    self.shell_stage=11
        elif self.shell_stage==11:
            if self.shell_key(qmp,part,text,'history -a','shell-history-next','down'):
                self.shell_stage=12
        elif self.shell_stage==12:
            if self.shell_key(qmp,part,text,'ty','shell-completion-tab','\t'):
                self.shell_stage=13;self.shell_typed=0
        elif self.shell_stage==13:
            suffix=' /temp/shoot.txt'
            if self.shell_type(qmp,part,text,'type',suffix,'shell-completed-command'):
                if self.shell_submit(qmp,part,text,'type'+suffix,'shell-run-completed-command'):
                    self.shell_stage=14
        elif self.shell_stage==14:
            if (self.shell_input(text,home)==home and
                    any(line.strip()=='shoot' for line in text.splitlines()) and
                    'Could not run' not in text):
                self.shell_stage=15;self.shell_typed=0
        elif self.shell_stage==15:
            prefix=f'jump /vol/{label}/wor'
            if self.shell_type(qmp,part,text,'',prefix,'shell-directory-completion-prefix'):
                if self.shell_key(qmp,part,text,prefix,'shell-directory-completion','\t'):
                    self.shell_stage=16
        elif self.shell_stage==16:
            completed=f'jump {work}/'
            if self.shell_submit(qmp,part,text,completed,'shell-enter-completed-directory'):
                self.shell_prompt=f'developer {work}:';self.shell_stage=17
        elif self.shell_stage==17:
            if self.shell_input(text,self.shell_prompt)==self.shell_prompt:
                self.shell_stage=27;self.shell_typed=0
        elif self.shell_stage==27:
            if self.shell_type(qmp,part,text,'','list','shell-list-spaced-filename'):
                if self.shell_submit(qmp,part,text,'list','shell-list-spaced-filename-submit'):
                    self.shell_stage=28
        elif self.shell_stage==28:
            if (self.shell_input(text,self.shell_prompt)==self.shell_prompt and
                    "'hello world!'" in text):
                self.record(qmp,part,text,'list-space-filename',None)
                self.shell_stage=18;self.shell_typed=0
        elif self.shell_stage==18:
            if self.shell_type(qmp,part,text,'','jump /home/developer','shell-return-home'):
                if self.shell_submit(qmp,part,text,'jump /home/developer','shell-return-home-submit'):
                    self.shell_prompt=home;self.shell_stage=19
        elif self.shell_stage==19:
            command=f'/bin/shoot {self.reed_path}'
            if self.shell_type(qmp,part,text,'',command,'shell-launch-reed-script'):
                if self.shell_submit(qmp,part,text,command,'shell-launch-reed-script-submit'):
                    self.shoot_stage=3;self.shell_stage=20
        if self.shell_stage>=20:return
    def observe(self,qmp,text,part,started):
        if (self.reed_from_shoot and self.shell_active and
                time.monotonic()-self.shell_progress>25):
            raise TimeoutError(
                'Reed guest workflow stopped making rendered progress at '
                f'shell stage {self.shell_stage}, editor stage {self.stage}, '
                f'filename probe stage {self.filename_probe_stage}')
        if self.receiver is not None:
            try:message,peer=self.receiver.recvfrom(129)
            except BlockingIOError:pass
            else:
                if peer[0]!='127.0.0.1':raise ValueError('nonlocal notification')
                if message==f'REED1 channel run={self.run}\n'.encode():
                    self.channel_messages+=1
                    if self.phase!='create' or self.channel_messages>64:raise ValueError('invalid channel handshake')
                    self.receiver.sendto(self.run.encode(),peer)
                else:
                    if self.phase!='create' or self.stage!=3 or self.external_seen or message!=f'REED1 external run={self.run}\n'.encode():raise ValueError('invalid/duplicate external-change notification')
                    self.external_seen=True
                    (part/'external-notification.json').write_text(json.dumps({'peer':peer,'message':message.decode(),'seconds':time.monotonic()}))
        if self.phase=='readonly':
            if self.stage==0 and 'NORMAL  readonly-new.txt' in text:
                self.record(qmp,part,text,'readonly-new','i');self.stage=1
            elif self.stage==1 and 'INSERT  readonly-new.txt' in text:
                self.record(qmp,part,text,'readonly-edit','x');self.stage=2
            elif self.stage==2 and re.search(r'^x\s*$',text,re.M):
                self.record(qmp,part,text,'readonly-edited','\x1b');self.stage=3
            elif self.stage==3 and 'NORMAL  readonly-new.txt [+]' in text:
                self.record(qmp,part,text,'readonly-saveas','/');self.stage=4
            elif self.stage==4 and self.command(qmp,text,part,'saveas forbidden'):self.stage=5
            elif self.stage==5 and 'Save denied: access changed; edits kept.' in text:
                self.record(qmp,part,text,'readonly-denied','/');self.stage=6
            elif self.stage==6 and self.command(qmp,text,part,'q'):self.stage=7
            elif self.stage==7 and "Save changes? [s] Save [d] Don't Save [c] Cancel:" in text:
                self.record(qmp,part,text,'readonly-discard','d');self.stage=8
            elif self.stage==8 and not re.search(r'^(?:NORMAL|INSERT|COMMAND|SEARCH)\s+',text,re.M):
                self.record(qmp,part,text,'terminal-restored',None);self.stage=9
            return
        if self.phase=='verify':
            if self.stage==0 and 'NORMAL  copied.txt' in text and 'editedbase' in text:
                self.record(qmp,part,text,'reopen-loaded','/');self.stage=1
            elif self.stage==1 and self.command(qmp,text,part,'q'):self.stage=2
            elif self.stage==2 and not re.search(r'^(?:NORMAL|INSERT|COMMAND|SEARCH)\s+',text,re.M):
                self.record(qmp,part,text,'terminal-restored',None);self.stage=3
            return
        if (self.reed_from_shoot and
                (self.shell_stage < 20 or self.shell_stage in (27, 28))):
            self.observe_interactive_shell(qmp,text,part)
            return
        if self.reed_from_shoot and self.filename_probe_stage < 6:
            if (self.filename_probe_stage==0 and
                    self.editor_title(text,'NORMAL','hello world!') and
                    re.search(r'^argv-probe\s*$',text,re.M)):
                self.record(qmp,part,text,'space-filename-escaped-opened','/')
                self.filename_probe_stage=1
            elif (self.filename_probe_stage==1 and
                  self.command(qmp,text,part,'q')):
                self.filename_probe_stage=2
            elif (self.filename_probe_stage==2 and
                  self.editor_title(text,'NORMAL','hello quoted!') and
                  re.search(r'^quoted-probe\s*$',text,re.M)):
                self.record(qmp,part,text,'filename-escaped-returned',None)
                self.filename_probe_stage=3
            elif (self.filename_probe_stage==3 and
                    self.editor_title(text,'NORMAL','hello quoted!') and
                    re.search(r'^quoted-probe\s*$',text,re.M)):
                self.record(qmp,part,text,'space-filename-quoted-opened','/')
                self.filename_probe_stage=4
            elif (self.filename_probe_stage==4 and
                  self.command(qmp,text,part,'q')):
                self.filename_probe_stage=5
            elif (self.filename_probe_stage==5 and
                  self.editor_title(text,'NORMAL','reed.txt') and
                  re.search(r'^base\s*$',text,re.M)):
                self.record(qmp,part,text,'filename-quoted-returned',None)
                self.filename_probe_stage=6
            return
        if self.reed_from_shoot and self.shoot_stage==3 and self.stage==8 and self.shell_stage==20:
            command=f'type /vol/{getattr(self,"volume_label","QUAL_MGFS")}/work/reed.txt'
            if self.shell_type(qmp,part,text,'',command,'shell-read-saved-reed-file'):
                if self.shell_submit(qmp,part,text,command,'shell-read-saved-reed-file-submit'):
                    self.shell_stage=21
            return
        if self.reed_from_shoot and self.shoot_stage==3 and self.stage==8 and self.shell_stage==21:
            if (self.shell_input(text,'developer ~:')=='developer ~:' and
                    any(line.strip()=='editedbase' for line in text.splitlines()) and
                    'Could not run' not in text):
                self.shell_stage=22;self.typed=0
            return
        if self.reed_from_shoot and self.shoot_stage==3 and self.stage==8 and self.shell_stage==22:
            command='exit';lines=text.rstrip().splitlines()
            prefix='developer ~: '+command[:self.typed]
            if lines and lines[-1]==prefix.rstrip():
                c=command[self.typed] if self.typed<len(command) else '\n'
                self.record(qmp,part,text,'shoot-exit-prefix:'+prefix,c);self.typed+=1
                if c=='\n':self.typed=0;self.shoot_stage=4;self.stage=9;self.shell_stage=23
            return
        if self.shoot_stage<3:
            commands=['/bin/say shoot > /temp/shoot.txt','history -a',
                      f'/bin/shoot {self.reed_path}' if self.reed_from_shoot else 'exit']
            lines=text.rstrip().splitlines();prefix='developer ~: '+commands[self.shoot_stage][:self.typed]
            if lines and lines[-1]==prefix.rstrip():
                command=commands[self.shoot_stage]
                c=command[self.typed] if self.typed<len(command) else '\n'
                self.record(qmp,part,text,'shoot-prefix:'+prefix,c);self.typed+=1
                if c=='\n':self.typed=0;self.shoot_stage+=1
            # It is the same ordinary prompt as the initiating shell. Only
            # observe after the launch; the native parent waits for its child.
            return
        if self.stage==0 and self.editor_title(text,'NORMAL','reed.txt') and re.search(r'^base\s*$',text,re.M):
            self.record(qmp,part,text,'load','a');self.stage=0.5
        elif self.stage==0.5 and self.editor_title(text,'NORMAL','reed.txt') and '1:1' in text:
            self.record(qmp,part,text,'line-start','i');self.stage=1
        elif self.stage==1 and self.editor_title(text,'INSERT','reed.txt') and re.search(r'^'+re.escape('edited'[:self.typed]+'base')+r'\s*$',text,re.M):
            c='edited'[self.typed] if self.typed<6 else '\x1b'
            self.record(qmp,part,text,'insert:'+str(self.typed),c);self.typed+=1
            if c=='\x1b':self.typed=0;self.stage=2
        elif (self.stage==2 and self.editor_title(text,'NORMAL','reed.txt') and ' [+]' in text and
              re.search(r'^editedbase\s*$',text,re.M)):
            # Capture the edited state before issuing undo.
            if self.keyboard_repeat:
                self.record(qmp,part,text,'edit-before-repeat',None)
                self.record(qmp,part,text,'repeat-line-end','e');self.stage=2.05
            else:
                self.record(qmp,part,text,'edit-before-undo','u');self.stage=2.1
        elif (self.stage==2.05 and self.keyboard_repeat and
              self.editor_title(text,'NORMAL','reed.txt') and
              re.search(r'^editedbase\s*$',text,re.M)):
            self.probe_keyboard_repeat(qmp,part,text)
            self.record(qmp,part,text,'edit-before-undo','u');self.stage=2.1
        elif (self.stage==2.1 and self.editor_title(text,'NORMAL','reed.txt') and
              ' [+]' not in text and re.search(r'^base\s*$',text,re.M)):
            self.redo_status='attempted';self.redo_requested_at=time.monotonic()
            # The clean original text is the observable result of undo. Save
            # that witness before sending the existing Shift+U redo chord.
            self.record(qmp,part,text,'undo-restores-base','shift-u');self.stage=2.15
        elif (self.stage==2.15 and self.redo_input_diagnostic is None and
              self.editor_title(text,'NORMAL','reed.txt') and
              re.search(r'test input U modifiers=(\d+) action=(none|undo|redo) bytes=(\d+)',text)):
            diagnostic=re.search(r'test input U modifiers=(\d+) action=(none|undo|redo) bytes=(\d+)',text)
            shift_mask=(1 << 1) | (1 << 5)
            if (diagnostic and diagnostic.group(2)=='redo' and
                    int(diagnostic.group(1)) & shift_mask):
                self.redo_input_diagnostic=diagnostic.group(0)
                self.record(qmp,part,text,'redo-input-received',None)
        elif (self.stage==2.15 and self.editor_title(text,'NORMAL','reed.txt') and
              ' [+]' in text and re.search(r'^editedbase\s*$',text,re.M)):
            self.redo_status='observed'
            self.record(qmp,part,text,'redo-restores-edit',None)
            if self.reed_guest_conflict_sync:
                # Save this redone buffer directly so the native conflict test
                # proves the same edit that redo restored is retained by the
                # stale-save cancellation path.
                self.stage=2.3
            else:
                self.record(qmp,part,text,'undo-after-redo','u');self.stage=2.16
        elif (self.stage==2.15 and self.editor_title(text,'NORMAL','reed.txt') and
              ' [+]' not in text and re.search(r'^base\s*$',text,re.M) and
              time.monotonic()-self.redo_requested_at>=5.0):
            # QMP accepted Shift+U, but qualification requires observing the
            # edited buffer return. If it does not, record that gap and use
            # the established second-edit path so the rest of the guest
            # integration can still be checked independently.
            self.redo_status='not-observed'
            self.record(qmp,part,text,'redo-state-unchanged',None);self.stage=2.16
        elif (self.stage==2.16 and self.editor_title(text,'NORMAL','reed.txt') and
              ' [+]' not in text and re.search(r'^base\s*$',text,re.M)):
            description='edit-after-redo' if self.redo_status=='observed' else 'reedit-after-unobserved-redo'
            self.record(qmp,part,text,description,'i');self.stage=2.2;self.typed=0
        elif (self.stage==2.2 and self.editor_title(text,'INSERT','reed.txt') and
              re.search(r'^'+re.escape('edited'[:self.typed]+'base')+r'\s*$',text,re.M)):
            c='edited'[self.typed] if self.typed<6 else '\x1b'
            self.record(qmp,part,text,'reinsert:'+str(self.typed),c);self.typed+=1
            if c=='\x1b':self.typed=0;self.stage=2.3
        elif (self.stage==2.3 and self.editor_title(text,'NORMAL','reed.txt') and ' [+]' in text and
              re.search(r'^editedbase\s*$',text,re.M)):
            self.record(qmp,part,text,'save-opened-command','/');self.typed=0;self.stage=2.31
        elif self.stage==2.31 and self.command(qmp,text,part,'s'):
            self.stage=3
        elif self.stage==3 and self.reed_no_conflict and 'Saved opened file.' in text:
            # This focused mode exercises Reed without the host-to-guest UDP
            # rendezvous used only by stale-save conflict qualification.
            self.record(qmp,part,text,'save-complete','/')
            self.stage=4
        elif self.reed_no_conflict and self.stage==4 and self.command(qmp,text,part,'saveas copied.txt'):
            self.stage=5
        elif self.reed_no_conflict and self.stage==5 and 'NORMAL  copied.txt' in text and 'Saved new file.' in text:
            self.record(qmp,part,text,'saved-as','/')
            self.stage=6
        elif self.reed_no_conflict and self.stage==6 and self.command(qmp,text,part,'q'):
            self.stage=7
        elif (self.stage==3 and self.reed_guest_conflict_sync and
              'Changed: [c] Cancel [a] Save As [o] Overwrite:' in text):
            self.external_seen=True
            self.record(qmp,part,text,'stale-conflict','c');self.stage=5
        elif self.stage==3 and self.external_seen and 'Saved opened file.' in text:
            self.record(qmp,part,text,'independent-external-change-save','/');self.typed=0;self.stage=3.5
        elif self.stage==3.5 and self.command(qmp,text,part,'s'):
            self.stage=4
        elif self.stage==4 and 'Changed: [c] Cancel [a] Save As [o] Overwrite:' in text:
            self.record(qmp,part,text,'stale-conflict','c');self.stage=5
        elif self.stage==5 and 'Save cancelled; external change unresolved' in text:
            self.record(qmp,part,text,'cancel-preserved','/');self.stage=6
        elif self.stage==6 and self.command(qmp,text,part,'saveas occupied.txt'):self.stage=7
        elif self.stage==7 and 'Save refused: destination exists' in text:
            self.record(qmp,part,text,'exclusive-collision','/');self.stage=8
        elif self.stage==8 and self.command(qmp,text,part,'saveas copied.txt'):self.stage=9
        elif self.stage==9 and 'NORMAL  copied.txt' in text and 'Saved new file.' in text:
            self.record(qmp,part,text,'saved-as','/');self.stage=10
        elif self.stage==10 and self.command(qmp,text,part,'q'):self.stage=11
        elif ((self.reed_no_conflict and self.stage==7) or
              (not self.reed_no_conflict and self.stage==11)):
            restored=not re.search(r'^(?:NORMAL|INSERT|SELECT(?:-[A-Z]+)?|COMMAND|SEARCH)\s+',text,re.M)
            if self.reed_from_shoot:restored=restored and 'developer ~:' in text
            if restored:
                self.record(qmp,part,text,'terminal-restored',None);self.stage+=1
    def finish(self,expected_failure,failed):
        expected=(9 if self.phase=='readonly' else 3 if self.phase=='verify' else
                  9 if self.reed_from_shoot else 8 if self.reed_no_conflict else 12)
        if (expected_failure or failed or self.stage != expected or
            (self.reed_from_shoot and self.phase=='create' and
             (self.shell_stage!=23 or self.filename_probe_stage!=6 or
              not any(action['action']=='list-space-filename'
                      for action in self.actions)))):
            raise ValueError(f'incomplete real interactive workflow reed_stage={self.stage} shell_stage={self.shell_stage}')

def prepare(folder,build,run,backend,signal_port):
    disk=secondary.prepare_boot(folder,build,run,backend)
    with disk.open('rb') as f:f.seek(guest.ROOT_OFFSET);image=bytearray(f.read(guest.PART_BYTES))
    target=f'/vol/{secondary.LABELS[backend]}/work'
    for phase in ('create','verify','readonly','negative'):
        mode='verify' if phase=='negative' else phase
        script=(f'/bin/fstest volume-safe-{mode} {run} {secondary.LABELS[backend]} {backend} {target} {signal_port} > /temp/{phase}.log\n'
                f'/bin/say child-exit-0-{phase}-{run} > /temp/{phase}.exit\n/bin/shutdown\n').encode()
        secondary.replace(image,f'/temp/{phase}.shoot',script)
    secondary.write_partition(disk,image,guest.ROOT_OFFSET)
    guest.check_image(build,secondary.save(folder/'safe-prepared-root.img',image),folder/'safe-root-check.log')
    return disk

def read_media(folder,disk,backend,build,run,phase,wrong=False,reed_no_conflict=False,
               reed_from_shoot=False):
    data=secondary.partition(disk);image=secondary.save(folder/f'{phase}-secondary.img',data);log=folder/f'{phase}-media.log'
    identity=secondary.filesystem_identity(data,backend)
    if identity!=secondary.filesystem_identity((folder/'initial-secondary.img').read_bytes(),backend):raise ValueError('changed filesystem identity')
    reader=secondary.ExfatReader(data) if backend=='exfat' else None
    if backend=='mgfs':guest.check_image(build,image,log)
    else:guest.command(['/usr/sbin/fsck.'+('fat' if backend=='fat32' else 'exfat'),'-n',image],log)
    def read(path):
        if backend=='mgfs':return guest.payload(data,guest.find(data,path)[1])
        if backend=='exfat':
            item=reader.find(path)
            if item['directory'] or item['valid']!=item['length']:raise ValueError('invalid independently decoded file')
            return item['data']
        output=folder/(phase+'-'+path.replace('/','_'))
        guest.command(['mcopy','-i',image,'::'+path,output],log);return output.read_bytes()
    directory=f'/work/safe-{run}';identities={};hashes={}
    for size in SIZES:
        path=f'{directory}/s{size}.bin';actual=read(path);want=pattern(run,size)
        if wrong and size==8193:want=bytes([want[0]^1])+want[1:]
        if actual!=want:
            first=next((i for i,(a,b) in enumerate(zip(actual,want)) if a!=b),min(len(actual),len(want)))
            raise ValueError(f'independent safe content mismatch {path} offset={first} actual={len(actual)} expected={size}')
        hashes[path]=hashlib.sha256(actual).hexdigest()
        if backend=='mgfs':identities[size]=guest.find(data,path)[0]
        # FAT/exFAT native identities describe mount-local entry positions; their
        # consistency is verified across snapshot and stat, not synthesized here.
    expected_text={'/work/reed.txt':b'editedbase\n' if reed_no_conflict else b'external\n','/work/copied.txt':b'editedbase\n','/work/occupied.txt':b'occupied\n','/work/seed.txt':secondary.SEED}
    if reed_from_shoot:
        expected_text['/work/hello world!']=b'argv-probe\n'
        expected_text['/work/hello quoted!']=b'quoted-probe\n'
    for path,want in expected_text.items():
        if read(path)!=want:raise ValueError('independent application content mismatch '+path)
    def names(path):
        if backend=='mgfs':return [r[4].decode() for r in guest.image_tool.directory_entries(guest.payload(data,guest.find(data,path)[1])) if r[3]&1]
        if backend=='exfat':return list(reader.entries(reader.find(path)['data']))
        listing=folder/(phase+'-'+path.replace('/','_')+'-listing.log')
        guest.command(['mdir','-b','-i',image,'::'+path],listing)
        return fat_listing_names(listing.read_text())
    if sorted(names(directory))!=sorted(f's{s}.bin' for s in SIZES):raise ValueError('safe namespace mismatch')
    if sorted(names('/work'))!=sorted(work_names(backend,run,reed_from_shoot)):raise ValueError('application namespace mismatch')
    return {'filesystem_identity':identity,'partition_sha256':hashlib.sha256(data).hexdigest(),'file_hashes':hashes,'checker':'clean'},identities

def verify_rows(size,identifier):
    return [('stat',0),('type',1),('length',size),('open-read',1),('snapshot',0),('snapshot-length',size),('snapshot-identity',identifier),('snapshot-revision',1),('snapshot-bytes',1),('read',size),('read-bytes',1),('eof',0),('read-only-replace',-3),('close-read',0)]

def rows_safe(phase,backend,identities):
    rows=[('owners-before',0)]
    if phase=='create':rows += [('mkdir',0)]
    for size in SIZES:
        if phase=='create':
            rows += [('exclusive',0),('handle',1),('collision',-9),('collision-handle',0)]
            if backend!='mgfs':rows += [('case-collision',-9)]
            rows += [('snapshot-initial',0),('initial-length',size),('initial-bytes',1)]
            if size>1:rows += [('short-snapshot',-10),('short-state',1)]
            rows += [('peer-open',1),('peer-replace',0),('stale-refusal',-34),('refresh',0),('peer-bytes',1),('stable-identity',identities[size]),('new-revision',1),('seek',0),('replace',0),('repeat-replace',0),('ordinary-after-replace',1 if size else 0),('ordinary-rewind',0),('snapshot-final',0),('final-bytes',1),('offset-preserved',size),('offset-bytes',1),('rename-open',0),('snapshot-moved',0),('moved-bytes',1),('move-back',0),('peer-close',0),('close',0)]
        rows += verify_rows(size,identities[size])
    rows += [('directory-open',1),('directory-eof',-11),('namespace',7),('directory-close',0)]
    if phase=='create':rows += [('reuse-create',0),('reuse-close',0),('reuse-remove',0),('reuse-recreate',0),('reuse-reclose',0),('reuse-reremove',0),('unicode-create',0),('unicode-close',0),*([('unicode-collision',-9)] if backend!='mgfs' else [('unicode-distinct',0),('unicode-peer-close',0),('unicode-peer-remove',0)]),('unicode-remove',0),('missing-parent',-2),('permission',-8),('too-large',-35)]
    return rows+[('owners-after',0)]

def check_rows(body,rows):
    got=re.findall(r'^SOP (\d+) ([\w-]+) expected=(-?\d+) observed=(-?\d+)$',body,re.M)
    if len(re.findall(r'^SOP ',body,re.M))!=len(got):raise ValueError('malformed native operation')
    want=[(str(i),name,str(n),str(n)) for i,(name,n) in enumerate(rows,1)]
    if got!=want:
        first=next((i for i,pair in enumerate(zip(got,want)) if pair[0]!=pair[1]),min(len(got),len(want)))
        raise ValueError(f'native operation mismatch {first}: got={got[first:first+1]} expected={want[first:first+1]}')

def validate_deliberate_shoot_failure(body):
    # The command reports its error; Shoot propagates ordinary nonzero exits
    # without a second generic diagnostic.
    expected='Could not read "/temp/no-such-safe-input": not found.\n'
    if body!=expected:raise ValueError('wrong deliberate script-error evidence')

def evidence(folder,disk,phase,backend,run,build,identities,reed_no_conflict=False,
             reed_from_shoot=False,reed_guest_conflict_sync=False):
    with disk.open('rb') as f:f.seek(guest.ROOT_OFFSET);image=f.read(guest.PART_BYTES)
    guest.check_image(build,secondary.save(folder/f'{phase}-root.img',image),folder/f'{phase}-root-checker.log')
    text=guest.payload(image,guest.find(image,f'/temp/{phase}.log')[1]).decode('ascii');(folder/f'{phase}-guest.log').write_text(text)
    if phase=='readonly':
        blocks=re.findall(r'^READSAFE1 phase=.*?^READSAFE1 complete[^\n]*\n',text,re.M|re.S)
        if len(blocks)!=1:raise ValueError('missing read-only native execution')
        rows=[('readonly-exclusive',-8),('readonly-output-unchanged',123),('readonly-absent',-2)]
        size=8193
        if backend!='mgfs':
            ids=re.findall(r'^SOP \d+ snapshot-identity expected=(\d+) observed=\d+$',blocks[0],re.M)
            if len(ids)!=1 or not int(ids[0]):raise ValueError('missing read-only snapshot identity')
            identities[size]=int(ids[0])
        rows+=verify_rows(size,identities[size])
        rows += [('readonly-getcwd',1),('readonly-chdir',0),('readonly-reed-spawn',1),('readonly-reed-wait',0),('readonly-reed-exit',0),('readonly-reed-close',0),('readonly-reed-absent',-2),('readonly-restore',0),('readonly-no-owner',0)]
        check_rows(blocks[0],rows)
        if not blocks[0].startswith(f'READSAFE1 phase=readonly run={run}\n') or not blocks[0].endswith(f'READSAFE1 complete phase=readonly run={run} ops={len(rows)}\n'):raise ValueError('read-only phase mismatch')
        body=blocks[0];r=[]
    else:
        return evidence_writable(folder,disk,phase,backend,run,build,identities,text,image,
                                 reed_no_conflict,reed_from_shoot,
                                 reed_guest_conflict_sync)
    return evidence_volume(text,image,phase,backend,run,len(rows)-9,9,identities,reed_no_conflict,reed_from_shoot)

def evidence_writable(folder,disk,phase,backend,run,build,identities,text,image,
                      reed_no_conflict=False,reed_from_shoot=False,
                      reed_guest_conflict_sync=False):
    blocks=re.findall(r'^SAFE1 phase=.*?^SAFE1 complete[^\n]*\n',text,re.M|re.S)
    if len(blocks)!=1:raise ValueError('missing safe execution')
    body=blocks[0]
    # Resolve only relational native IDs on FAT/exFAT; exact consistency still
    # required against stat, initial snapshot and refresh on every operation.
    if backend!='mgfs':
        values=[int(n) for n in re.findall(r'^SOP \d+ snapshot-identity expected=(\d+) observed=\d+$',body,re.M)]
        if len(values)!=len(SIZES) or len(set(values))!=len(values) or not all(values):raise ValueError('invalid native identities')
        identities=dict(zip(SIZES,values))
    rows=rows_safe(phase,backend,identities);check_rows(body,rows)
    if phase in ('create','verify'):
        if re.findall(r'^RT1 PASS core-c17-runtime$',body,re.M)!=['RT1 PASS core-c17-runtime']:
            raise ValueError('missing native runtime foundation witness')
        body=re.sub(r'^RT1 PASS core-c17-runtime\n','',body,flags=re.M)
    header=f'SAFE1 phase={phase} run={run} filesystem={backend} target=/vol/{secondary.LABELS[backend]}/work\n'
    if not body.startswith(header) or not body.endswith(f'SAFE1 complete phase={phase} run={run} ops={len(rows)}\n'):raise ValueError('safe phase identity/completion')
    if re.findall(r'^SAFE1 size=(\d+)$',body,re.M)!=list(map(str,SIZES)):raise ValueError('missing boundary witnesses')
    reed=re.findall(r'^REED1 phase=.*?^REED1 complete[^\n]*\n',text,re.M|re.S)
    if len(reed)!=1:raise ValueError('missing application completion')
    r=[('reed-getcwd',1),('reed-chdir',0)]
    if phase=='create' and reed_no_conflict and reed_from_shoot:r += [('reed-shoot-launch',1),('reed-shoot-shell-status',0)]
    elif phase=='create' and reed_no_conflict:r += [('reed-source',0),('reed-source-close',0),('reed-occupied',0),('reed-occupied-close',0),('reed-spawn',1),('reed-clock',0),('reed-first-save',1),('reed-wait',0),('reed-exit',0),('reed-child-close',0)]
    elif phase=='create' and reed_guest_conflict_sync:r += [('reed-source',0),('reed-source-close',0),('reed-occupied',0),('reed-occupied-close',0),('reed-sync-ready-absent',-2),('reed-sync-release-absent',-2),('reed-spawn',1),('reed-clock',0),('reed-sync-ready',1),('text-open',1),('text-snapshot',0),('text-bytes',1),('text-close',0),('reed-sync-ready-content',0),('reed-peer',1),('reed-external',0),('reed-peer-close',0),('reed-release-create',0),('reed-release-handle',1),('reed-release-close',0),('reed-wait',0),('reed-exit',0),('reed-child-close',0),('reed-sync-ready-remove',0),('reed-sync-release-remove',0)]
    elif phase=='create':r += [('reed-source',0),('reed-source-close',0),('reed-occupied',0),('reed-occupied-close',0),('reed-network',0),('reed-network-ready',1),('reed-signal-open',0),('reed-channel-ready',1),('reed-spawn',1),('reed-clock',0),('reed-first-save',1),('reed-peer',1),('reed-external',0),('reed-peer-close',0),('reed-external-witness',len(f'REED1 external run={run}\n')),('reed-signal-close',0),('reed-wait',0),('reed-exit',0),('reed-child-close',0)]
    else:r += [('reed-reopen',1),('reed-rewait',0),('reed-reexit',0),('reed-reclose',0)]
    r += [('text-open',1),('text-snapshot',0),('text-bytes',1),('text-close',0)]*3+[('reed-restore-cwd',0),('reed-no-owner',0)]
    check_rows(reed[0],r)
    if not reed[0].startswith(f'REED1 phase={phase} run={run}\n') or not reed[0].endswith(f'REED1 complete phase={phase} run={run} ops={len(r)}\n'):raise ValueError('Reed phase identity/completion')
    if phase=='create':
        shoot=re.findall(r'^SHOOT1 phase=.*?^SHOOT1 complete[^\n]*\n',text,re.M|re.S)
        if len(shoot)!=1:raise ValueError('missing real Shoot child')
        sr=[]
        if reed_from_shoot:sr += [('shoot-reed-marker-absent',-2),('shoot-reed-source-create',0),('shoot-reed-source-close',0),('shoot-occupied-create',0),('shoot-occupied-close',0),('shoot-space-file-create',0),('shoot-space-file-close',0),('shoot-quoted-file-create',0),('shoot-quoted-file-close',0),('shoot-reed-script-create',0),('shoot-reed-script-close',0)]
        sr += [('shoot-spawn',1),('shoot-wait',0),('shoot-status',0)]
        if reed_from_shoot:sr += [('shoot-reed-marker',0),('text-open',1),('text-snapshot',0),('text-bytes',1),('text-close',0)]
        sr += [('shoot-close',0)]
        sr += [('text-open',1),('text-snapshot',0),('text-bytes',1),('text-close',0)]
        if reed_from_shoot:sr += [('text-open',1),('text-snapshot',0),('text-bytes',1),('text-close',0)]
        sr += [('shoot-script-create',0),('shoot-script-close',0),('shoot-bad-create',0),('shoot-bad-close',0),('shoot-script-spawn',1),('shoot-script-wait',0),('shoot-script-status',0),('shoot-script-child-close',0)]
        sr += [('text-open',1),('text-snapshot',0),('text-bytes',1),('text-close',0)]
        sr += [('shoot-bad-spawn',1),('shoot-bad-wait',0),('shoot-bad-status',1),('shoot-bad-child-close',0),('shoot-no-continuation',-2)]
        check_rows(shoot[0],sr)
        if not shoot[0].startswith(f'SHOOT1 phase=create run={run}\n') or not shoot[0].endswith(f'SHOOT1 complete phase=create run={run} ops={len(sr)}\n'):raise ValueError('Shoot completion')
    for name in ('shoot.txt','shoot-copy.txt'):
        if guest.payload(image,guest.find(image,'/temp/'+name)[1])!=b'shoot\n':raise ValueError('Shoot persisted redirection mismatch')
    long_expected=(INTERACTIVE_LONG_TEXT+'\n').encode()
    if reed_from_shoot and guest.payload(image,guest.find(image,'/temp/interactive-long.txt')[1])!=long_expected:
        raise ValueError('interactive wrapped-command redirection mismatch')
    history=guest.payload(image,guest.find(image,'/home/developer/.state/shoot/history')[1])
    required_history=(b'/bin/say shoot > /temp/shoot.txt\n',b'history -a\n')
    if reed_from_shoot:
        required_history += (
            ('/bin/say '+INTERACTIVE_LONG_TEXT+' > /temp/interactive-long.txt\n').encode(),
            b'type /temp/shoot.txt\n',
            f'jump /vol/{secondary.LABELS[backend]}/work/\n'.encode(),
            b'jump /home/developer\n',
            f'/bin/shoot /temp/reed-launch-{run}.shoot\n'.encode(),
            f'type /vol/{secondary.LABELS[backend]}/work/reed.txt\n'.encode(),
            b'list\n',
            b'exit\n')
    if any(command not in history for command in required_history):raise ValueError('Shoot history did not persist the qualified interactive commands')
    try:guest.find(image,'/temp/forbidden')
    except ValueError:pass
    else:raise ValueError('failed Shoot script continued')
    if 'FAIL' in text:raise ValueError('failed native assertion')
    return evidence_volume(text,image,phase,backend,run,len(rows),len(r),identities,
                           reed_no_conflict,reed_from_shoot,
                           reed_guest_conflict_sync)

def validate_sections(text,phase,backend,run,safe_count,reed_count,
                      reed_no_conflict=False,reed_from_shoot=False,
                      reed_guest_conflict_sync=False):
    prefix=f'VOLUME1 phase=volume-safe-{phase} run={run} label={secondary.LABELS[backend]} filesystem={backend} target=/vol/{secondary.LABELS[backend]}/work\n'
    if not text.startswith(prefix):raise ValueError('wrong volume phase header')
    names=['SHOOT1','SAFE1','REED1'] if phase=='create' else ['SAFE1','REED1'] if phase=='verify' else ['READSAFE1']
    remaining=text;last=-1;shoot_operations=0
    for name in names:
        blocks=re.findall(r'^'+name+r' phase=.*?^'+name+r' complete[^\n]*\n',text,re.M|re.S)
        if len(blocks)!=1:raise ValueError('missing/duplicate application section')
        block=blocks[0];at=text.index(block)
        if at<last:raise ValueError('application section order')
        last=at+len(block)
        body=re.sub(r'^SOP \d+ [\w-]+ expected=-?\d+ observed=-?\d+\n','',block,flags=re.M)
        body=re.sub(r'^'+name+r' (?:phase|complete)[^\n]*\n','',body,flags=re.M)
        if name=='SAFE1':body=re.sub(r'^SAFE1 size=\d+\n','',body,flags=re.M)
        if name=='SAFE1' and phase in ('create','verify'):
            body=re.sub(r'^RT1 PASS core-c17-runtime\n','',body,flags=re.M)
        if name=='SHOOT1':
            completion=re.search(r'^SHOOT1 complete[^\n]* ops=([0-9]+)\n$',block,re.M)
            if not completion or int(completion.group(1))!=len(re.findall(r'^SOP ',block,re.M)):
                raise ValueError('Shoot operation count differs from completion')
            shoot_operations=int(completion.group(1))
            validate_deliberate_shoot_failure(body)
            body=''
        if (name=='REED1' and phase=='create' and not reed_no_conflict and
            not reed_guest_conflict_sync):
            attempts=re.findall(r'^REED1 channel-attempt=(\d+) sent=(-?\d+) received=(-?\d+)\n',body,re.M)
            if not 1<=len(attempts)<=64:raise ValueError('missing bounded channel readiness')
            for i,(attempt,sent,received) in enumerate(attempts,1):
                if int(attempt)!=i or int(sent) not in (-17,len(f'REED1 channel run={run}\n')) or int(received)!= (16 if i==len(attempts) else -18):raise ValueError('wrong channel readiness result')
            body=re.sub(r'^REED1 channel-attempt=\d+ sent=-?\d+ received=-?\d+\n','',body,flags=re.M)
        if body:raise ValueError('unknown application transcript line')
        remaining=remaining.replace(block,'',1)
    # Each section's rows are validated against the native operation oracle
    # before this aggregate check; reject additional rows outside sections.
    if len(re.findall(r'^SOP ',text,re.M))!=safe_count+reed_count+shoot_operations:raise ValueError('out-of-section native operation')
    tail=text.index(' final-unmount expected=')
    if tail<last:raise ValueError('unmount before application completion')
    if text.count('CRT1 atexit\n')!=1 or not text.endswith('CRT1 atexit\n'):
        raise ValueError('missing native CRT atexit completion witness')
    remaining=remaining.replace('CRT1 atexit\n','',1)
    remaining=re.sub(r'^VOP \d+ [\w-]+ expected=-?\d+ observed=-?\d+\n|^VOLUME1 [^\n]+\n','',remaining,flags=re.M)
    if remaining:raise ValueError('unknown volume transcript line')

def evidence_volume(text,image,phase,backend,run,safe_count,reed_count,identities,
                    reed_no_conflict=False,reed_from_shoot=False,
                    reed_guest_conflict_sync=False):
    validate_sections(text,phase,backend,run,safe_count,reed_count,
                      reed_no_conflict,reed_from_shoot,
                      reed_guest_conflict_sync)
    vrows=secondary.expected_vops('readonly' if phase=='readonly' else 'verify',backend)
    # shared startup and confirmed unmount use the unchanged production client.
    split=vrows.index(('seed-open',1))
    setup=secondary.expected_vops('create',backend)
    end=setup.index(('seed-close',0))+1
    vrows=setup[:end] if phase=='create' else vrows[:split]+vrows[split:split+4]
    vrows += [('final-unmount',0),('final-detached',-2),('final-snapshot',0),('final-suppressed',0)]
    actual=re.findall(r'^VOP (\d+) ([\w-]+) expected=(-?\d+) observed=(-?\d+)$',text,re.M)
    if actual!=[(str(i),name,str(n),str(n)) for i,(name,n) in enumerate(vrows,1)]:raise ValueError('volume admission/unmount evidence mismatch')
    ids=re.findall(r'^VOLUME1 identity instance=([1-9]\d*) parent=([1-9]\d*) name=\S+ mount=/vol/'+secondary.LABELS[backend]+'$',text,re.M)
    if len(ids)!=1 or ids[0][0]==ids[0][1]:raise ValueError('missing/ambiguous volume identity')
    expected_end=(f'VOLUME1 complete phase=volume-safe-{phase} run={run} '
                  f'ops={len(vrows)}\nCRT1 atexit\n')
    if not text.endswith(expected_end):raise ValueError('missing volume/CRT completion')
    if guest.payload(image,guest.find(image,f'/temp/{phase}.exit')[1])!=f'child-exit-0-{phase}-{run}\n'.encode():raise ValueError('missing waited child')
    return {'safe_assertions':safe_count,'reed_assertions':reed_count,'volume_assertions':len(vrows),'native_identities':identities}


def negative(folder,disk,secondary_disk,backend,args,run):
    # Change only stopped disposable media. The guest's expected pattern stays
    # independent and unchanged; verify must fail rather than recreate/repair it.
    path=f'/work/safe-{run}/s8193.bin';data=bytearray(secondary.partition(secondary_disk))
    if backend=='mgfs':
        extents,_=guest.image_tool.record_extents(data,guest.find(data,path)[1])
        if not extents or extents[0][0]!=0:raise ValueError('unsupported wrong-content fixture')
        data[extents[0][1]*4096]^=1
    elif backend=='exfat':
        reader=secondary.ExfatReader(data);item=reader.find(path)
        first=secondary.u32(item['entry'],52);data[reader.address(first)]^=1
    else:
        image=folder/'wrong-fat.img';image.write_bytes(data)
        payload=folder/'wrong-payload.bin';payload.write_bytes(bytes([pattern(run,8193)[0]^1])+pattern(run,8193)[1:])
        guest.command(['mcopy','-o','-i',image,payload,'::'+path],folder/'wrong-fat.log');data=bytearray(image.read_bytes())
    secondary.write_partition(secondary_disk,data)
    try:read_media(folder,secondary_disk,backend,args.build_dir,run,'host-negative')
    except ValueError as error:
        if f'independent safe content mismatch {path} offset=0' not in str(error):raise
    else:raise ValueError('host oracle accepted incorrect contents')
    args.lifetime_control=None;args.confirmations=[]
    outcome=guest.boot(folder,disk,'negative',args)
    with disk.open('rb') as f:f.seek(guest.ROOT_OFFSET);root=f.read(guest.PART_BYTES)
    guest.check_image(args.build_dir,secondary.save(folder/'negative-root.img',root),folder/'negative-root-check.log')
    text=guest.payload(root,guest.find(root,'/temp/negative.log')[1]).decode('ascii');(folder/'negative-guest.log').write_text(text)
    want=pattern(run,8193)[0]
    if f'FAIL safe-byte offset=0 expected={want} observed={want^1}\n' not in text or 'SAFE1 complete' in text:raise ValueError('negative failed for wrong reason')
    try:guest.find(root,'/temp/negative.exit')
    except ValueError as error:
        if str(error)!='namespace mismatch: /temp/negative.exit':raise
    else:raise ValueError('negative child continued script')
    media,_=read_media(folder,secondary_disk,backend,args.build_dir,run,
                       'after-negative',wrong=True,
                       reed_no_conflict=args.heap_profile or args.reed_no_conflict)
    return {**outcome,'status':'EXPECTED_FAILURE','host_oracle':'rejected byte zero','media':media}

def heap_profile_evidence(folder,disk,phase,requested_cpus):
    with disk.open('rb') as source:
        source.seek(guest.ROOT_OFFSET)
        image=source.read(guest.PART_BYTES)
    raw=guest.payload(image,guest.find(image,'/temp/heap-profile.bin')[1])
    directory=folder/phase/'heap-profile'
    directory.mkdir()
    artifact=directory/'snapshot.bin'
    artifact.write_bytes(raw)
    summary=heap_profile_analyze.parse_snapshot(raw,expected_phase=1)
    if summary['cpu_online_count'] < requested_cpus:
        raise ValueError(f"profile saw {summary['cpu_online_count']} online CPUs, "
                         f"requested {requested_cpus}")
    events=summary.pop('event_trace_records')
    trace_path=directory/'allocation-events.csv'
    trace_sha256=summary['event_trace']['sha256']
    lines=[f'PITH-ALLOC-TRACE-2,{len(events)},{trace_sha256}']
    event_codes={'allocated':1,'freed':2,'failed':3}
    context_codes={'normal':0,'irq_disabled':1,'spinlock_held':2}
    lines.extend(','.join(str(value) for value in (
        event['sequence'],event_codes[event['event']],event['object_id'],
        event['requested_bytes'],event['cpu'],
        context_codes[event['context']],event['phase'],
        event['caller_text_offset'])) for event in events)
    trace_bytes=('\n'.join(lines)+'\n').encode('ascii')
    trace_path.write_bytes(trace_bytes)
    summary['event_trace']['export_file']=trace_path.name
    summary['event_trace']['export_sha256']=hashlib.sha256(trace_bytes).hexdigest()
    (directory/'summary.json').write_text(json.dumps(summary,indent=2,sort_keys=True)+'\n')
    result={'snapshot_bytes':len(raw),'snapshot_sha256':hashlib.sha256(raw).hexdigest(),
            'tracked_live_at_snapshot':summary['totals']['tracked_live'],
            'allocation_successes':summary['totals']['successes'],
            'failed_requests':summary['totals']['failures'],
            'cross_cpu_frees':summary['totals']['cross_cpu_frees'],
            'candidate_count':len(summary['slab_candidates']),
            'trace_events':len(events),
            'trace_sha256':summary['event_trace']['sha256'],
            'trace_export_sha256':summary['event_trace']['export_sha256']}
    hybrid=summary.get('hybrid_runtime')
    if hybrid is not None:
        routes=hybrid['route_counters']
        result['hybrid_runtime']={
            'status':hybrid['status'],
            'online_cpus':hybrid['online_cpus'],
            'route_counters':routes,
            'resources':hybrid['resources'],
            'classifier':hybrid['classifier'],
            'classes':[row for row in hybrid['classes']
                       if row['allocations'] or row['frees'] or
                       row['backing_allocations'] or row['cached_slots']],
            'per_cpu_classes':[row for row in hybrid['per_cpu_classes']
                               if row['allocations'] or row['frees'] or
                               row['cache_hits'] or row['cache_misses'] or
                               row['cross_cpu_frees'] or row['cached_objects']],
            'event_trace_is_complete_public_history':False,
        }
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-dir',type=Path,required=True)
    p.add_argument('--output',type=Path,help='new output directory; retained for review (default: temporary)')
    artifacts.add_retention_argument(p)
    p.add_argument('--backend',choices=['mgfs','fat32','exfat','all'],default='all');p.add_argument('--repetitions',type=int,choices=range(1,4),default=2)
    p.add_argument('--faults',action='store_true',help='also qualify native replacement flush failure on a private copy');p.add_argument('--negative-control',action='store_true');p.add_argument('--readonly',action='store_true');p.add_argument('--heap-profile',action='store_true',help='require and validate profile-build snapshots from each native fixture');p.add_argument('--reed-no-conflict',action='store_true',help='exercise Reed without the host UDP stale-change rendezvous');p.add_argument('--reed-guest-conflict-sync',action='store_true',help='coordinate stale-save mutation through a bounded guest-side file handshake');p.add_argument('--keyboard-repeat',action='store_true',help='qualify held H navigation and release through the native USB keyboard path');p.add_argument('--cpus',type=int,choices=(1,2),default=1);p.add_argument('--run-id');p.add_argument('--accel',choices=['tcg','kvm'],default=guest.default_accel());p.add_argument('--qemu',default='qemu-system-x86_64')
    p.add_argument('--ovmf-code',type=Path,default=Path('/usr/share/OVMF/OVMF_CODE_4M.fd'));p.add_argument('--ovmf-vars',type=Path,default=Path('/usr/share/OVMF/OVMF_VARS_4M.fd'))
    p.add_argument('--timeout',type=int,metavar='SECONDS',default=600)
    a=p.parse_args()
    error=guest.accel_error(a.accel,a.qemu)
    if error: p.error(error)
    if a.heap_profile and a.reed_no_conflict:
        p.error('--heap-profile already selects the no-conflict Reed workflow')
    if a.reed_guest_conflict_sync and (a.heap_profile or a.reed_no_conflict):
        p.error('guest conflict-sync and no-conflict Reed workflows are mutually exclusive')
    if a.keyboard_repeat and (a.heap_profile or not a.reed_no_conflict):
        p.error('--keyboard-repeat requires --reed-no-conflict')
    if not 30<=a.timeout<=600:p.error('boot timeout must be 30..600 seconds')
    for path in (a.build_dir,a.output,a.ovmf_code,a.ovmf_vars):
        if path is None:continue
        if ',' in str(path) or '\n' in str(path):p.error('QEMU paths cannot contain commas or newlines')
    if a.output is not None and a.output.exists():p.error('output directory must be new')
    a.build_dir=a.build_dir.resolve()
    workspace=artifacts.ArtifactWorkspace(a.output,name='guest-safe',retain=a.retain_artifacts)
    a.output=workspace.path;a.guest_poll_interval=.1
    receiver=None
    reed_no_conflict=a.heap_profile or a.reed_no_conflict
    behavior=('USB keyboard: Shoot command/path completion, history draft restore, cursor editing, wrapped redirection and script launch; Reed held-key repeat/release, edit/undo, Shift+U redo, save, Save As, prompt restoration and shell exit; no external stale-change rendezvous'
              if a.reed_no_conflict else
              'Reed opens a captured generation; fstest changes the file through VFS before the editor becomes interactive; keyboard conflict cancellation and Save As preserve both versions'
              if a.reed_guest_conflict_sync else
              'edit and save; no external stale-change rendezvous' if a.heap_profile else None)
    report={'status':'FAIL','runs':[],'acceleration':a.accel,'cpus':a.cpus,'requested':{'readonly':a.readonly,'wrong_content':a.negative_control,'replacement_flush_failure':a.faults,'heap_profile':a.heap_profile,'reed_no_conflict':a.reed_no_conflict,'reed_guest_conflict_sync':a.reed_guest_conflict_sync,'keyboard_repeat':a.keyboard_repeat,'reed_from_shoot':a.reed_no_conflict,'reed_test_behavior':behavior}};start=time.monotonic();cpu=time.process_time()
    try:
        report['binary_sha256']={name:guest.digest(a.build_dir/path) for name,path in [('fstest','fstest/fstest.elf'),('reed','reed/reed.elf'),('shoot','shoot/shoot.elf'),('kernel','Mangrove/pith.elf'),('boot','EFI/BOOT/BOOTX64.EFI')]}
        if a.run_id and (not re.fullmatch('[0-9a-f]{16}',a.run_id) or a.repetitions!=1 or a.backend=='all'):raise ValueError('exact run requires one backend/campaign')
        artifacts.check_free_space(
            a.output, 3 * 1024**3 * a.repetitions *
            (3 if a.backend == 'all' else 1),
            'safe-file guest images and failure evidence')
        for backend in ('mgfs','fat32','exfat') if a.backend=='all' else (a.backend,):
            for rep in range(a.repetitions):
                run=a.run_id or secrets.token_hex(8);folder=a.output/f'{backend}-{rep}';folder.mkdir()
                entry={'backend':backend,'run':run,'phases':{}};report['runs'].append(entry)
                if reed_no_conflict or a.reed_guest_conflict_sync:
                    receiver=None
                    signal_port=1024
                else:
                    receiver=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);receiver.bind(('127.0.0.1',0));receiver.setblocking(False)
                    signal_port=receiver.getsockname()[1]
                disk=prepare(folder,a.build_dir,run,backend,signal_port);a.secondary_disk=secondary.create_secondary(folder,a.build_dir,backend)
                entry['initial_secondary_sha256']=guest.digest(a.secondary_disk)
                for phase in ('create','verify',*(['readonly'] if a.readonly else [])):
                    a.lifetime_control=ReedControl(run,phase,receiver if phase=='create' else None);a.lifetime_control.reed_no_conflict=reed_no_conflict;a.lifetime_control.reed_from_shoot=a.reed_no_conflict;a.lifetime_control.reed_guest_conflict_sync=a.reed_guest_conflict_sync;a.lifetime_control.keyboard_repeat=a.keyboard_repeat;a.lifetime_control.reed_path=f'/temp/reed-launch-{run}.shoot';a.lifetime_control.volume_label=secondary.LABELS[backend];a.confirmations=['y'];a.secondary_readonly=phase=='readonly'
                    before=guest.digest(a.secondary_disk) if phase=='readonly' else None
                    entry['active_phase']=phase
                    outcome=guest.boot(folder,disk,phase,a)
                    outcome['smp']=guest.inspect_smp_boot(disk,a.cpus)
                    media,identities=read_media(folder,a.secondary_disk,backend,a.build_dir,run,phase,reed_no_conflict=reed_no_conflict,reed_from_shoot=a.reed_no_conflict)
                    native=evidence(folder,disk,phase,backend,run,a.build_dir,identities,
                                    reed_no_conflict,a.reed_no_conflict,
                                    a.reed_guest_conflict_sync)
                    if phase=='create' and a.reed_no_conflict:
                        control=a.lifetime_control
                        witness_names={'edit-before-undo','undo-restores-base','redo-restores-edit',
                                       'redo-state-unchanged','saved-as',
                                       'terminal-restored','edit-before-repeat',
                                       'list-space-filename','space-filename-escaped-opened',
                                       'space-filename-quoted-opened'}
                        retained=[]
                        for index,action in enumerate(control.actions):
                            if action['action'] not in witness_names:continue
                            part_name=f'reed-{index:03}.txt'
                            retained.append(str((folder/'create'/part_name).relative_to(a.output)))
                            image_name=f'reed-{index:03}.ppm'
                            if (folder/'create'/image_name).is_file():
                                retained.append(str((folder/'create'/image_name).relative_to(a.output)))
                        if a.keyboard_repeat:
                            for name in ('reed-repeat-released-1.ppm',
                                         'reed-repeat-released-2.ppm'):
                                retained.append(str((folder/'create'/name).relative_to(a.output)))
                        retained.append(str((folder/'create'/'reed-actions.json').relative_to(a.output)))
                        report.setdefault('retained_artifacts',[]).extend(retained)
                        native['interactive_acceptance']={
                            'shoot_history_and_editing':'PASS',
                            'shoot_list_quotes_spaced_filename':(
                                'PASS' if any(action['action']=='list-space-filename'
                                              for action in control.actions) else 'INCOMPLETE'),
                            'shoot_escaped_filename_opens_in_reed':(
                                'PASS' if any(action['action']=='space-filename-escaped-opened'
                                              for action in control.actions) else 'INCOMPLETE'),
                            'shoot_quoted_filename_opens_in_reed':(
                                'PASS' if any(action['action']=='space-filename-quoted-opened'
                                              for action in control.actions) else 'INCOMPLETE'),
                            'command_and_path_completion':'PASS',
                            'script_and_redirection':'PASS',
                            'reed_save_readback_and_terminal_restore':'PASS',
                            'reed_keyboard_repeat':(
                                'PASS' if control.keyboard_repeat_evidence else
                                'INCOMPLETE'),
                            'reed_keyboard_repeat_evidence':
                                control.keyboard_repeat_evidence,
                            'reed_native_redo':('PASS' if control.redo_status=='observed'
                                                else 'INCOMPLETE'),
                            'reed_redo_input':'PASS' if control.redo_input_diagnostic else 'INCOMPLETE',
                            'reed_redo_input_diagnostic':control.redo_input_diagnostic,
                            'reed_redo_witness_screens':control.redo_witness_screens,
                            'redo_evidence':(
                                'Shift+U changed the rendered buffer back to editedbase.'
                                if control.redo_status=='observed' else
                                ('No native redo state was observed; test-build input evidence was '
                                 + repr(control.redo_input_diagnostic))
                                if control.redo_input_diagnostic else
                                'No native redo state or Reed input diagnostic was observed after the held QMP USB key chord.'),
                        }
                    if phase=='create' and a.reed_guest_conflict_sync:
                        control=a.lifetime_control
                        actions={row['action'] for row in control.actions}
                        retained=[]
                        for index,action in enumerate(control.actions):
                            if action['action'] not in {'edit-before-undo','undo-restores-base',
                                    'redo-input-received','redo-restores-edit',
                                    'redo-state-unchanged','stale-conflict',
                                    'cancel-preserved','saved-as','terminal-restored'}:
                                continue
                            retained.append(str((folder/'create'/f'reed-{index:03}.txt').relative_to(a.output)))
                            image_name=f'reed-{index:03}.ppm'
                            if (folder/'create'/image_name).is_file():
                                retained.append(str((folder/'create'/image_name).relative_to(a.output)))
                        retained.append(str((folder/'create'/'reed-actions.json').relative_to(a.output)))
                        report.setdefault('retained_artifacts',[]).extend(retained)
                        native['interactive_acceptance']={
                            'reed_undo_state':'PASS' if 'undo-restores-base' in actions else 'INCOMPLETE',
                            'reed_redo_input':'PASS' if control.redo_input_diagnostic else 'INCOMPLETE',
                            'reed_redo_input_diagnostic':control.redo_input_diagnostic,
                            'reed_redo_witness_screens':control.redo_witness_screens,
                            'reed_redo_state':'PASS' if 'redo-restores-edit' in actions else 'INCOMPLETE',
                            'stale_save_conflict':'PASS' if 'stale-conflict' in actions else 'INCOMPLETE',
                            'cancel_preserves_unsaved_edit':'PASS' if 'cancel-preserved' in actions else 'INCOMPLETE',
                            'external_file_preserved_and_edit_saved_as':'PASS' if 'saved-as' in actions else 'INCOMPLETE',
                        }
                    if phase=='verify':
                        control=a.lifetime_control
                        actions={row['action'] for row in control.actions}
                        retained=[]
                        for index,action in enumerate(control.actions):
                            if action['action'] not in {'reopen-loaded','terminal-restored'}:continue
                            retained.append(str((folder/'verify'/f'reed-{index:03}.txt').relative_to(a.output)))
                            image_name=f'reed-{index:03}.ppm'
                            if (folder/'verify'/image_name).is_file():
                                retained.append(str((folder/'verify'/image_name).relative_to(a.output)))
                        retained.append(str((folder/'verify'/'reed-actions.json').relative_to(a.output)))
                        report.setdefault('retained_artifacts',[]).extend(retained)
                        native['reed_reopened_inside_editor']='PASS' if 'reopen-loaded' in actions else 'INCOMPLETE'
                        native['terminal_restored_after_reopen']='PASS' if 'terminal-restored' in actions else 'INCOMPLETE'
                    if a.heap_profile:
                        native['heap_profile']=heap_profile_evidence(
                            folder,disk,phase,a.cpus)
                    (folder/phase/'reed-actions.json').write_text(json.dumps(a.lifetime_control.actions,indent=2))
                    if before and before!=guest.digest(a.secondary_disk):raise ValueError('write-protected secondary changed')
                    entry['phases'][phase]={**outcome,'media':media,'native':native}
                qualified=folder/'qualified-secondary.img';shutil.copyfile(a.secondary_disk,qualified)
                entry['qualified_secondary_sha256']=guest.digest(qualified)
                if a.faults:
                    import guest_lifetime_test as lifetime
                    fault=copy.copy(a);fault.output=folder/'replacement-flush';fault.source_media=qualified
                    fault.mutation='replace';fault.kind='flush';fault.replacement_phase='stage'
                    fault.negative_control=False;fault.run_id=run;fault.backend=backend
                    with (folder/'replacement-flush.log').open('w') as log,contextlib.redirect_stdout(log):lifetime.campaign(fault)
                    result=json.loads((fault.output/'report.json').read_text())
                    if result['status']!='PASS':raise ValueError('native replacement flush failure unqualified')
                    entry['replacement_flush_failure']=result
                    if result.get('retained_artifacts'):
                        report.setdefault('retained_artifacts', []).extend(
                            str((fault.output / name).relative_to(a.output))
                            for name in result['retained_artifacts'])
                if receiver is not None:receiver.close()
                receiver=None
                a.secondary_readonly=False
                if a.negative_control:entry['wrong_content']=negative(folder,disk,a.secondary_disk,backend,a,run)
                del entry['active_phase'];entry['status']='PASS'
        redo_gaps=[f"{entry['run']}: native Reed redo was not observed"
                   for entry in report['runs']
                   if entry.get('phases',{}).get('create',{}).get('native',{}).get(
                       'interactive_acceptance',{}).get('reed_native_redo')=='INCOMPLETE']
        report['status']='INCOMPLETE' if redo_gaps else 'PASS'
        if redo_gaps:report['qualification_gaps']=redo_gaps
    except Exception as error:
        report['error']=f'{type(error).__name__}: {error}'
        selected=[]
        for candidate in (locals().get('disk'), getattr(a,'secondary_disk',None)):
            if candidate is not None and Path(candidate).is_file():
                selected.append(str(Path(candidate).relative_to(a.output)))
        if selected:
            report['retained_artifacts']=selected[:2]
        raise
    finally:
        if receiver is not None:receiver.close()
        report.update(seconds=time.monotonic()-start,runner_cpu_seconds=time.process_time()-cpu,
                      runner_peak_rss_bytes=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss*1024,
                      artifact_bytes=sum(x.stat().st_size for x in a.output.rglob('*') if x.is_file()))
        report=workspace.finish(report,verified=report.get('status')=='PASS')
        print(json.dumps(report,indent=2))
if __name__=='__main__':main()
