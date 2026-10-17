/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <complex.h>
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdio.h>

static unsigned records;
static unsigned long long random_state = 0x636f6d706c65782dULL;

static unsigned long long next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

static int random_integer(void)
{
    return (int)(next_random() % 2001U) - 1000;
}

#define RECORD_F(name, z, r) \
    do { printf("F %s %.6a %.6a %.6a %.6a\n", #name, \
        (double)crealf(z), (double)cimagf(z), \
        (double)crealf(r), (double)cimagf(r)); ++records; } while (0)
#define RECORD_D(name, z, r) \
    do { printf("D %s %.13a %.13a %.13a %.13a\n", #name, \
        creal(z), cimag(z), creal(r), cimag(r)); ++records; } while (0)
#define RECORD_L(name, z, r) \
    do { printf("L %s %.16La %.16La %.16La %.16La\n", #name, \
        creall(z), cimagl(z), creall(r), cimagl(r)); ++records; } while (0)
#define RECORD_POW_F(x, y, r) \
    do { printf("F cpow %.6a %.6a %.6a %.6a %.6a %.6a\n", \
        (double)crealf(x), (double)cimagf(x), \
        (double)crealf(y), (double)cimagf(y), \
        (double)crealf(r), (double)cimagf(r)); ++records; } while (0)
#define RECORD_POW_D(x, y, r) \
    do { printf("D cpow %.13a %.13a %.13a %.13a %.13a %.13a\n", \
        creal(x), cimag(x), creal(y), cimag(y), creal(r), cimag(r)); \
        ++records; } while (0)
#define RECORD_POW_L(x, y, r) \
    do { printf("L cpow %.16La %.16La %.16La %.16La %.16La %.16La\n", \
        creall(x), cimagl(x), creall(y), cimagl(y), creall(r), cimagl(r)); \
        ++records; } while (0)
#define RECORD_SCALAR_F(name, z, r) \
    do { printf("F %s %.6a %.6a %.6a\n", #name, (double)crealf(z), \
        (double)cimagf(z), (double)(r)); ++records; } while (0)
