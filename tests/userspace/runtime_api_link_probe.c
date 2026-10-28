/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <ctype.h>
#include <complex.h>
#include <inttypes.h>
#include <fenv.h>
#include <math.h>
#include <mg/sync.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <tgmath.h>
#include <wchar.h>

/* Ensures the native executable link preserves the compiler's static TLS
 * relocation and emits a PT_TLS image for the kernel loader. */
static _Thread_local int runtime_tls_probe = 7;
static atomic_uint runtime_atomic_probe = ATOMIC_VAR_INIT(1U);
volatile int *runtime_tls_probe_address(void)
{
    return &runtime_tls_probe;
}

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
API(fabs); API(fabsf); API(fabsl); API(copysign); API(copysignf); API(copysignl);
API(fmin); API(fminf); API(fminl); API(fmax); API(fmaxf); API(fmaxl);
API(fdim); API(fdimf); API(fdiml);
API(floor); API(floorf); API(floorl); API(ceil); API(ceilf); API(ceill);
API(trunc); API(truncf); API(truncl); API(round); API(roundf); API(roundl);
API(rint); API(rintf); API(rintl); API(nearbyint); API(nearbyintf); API(nearbyintl);
API(lrint); API(lrintf); API(lrintl); API(llrint); API(llrintf); API(llrintl);
API(lround); API(lroundf); API(lroundl); API(llround); API(llroundf); API(llroundl);
API(modf); API(modff); API(modfl); API(frexp); API(frexpf); API(frexpl);
API(ilogb); API(ilogbf); API(ilogbl); API(logb); API(logbf); API(logbl);
API(nextafter); API(nextafterf); API(nextafterl); API(nexttoward); API(nexttowardf);
API(nexttowardl); API(sqrt); API(sqrtf); API(sqrtl);
API(cbrt); API(cbrtf); API(cbrtl); API(hypot); API(hypotf); API(hypotl);
API(fma); API(fmaf); API(fmal);
API(pow); API(powf); API(powl);
API(erf); API(erff); API(erfl); API(erfc); API(erfcf); API(erfcl);
API(tgamma); API(tgammaf); API(tgammal); API(lgamma); API(lgammaf); API(lgammal);
API(cacos); API(cacosf); API(cacosl); API(casin); API(casinf); API(casinl);
API(catan); API(catanf); API(catanl); API(ccos); API(ccosf); API(ccosl);
API(csin); API(csinf); API(csinl); API(ctan); API(ctanf); API(ctanl);
API(cacosh); API(cacoshf); API(cacoshl); API(casinh); API(casinhf); API(casinhl);
API(catanh); API(catanhf); API(catanhl); API(ccosh); API(ccoshf); API(ccoshl);
API(csinh); API(csinhf); API(csinhl); API(ctanh); API(ctanhf); API(ctanhl);
API(cexp); API(cexpf); API(cexpl); API(clog); API(clogf); API(clogl);
API(cabs); API(cabsf); API(cabsl); API(cpow); API(cpowf); API(cpowl);
API(csqrt); API(csqrtf); API(csqrtl); API(carg); API(cargf); API(cargl);
API(cimag); API(cimagf); API(cimagl); API(conj); API(conjf); API(conjl);
API(cproj); API(cprojf); API(cprojl); API(creal); API(crealf); API(creall);
API(nan); API(nanf); API(nanl);
API(exp); API(expf); API(expl); API(exp2); API(exp2f); API(exp2l);
API(expm1); API(expm1f); API(expm1l); API(log); API(logf); API(logl);
API(log2); API(log2f); API(log2l); API(log10); API(log10f); API(log10l);
API(log1p); API(log1pf); API(log1pl);
API(sinh); API(sinhf); API(sinhl); API(cosh); API(coshf); API(coshl);
API(tanh); API(tanhf); API(tanhl); API(asinh); API(asinhf); API(asinhl);
API(acosh); API(acoshf); API(acoshl); API(atanh); API(atanhf); API(atanhl);
API(atan); API(atanf); API(atanl); API(atan2); API(atan2f); API(atan2l);
API(asin); API(asinf); API(asinl); API(acos); API(acosf); API(acosl);
API(sin); API(sinf); API(sinl); API(cos); API(cosf); API(cosl);
API(tan); API(tanf); API(tanl);
API(ldexp); API(ldexpf); API(ldexpl); API(scalbn); API(scalbnf); API(scalbnl);
API(scalbln); API(scalblnf); API(scalblnl); API(fmod); API(fmodf); API(fmodl);
API(remainder); API(remainderf); API(remainderl); API(remquo); API(remquof); API(remquol);
API(feclearexcept); API(fegetexceptflag); API(feraiseexcept);
API(fesetexceptflag); API(fetestexcept); API(fegetround); API(fesetround);
API(fegetenv); API(feholdexcept); API(fesetenv); API(feupdateenv);
API(mg_sync_wait_u32); API(mg_sync_wake_u32);
API(mg_mutex_init); API(mg_mutex_destroy); API(mg_mutex_try_lock);
API(mg_mutex_lock); API(mg_mutex_unlock);
API(mg_condition_init); API(mg_condition_destroy); API(mg_condition_wait);
API(mg_condition_timed_wait); API(mg_condition_signal);
API(mg_condition_broadcast);

FILE * volatile *runtime_api_stdin = &stdin;
FILE * volatile *runtime_api_stdout = &stdout;
FILE * volatile *runtime_api_stderr = &stderr;

int main(void)
{
    unsigned expected = 1U;
    if (!atomic_compare_exchange_strong_explicit(
            &runtime_atomic_probe, &expected, 2U, memory_order_acq_rel,
            memory_order_acquire))
        return 1;
    return runtime_api_malloc == NULL || runtime_api_fopen == NULL;
}
