#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production byte-string APIs, exact reference checks and historical ASan control."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
CC=sys.argv[1] if len(sys.argv)>1 else 'gcc'
CODE=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern char *probe_strncat(char *,const char *,size_t);
extern char *probe_strrchr(const char *,int);
extern char *probe_strstr(const char *,const char *);
extern char *probe_strpbrk(const char *,const char *);
extern size_t probe_strspn(const char *,const char *);
extern size_t probe_strcspn(const char *,const char *);
extern int probe_strcoll(const char *,const char *);
extern size_t probe_strxfrm(char *,const char *,size_t);
extern void *probe_memchr(const void *,int,size_t);
extern char *probe_strtok(char *,const char *);
extern char *probe_strcat(char *,const char *);
int main(void){
    char sample[] = { 'a', (char)0x80, 'b', 'a', 0 };
    assert(probe_strrchr(sample, 'a') == sample + 3);
    assert(probe_strrchr(sample, 0) == sample + 4);
    assert(probe_strrchr(sample, 'z') == NULL);
    const char haystack[] = "one two one";
    assert(probe_strstr(haystack, "two") == haystack + 4);
    assert(probe_strstr(haystack, "") == haystack);
    assert(probe_strstr("abc", "abcd") == NULL);
    assert(probe_memchr(sample, 0x180, sizeof sample) == sample + 1);
    assert(probe_memchr(sample, 'z', sizeof sample) == NULL);
    assert(probe_strspn("aaabxyz", "ab") == 4);
    assert(probe_strcspn("abcxyz", "xz") == 3);
    const char candidates[] = "abcdef";
    assert(probe_strpbrk(candidates, "xzde") == candidates + 3);
    assert(probe_strcoll("alpha", "beta") < 0);
    char transformed[5];
    assert(probe_strxfrm(transformed, "alphabet", sizeof transformed) == 8);
    assert(!strcmp(transformed, "alph"));
    assert(probe_strxfrm(NULL, "alphabet", 0) == 8);
    char combined[16] = "north";
    assert(probe_strcat(combined, "wind") == combined);
    assert(!strcmp(combined, "northwind"));
    char tokens[] = ",,red,,green,blue,,";
    assert(!strcmp(probe_strtok(tokens, ","), "red"));
    assert(!strcmp(probe_strtok(NULL, ","), "green"));
    assert(!strcmp(probe_strtok(NULL, ","), "blue"));
    assert(probe_strtok(NULL, ",") == NULL);
    assert(!strcmp(probe_strtok(tokens, ""), tokens));
    assert(probe_strtok(NULL, "") == NULL);
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
    printf("Production string APIs plus %u bounded strncat cases: PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-libc-string-') as name:
    folder=Path(name);fixture=folder/'test.c';fixture.write_text(CODE)
    old=folder/'historical.c'
    old.write_bytes(subprocess.check_output(['git','show','87db3a58a42604a2ef29e4d818f9006cfafe3823:libc/src/string.c'],cwd=ROOT))
    legacy_include=folder/'legacy-include';legacy_include.mkdir()
    (legacy_include/'string.h').write_text(r'''#pragma once
#include <stdint.h>
typedef unsigned long long usize;
typedef unsigned char u8;
usize strlen(const char *);
int strcmp(const char *,const char *);
int strncmp(const char *,const char *,usize);
char *strchr(const char *,int);
char *strcpy(char *,const char *);
char *strncpy(char *,const char *,usize);
char *strncat(char *,const char *,usize);
void *memset(void *,int,usize);
void *memcpy(void *,const void *,usize);
void *memmove(void *,const void *,usize);
int memcmp(const void *,const void *,usize);
''')
    flags=[CC,'-std=c17','-O1','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
           '-fno-builtin','-fsanitize=address,undefined','-fno-sanitize-recover=all']
    renames=['strlen','strcmp','strncmp','strchr','strrchr','strstr','strpbrk',
             'strspn','strcspn','strcpy','strncpy','strncat','strcoll','strxfrm',
             'strcat','memchr','memset','memcpy','memmove','memcmp']
    for historical,source in [(False,ROOT/'libc/src/string.c'),(True,old)]:
        obj=folder/'string.o';binary=folder/'test'
        includes=['-I'+str(legacy_include)] if historical else ['-I'+str(ROOT/'libc/include'),'-I'+str(ROOT/'include')]
        command=flags+includes+['-D'+n+'=probe_'+n for n in renames]
        command+=['-c',str(source),'-o',str(obj)]
        subprocess.run(command,check=True)
        selected_fixture=fixture
        if historical:
            selected_fixture=folder/'historical-test.c'
            selected_fixture.write_text(r'''#include <stdlib.h>
#include <string.h>
extern char *probe_strncat(char *, const char *, size_t);
int main(void) {
  char destination[8] = "a";
  char *source = malloc(2);
  if (!source) return 2;
  source[0] = 'b'; source[1] = 'c';
  (void)probe_strncat(destination, source, 2);
  free(source);
  return 0;
}
''')
        objects=[obj]
        if not historical:
            strtok_obj=folder/'strtok.o'
            subprocess.run(flags+includes+['-Dstrtok=probe_strtok','-c',
                str(ROOT/'libc/src/strtok.c'),'-o',str(strtok_obj)],check=True)
            objects.append(strtok_obj)
        subprocess.run(flags+[str(selected_fixture)]+[str(item) for item in objects]+
                        ['-o',str(binary)],check=True)
        result=subprocess.run([str(binary)],text=True,capture_output=True,timeout=15,
            env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
        if historical:
            assert result.returncode and 'AddressSanitizer: heap-buffer-overflow' in result.stderr and 'probe_strncat' in result.stderr,result.stderr
            print('Historical production control: exact-bound source overread detected by ASan')
        else:
            assert not result.returncode and not result.stderr,result.stdout+result.stderr
            print(result.stdout,end='')
