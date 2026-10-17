/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

/* Each typed relocation checks that a public declaration has a target symbol. */
#define API(name) __typeof__(&name) volatile runtime_api_##name = &name

API(isalnum); API(isalpha); API(isblank); API(iscntrl); API(isdigit);
API(isgraph); API(islower); API(isprint); API(ispunct); API(isspace);
API(isupper); API(isxdigit); API(tolower); API(toupper);
API(imaxabs); API(imaxdiv); API(strtoimax); API(strtoumax);
API(fopen); API(freopen); API(fclose); API(fflush); API(fread); API(fwrite);
API(fgetc); API(getc); API(getchar); API(fputc); API(putc); API(putchar);
API(fgets); API(fputs); API(puts); API(ungetc); API(feof); API(ferror);
API(clearerr); API(setvbuf); API(setbuf); API(fseek); API(ftell); API(rewind);
API(fgetpos); API(fsetpos); API(remove); API(rename); API(perror); API(tmpfile);
API(tmpnam); API(vfprintf); API(fprintf); API(printf); API(vprintf);
API(vsnprintf); API(snprintf); API(vsprintf); API(sprintf); API(vfscanf);
API(fscanf); API(scanf); API(vscanf); API(vsscanf); API(sscanf);
API(malloc); API(aligned_alloc); API(calloc); API(realloc); API(free); API(abort);
API(atexit); API(at_quick_exit); API(exit); API(quick_exit); API(_Exit);
API(abs); API(labs); API(llabs); API(div); API(ldiv); API(lldiv); API(getenv);
API(system); API(rand); API(srand); API(strtol); API(strtoul); API(strtoll);
API(strtoull); API(strtof); API(strtod); API(strtold); API(atoi); API(atol);
API(atoll); API(atof); API(mblen); API(mbtowc); API(wctomb); API(mbstowcs);
API(wcstombs); API(bsearch); API(qsort);
API(strlen); API(strtok); API(strcmp); API(strncmp); API(strchr); API(strrchr);
API(strstr); API(strpbrk); API(strspn); API(strcspn); API(strcpy); API(strcat); API(strncpy);
API(strncat); API(strcoll); API(strxfrm); API(memset); API(memcpy); API(memmove);
API(memcmp); API(memchr); API(strerror);
API(clock); API(difftime); API(mktime); API(time); API(timespec_get);
API(asctime); API(ctime); API(gmtime); API(localtime); API(strftime);
API(asctime_r); API(ctime_r); API(gmtime_r); API(localtime_r);
API(mbrlen); API(mbrtowc); API(wcrtomb); API(mbsrtowcs); API(wcsrtombs);
API(btowc); API(wctob); API(mbsinit); API(wcscpy); API(wcsncpy); API(wcscat);
API(wcsncat); API(wcstok); API(wcscmp); API(wcsncmp); API(wcscoll); API(wcsxfrm); API(wcslen);
API(wcschr); API(wcsrchr); API(wcsstr); API(wcspbrk); API(wcscspn); API(wcsspn);
API(wmemchr); API(wmemcmp); API(wmemcpy); API(wmemmove); API(wmemset);

FILE * volatile *runtime_api_stdin = &stdin;
FILE * volatile *runtime_api_stdout = &stdout;
FILE * volatile *runtime_api_stderr = &stderr;

int main(void)
{
    return runtime_api_malloc == NULL || runtime_api_fopen == NULL;
}
