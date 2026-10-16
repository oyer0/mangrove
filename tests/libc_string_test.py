#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production strncat bounds, exact byte oracle and historical ASan control."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
CC=sys.argv[1] if len(sys.argv)>1 else 'gcc'
CODE=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern char *probe_strncat(char *,const char *,unsigned long long);
int main(void){
    unsigned cases=0;
    for(unsigned prefix=0;prefix<=8;prefix++)
    for(unsigned length=1;length<=24;length++)
    for(unsigned count=0;count<=length;count++)
    for(unsigned zero=0;zero<=length;zero++){
        char *source=malloc(length);assert(source);
        for(unsigned i=0;i<length;i++)source[i]=(char)(0x80U+i);
        if(zero<length)source[zero]=0;
        char actual[64],expected[64];memset(actual,0xa5,sizeof actual);
        for(unsigned i=0;i<prefix;i++)actual[i]='A';actual[prefix]=0;
        memcpy(expected,actual,sizeof expected);
        unsigned copied=0;
        while(copied<count && copied<length && source[copied]){
            expected[prefix+copied]=source[copied];copied++;
        }
        expected[prefix+copied]=0;
        assert(probe_strncat(actual,source,count)==actual);
        assert(!memcmp(actual,expected,sizeof actual));free(source);cases++;
    }
    printf("Production strncat exact/bounded/zero/truncated byte cases: %u PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-libc-string-') as name:
    folder=Path(name);fixture=folder/'test.c';fixture.write_text(CODE)
    old=folder/'historical.c'
    old.write_bytes(subprocess.check_output(['git','show','87db3a58a42604a2ef29e4d818f9006cfafe3823:libc/src/string.c'],cwd=ROOT))
    flags=[CC,'-std=c17','-O1','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
           '-fno-builtin','-fsanitize=address,undefined','-fno-sanitize-recover=all']
    renames=['strlen','strcmp','strncmp','strchr','strcpy','strncpy','strncat','memset','memcpy','memmove','memcmp']
    for historical,source in [(False,ROOT/'libc/src/string.c'),(True,old)]:
        obj=folder/'string.o';binary=folder/'test'
        subprocess.run(flags+['-I'+str(ROOT/'libc/include'),'-I'+str(ROOT/'include'),
            *['-D'+n+'=probe_'+n for n in renames],'-c',str(source),'-o',str(obj)],check=True)
        subprocess.run(flags+[str(fixture),str(obj),'-o',str(binary)],check=True)
        result=subprocess.run([str(binary)],text=True,capture_output=True,timeout=15,
            env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
        if historical:
            assert result.returncode and 'AddressSanitizer: heap-buffer-overflow' in result.stderr and 'probe_strncat' in result.stderr,result.stderr
            print('Historical production control: exact-bound source overread detected by ASan')
        else:
            assert not result.returncode and not result.stderr,result.stdout+result.stderr
            print(result.stdout,end='')