#define RECORD_SCALAR_D(name, z, r) \
    do { printf("D %s %.13a %.13a %.13a\n", #name, creal(z), cimag(z), (r)); \
        ++records; } while (0)
#define RECORD_SCALAR_L(name, z, r) \
    do { printf("L %s %.16La %.16La %.16La\n", #name, creall(z), cimagl(z), \
        (r)); ++records; } while (0)

static int edge_checks(void)
{
    double complex d = CMPLX(-0.0, 2.0);
    long double complex l = CMPLXL(-4.0L, -0.0L);
    double complex branch = clog(CMPLX(-1.0, -0.0));
    long double complex root = csqrtl(l);
    double complex projected = cproj(CMPLX(INFINITY, -3.0));
    double complex exp_negative_infinity_nan =
        cexp(CMPLX(-INFINITY, NAN));
    double complex exp_positive_infinity_nan =
        cexp(CMPLX(INFINITY, NAN));
    double complex sqrt_positive_infinity_nan =
        csqrt(CMPLX(INFINITY, NAN));
    double complex sqrt_negative_infinity_nan =
        csqrt(CMPLX(-INFINITY, NAN));
    if (creal(d) != 0.0 || !signbit(creal(d)) || cimag(d) != 2.0 ||
        creall(conjl(l)) != -4.0L || signbit(cimagl(conjl(l))) ||
        creal(branch) != 0.0 || cimag(branch) != -acos(-1.0) ||
        creall(root) != 0.0L || cimagl(root) != -2.0L ||
        !isinf(creal(projected)) || cimag(projected) != 0.0 ||
        !signbit(cimag(projected)) || carg(CMPLX(-0.0, -0.0)) != -acos(-1.0))
        return 1;
    if (creal(exp_negative_infinity_nan) != 0.0 ||
        cimag(exp_negative_infinity_nan) != 0.0 ||
        !isinf(creal(exp_positive_infinity_nan)) ||
        !isnan(cimag(exp_positive_infinity_nan)) ||
        !isinf(creal(sqrt_positive_infinity_nan)) ||
        !isnan(cimag(sqrt_positive_infinity_nan)) ||
        !isnan(creal(sqrt_negative_infinity_nan)) ||
        !isinf(cimag(sqrt_negative_infinity_nan)))
        return 4;
    if (!signbit(cimagf(CMPLXF(0.0f, -0.0f))) ||
        cabs(CMPLX(DBL_MAX, DBL_MAX)) != INFINITY ||
        !isfinite(cabs(CMPLX(0x1p-1000, 0x1p-1000))))
        return 2;
    if (feclearexcept(FE_ALL_EXCEPT) ||
        !isinf(cimag(csqrt(CMPLX(-4.0, INFINITY)))) ||
        feclearexcept(FE_ALL_EXCEPT) ||
        clog(CMPLX(0.0, -0.0)) == CMPLX(0.0, 0.0) ||
        !(fetestexcept(FE_DIVBYZERO) & FE_DIVBYZERO))
        return 3;
    return 0;
}

int main(void)
{
    int check = edge_checks();
    printf("CHECK %d\n", check);
    if (check)
        return check;

    for (unsigned index = 0; index < 512; ++index) {
        float xf = (float)random_integer() / 251.0f;
        float yf = (float)random_integer() / 251.0f;
        double xd = (double)random_integer() / 251.0;
        double yd = (double)random_integer() / 251.0;
        long double xl = (long double)random_integer() / 251.0L;
        long double yl = (long double)random_integer() / 251.0L;
        float complex zf = CMPLXF(xf, yf);
        double complex zd = CMPLX(xd, yd);
        long double complex zl = CMPLXL(xl, yl);

        RECORD_F(cexp, zf, cexpf(zf)); RECORD_D(cexp, zd, cexp(zd)); RECORD_L(cexp, zl, cexpl(zl));
        RECORD_F(clog, zf, clogf(zf)); RECORD_D(clog, zd, clog(zd)); RECORD_L(clog, zl, clogl(zl));
        RECORD_F(csqrt, zf, csqrtf(zf)); RECORD_D(csqrt, zd, csqrt(zd)); RECORD_L(csqrt, zl, csqrtl(zl));
        RECORD_F(csin, zf, csinf(zf)); RECORD_D(csin, zd, csin(zd)); RECORD_L(csin, zl, csinl(zl));
        RECORD_F(ccos, zf, ccosf(zf)); RECORD_D(ccos, zd, ccos(zd)); RECORD_L(ccos, zl, ccosl(zl));
        RECORD_F(ctan, zf, ctanf(zf)); RECORD_D(ctan, zd, ctan(zd)); RECORD_L(ctan, zl, ctanl(zl));
        RECORD_F(csinh, zf, csinhf(zf)); RECORD_D(csinh, zd, csinh(zd)); RECORD_L(csinh, zl, csinhl(zl));
        RECORD_F(ccosh, zf, ccoshf(zf)); RECORD_D(ccosh, zd, ccosh(zd)); RECORD_L(ccosh, zl, ccoshl(zl));
        RECORD_F(ctanh, zf, ctanhf(zf)); RECORD_D(ctanh, zd, ctanh(zd)); RECORD_L(ctanh, zl, ctanhl(zl));
        RECORD_F(casin, zf, casinf(zf)); RECORD_D(casin, zd, casin(zd)); RECORD_L(casin, zl, casinl(zl));
        RECORD_F(cacos, zf, cacosf(zf)); RECORD_D(cacos, zd, cacos(zd)); RECORD_L(cacos, zl, cacosl(zl));
        RECORD_F(catan, zf, catanf(zf)); RECORD_D(catan, zd, catan(zd)); RECORD_L(catan, zl, catanl(zl));
        RECORD_F(casinh, zf, casinhf(zf)); RECORD_D(casinh, zd, casinh(zd)); RECORD_L(casinh, zl, casinhl(zl));
        RECORD_F(cacosh, zf, cacoshf(zf)); RECORD_D(cacosh, zd, cacosh(zd)); RECORD_L(cacosh, zl, cacoshl(zl));
        RECORD_F(catanh, zf, catanhf(zf)); RECORD_D(catanh, zd, catanh(zd)); RECORD_L(catanh, zl, catanhl(zl));
        RECORD_SCALAR_F(cabs, zf, cabsf(zf)); RECORD_SCALAR_D(cabs, zd, cabs(zd)); RECORD_SCALAR_L(cabs, zl, cabsl(zl));
        RECORD_SCALAR_F(carg, zf, cargf(zf)); RECORD_SCALAR_D(carg, zd, carg(zd)); RECORD_SCALAR_L(carg, zl, cargl(zl));

        float complex pf = CMPLXF(1.25f + xf * 0.1f, yf * 0.1f);
        double complex pd = CMPLX(1.25 + xd * 0.1, yd * 0.1);
        long double complex pl = CMPLXL(1.25L + xl * 0.1L, yl * 0.1L);
        RECORD_POW_F(pf, CMPLXF(xf * 0.2f, yf * 0.2f),
                     cpowf(pf, CMPLXF(xf * 0.2f, yf * 0.2f)));
        RECORD_POW_D(pd, CMPLX(xd * 0.2, yd * 0.2),
                     cpow(pd, CMPLX(xd * 0.2, yd * 0.2)));
        RECORD_POW_L(pl, CMPLXL(xl * 0.2L, yl * 0.2L),
                     cpowl(pl, CMPLXL(xl * 0.2L, yl * 0.2L)));
    }

    /* Adversarial finite inputs: branch-point neighborhoods, subnormal
     * scales, large exact trigonometric arguments and near-pole inverses. */
    float complex branch_f = CMPLXF(nextafterf(1.0f, 0.0f), FLT_TRUE_MIN);
    double complex branch_d = CMPLX(nextafter(1.0, 0.0), DBL_TRUE_MIN);
    long double complex branch_l = CMPLXL(nextafterl(1.0L, 0.0L), LDBL_TRUE_MIN);
    RECORD_F(casin, branch_f, casinf(branch_f));
    RECORD_D(casin, branch_d, casin(branch_d));
    RECORD_L(casin, branch_l, casinl(branch_l));
    RECORD_F(cacosh, branch_f, cacoshf(branch_f));
    RECORD_D(cacosh, branch_d, cacosh(branch_d));
    RECORD_L(cacosh, branch_l, cacoshl(branch_l));
    RECORD_F(cacos, branch_f, cacosf(branch_f));
    RECORD_D(cacos, branch_d, cacos(branch_d));
    RECORD_L(cacos, branch_l, cacosl(branch_l));
    float complex branch_above_f = CMPLXF(nextafterf(1.0f, INFINITY), FLT_TRUE_MIN);
    double complex branch_above_d = CMPLX(nextafter(1.0, INFINITY), DBL_TRUE_MIN);
    long double complex branch_above_l = CMPLXL(nextafterl(1.0L, INFINITY), LDBL_TRUE_MIN);
    RECORD_F(cacos, branch_above_f, cacosf(branch_above_f));
    RECORD_D(cacos, branch_above_d, cacos(branch_above_d));
    RECORD_L(cacos, branch_above_l, cacosl(branch_above_l));

    float complex subnormal_f = CMPLXF(FLT_TRUE_MIN, FLT_TRUE_MIN);
    double complex subnormal_d = CMPLX(DBL_TRUE_MIN, DBL_TRUE_MIN);
    long double complex subnormal_l = CMPLXL(LDBL_TRUE_MIN, LDBL_TRUE_MIN);
    RECORD_F(csqrt, subnormal_f, csqrtf(subnormal_f));
    RECORD_D(csqrt, subnormal_d, csqrt(subnormal_d));
    RECORD_L(csqrt, subnormal_l, csqrtl(subnormal_l));
    RECORD_F(clog, subnormal_f, clogf(subnormal_f));
    RECORD_D(clog, subnormal_d, clog(subnormal_d));
    RECORD_L(clog, subnormal_l, clogl(subnormal_l));

    float complex large_f = CMPLXF(0x1p120f, 0.25f);
    double complex large_d = CMPLX(0x1p180, 0.25);
    long double complex large_l = CMPLXL(0x1p160L, 0.25L);
    RECORD_F(csin, large_f, csinf(large_f));
    RECORD_D(csin, large_d, csin(large_d));
    RECORD_L(csin, large_l, csinl(large_l));

    float complex pole_f = CMPLXF(nextafterf(1.0f, 0.0f), 0.0f);
    double complex pole_d = CMPLX(nextafter(1.0, 0.0), 0.0);
    long double complex pole_l = CMPLXL(nextafterl(1.0L, 0.0L), 0.0L);
    RECORD_F(catanh, pole_f, catanhf(pole_f));
    RECORD_D(catanh, pole_d, catanh(pole_d));
    RECORD_L(catanh, pole_l, catanhl(pole_l));

    /* Exact axes and branch-cut boundary witnesses. */
    float complex af = CMPLXF(-2.0f, -0.0f);
    double complex ad = CMPLX(-2.0, -0.0);
    long double complex al = CMPLXL(-2.0L, -0.0L);
    RECORD_F(csqrt, af, csqrtf(af)); RECORD_D(csqrt, ad, csqrt(ad)); RECORD_L(csqrt, al, csqrtl(al));
    RECORD_F(clog, af, clogf(af)); RECORD_D(clog, ad, clog(ad)); RECORD_L(clog, al, clogl(al));
    RECORD_F(cacos, af, cacosf(af)); RECORD_D(cacos, ad, cacos(ad)); RECORD_L(cacos, al, cacosl(al));
    RECORD_F(casin, af, casinf(af)); RECORD_D(casin, ad, casin(ad)); RECORD_L(casin, al, casinl(al));

    /* A scalar exponential can overflow although a complex component remains
     * representable after multiplication by a small trigonometric factor. */
    const long double half_pi =
        1.570796326794896619231321691639751442L;
    long double complex exp_scaled = CMPLXL(11357.0L, half_pi);
    long double complex hyperbolic_scaled = CMPLXL(11358.0L, half_pi);
    long double complex trig_scaled = CMPLXL(half_pi, 11358.0L);
    RECORD_L(cexp, exp_scaled, cexpl(exp_scaled));
    RECORD_L(csinh, hyperbolic_scaled, csinhl(hyperbolic_scaled));
    RECORD_L(ccosh, hyperbolic_scaled, ccoshl(hyperbolic_scaled));
    RECORD_L(csin, trig_scaled, csinl(trig_scaled));
    RECORD_L(ccos, trig_scaled, ccosl(trig_scaled));

    printf("COMPLETE records=%u seed=0x636f6d706c65782d\n", records);
    return 0;
}
