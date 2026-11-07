/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's blob: the math libraries (docs/d29-zine.md 16.12): mathffp and mathtrans (Motorola's fast floating point
 * format), mathieeesingbas/-singtrans and mathieeedoubbas/-doubtrans (IEEE single and double), with the functions,
 * registers and argument order of the RKRM Includes and Autodocs and the NDK 3.2's .fd files. Computed in software
 * (gcc's soft float, libgcc): the same on every CPU, an FPU or none; the transcendental functions are ZINE's own
 * (range reduction and series, accurate to about 1e-15 relative in double). Never from AmigaOS code.
 * FFP (the autodocs' "Motorola fast floating point"): bits 31-8 the mantissa with its top bit set (0.5 <= m < 1),
 * bit 7 the sign, bits 6-0 the exponent + 64; 0 is all zero. Two-argument FFP calls take d1 and d0 and answer as
 * the autodocs say: SPCmp "GT: fnum2 > fnum1" (fnum2 the d0 one), SPPow "fnum2 raised to the fnum1 power"; so
 * SPSub and SPDiv are d0 - d1 and d0 / d1 (the same order; *read*, the autodocs' wording, not measured on AmigaOS). */
#include "blob.h"

typedef unsigned long long u64;
union f32 { float f; u32 u; };
union f64 { double d; u64 u; u32 w[2]; };

u32 zm_cc;                              /* the condition codes the stubs (blobs.S) put in the CCR: N 8, Z 4 */

static double dbl(u32 hi, u32 lo) { union f64 x; x.w[0] = hi; x.w[1] = lo; return x.d; }
static u64 ret(double d)
{
    union f64 x;
    x.d = d;
    zm_cc = (x.w[0] >> 31 ? 8 : 0) | (!(x.u << 1) ? 4 : 0);
    return x.u;
}
static float flt(u32 u) { union f32 x; x.u = u; return x.f; }
static u32 retf(float f)
{
    union f32 x;
    x.f = f;
    zm_cc = (x.u >> 31 ? 8 : 0) | (!(x.u << 1) ? 4 : 0);
    return x.u;
}

/* FFP <-> IEEE single, exactly (FFP's 24-bit mantissa is single's); overflow gives the largest FFP, underflow 0 */
static float ffp2f(u32 v)
{
    if (!(v & 0xFFFFFF00u))
        return 0.0f;
    union f32 x;
    x.u = (v & 0x80) << 24 | (((v & 0x7F) + 62) << 23) | ((v >> 8) & 0x7FFFFF);
    return x.f;
}
static u32 f2ffp(float f)
{
    union f32 x;
    x.f = f;
    u32 e = x.u >> 23 & 0xFF, s = x.u >> 31 ? 0x80 : 0, r;
    if (e == 0)
        r = 0;
    else if (e == 255 || e > 127 + 62)
        r = 0xFFFFFF7Fu | s;
    else if (e < 62)
        r = 0;
    else
        r = ((x.u & 0x7FFFFF) | 0x800000) << 8 | s | (e - 62);
    zm_cc = (r && s ? 8 : 0) | (!r ? 4 : 0);
    return r;
}

/* ---- the transcendental functions in double ---- */
#define PI 3.14159265358979323846
#define LN2 0.69314718055994530942
static double fabs_(double x) { return x < 0 ? -x : x; }
static int isnan_(double x) { return x != x; }

static double floor_(double x)
{
    union f64 v;
    v.d = x;
    int e = (int)(v.w[0] >> 20 & 0x7FF) - 1023;
    if (e >= 52)
        return x;                       /* already whole (or inf/nan) */
    if (e < 0)
        return x < 0 ? -1.0 : (x == 0 ? x : 0.0);
    u64 m = (1ull << (52 - e)) - 1;
    if (!(v.u & m))
        return x;
    if (x < 0)
        v.u += m;
    v.u &= ~m;
    return v.d;
}
static double ceil_(double x) { return -floor_(-x); }

/* x * 2^k by its exponent field */
static double ldexp_(double x, int k)
{
    while (k > 1000) { x *= 8.98846567431158e307; k -= 1023; }      /* 2^1023 */
    while (k < -1000) { x *= 2.2250738585072014e-308; k += 1022; }  /* 2^-1022 */
    union f64 v;
    v.u = (u64)(k + 1023) << 52;
    return x * v.d;
}

static double sqrt_(double x)
{
    if (x <= 0 || isnan_(x))
        return x == 0 ? x : (x < 0 ? (0.0 / 0.0) : x);
    union f64 v;
    v.d = x;
    int e = (int)(v.w[0] >> 20 & 0x7FF) - 1023;
    double g = ldexp_(1.0, e / 2);      /* within a factor of 2 */
    for (int i = 0; i < 8; i++)
        g = 0.5 * (g + x / g);
    return g;
}

static double exp_(double x)
{
    if (isnan_(x))
        return x;
    if (x > 709.7)
        return 1e308 * 10;
    if (x < -745)
        return 0;
    double k = floor_(x / LN2 + 0.5);
    double r = x - k * 0.693147180369123816490 - k * 1.90821492927058770002e-10;
    double t = 1, s = 1;
    for (int i = 1; i < 18; i++) {
        t *= r / i;
        s += t;
    }
    return ldexp_(s, (int)k);
}

static double log_(double x)
{
    if (x <= 0 || isnan_(x))
        return x == 0 ? -1e308 * 10 : (0.0 / 0.0);
    union f64 v;
    v.d = x;
    int e = (int)(v.w[0] >> 20 & 0x7FF) - 1023;
    if (e == -1023) {                   /* a denormal: scaled up first */
        v.d = x * 18014398509481984.0;  /* 2^54 */
        e = (int)(v.w[0] >> 20 & 0x7FF) - 1023 - 54;
    }
    v.w[0] = (v.w[0] & 0x800FFFFFu) | 0x3FF00000u;     /* 1 <= m < 2 */
    double m = v.d;
    if (m > 1.41421356237309504880)
        m *= 0.5, e++;
    double s = (m - 1) / (m + 1), s2 = s * s, t = s, sum = 0;
    for (int i = 1; i < 40; i += 2) {
        sum += t / i;
        t *= s2;
    }
    return 2 * sum + e * LN2;
}

/* sin and cos of r, |r| <= pi/4 */
static double ksin(double r)
{
    double r2 = r * r, t = r, s = r;
    for (int i = 2; i < 24; i += 2) {
        t *= -r2 / (i * (i + 1));
        s += t;
    }
    return s;
}
static double kcos(double r)
{
    double r2 = r * r, t = 1, s = 1;
    for (int i = 1; i < 24; i += 2) {
        t *= -r2 / (i * (i + 1));
        s += t;
    }
    return s;
}
static void sincos_(double x, double *sn, double *cs)
{
    if (isnan_(x) || fabs_(x) > 1e15) {
        *sn = *cs = 0.0 / 0.0;
        return;
    }
    double k = floor_(x / (PI / 2) + 0.5);
    double r = x - k * 1.57079632673412561417 - k * 6.07710050650619224932e-11;
    long q = (long)(k - 4 * floor_(k / 4));
    double s = ksin(r), c = kcos(r);
    switch (q) {
    case 0: *sn = s; *cs = c; break;
    case 1: *sn = c; *cs = -s; break;
    case 2: *sn = -s; *cs = -c; break;
    default: *sn = -c; *cs = s; break;
    }
}
static double sin_(double x) { double s, c; sincos_(x, &s, &c); return s; }
static double cos_(double x) { double s, c; sincos_(x, &s, &c); return c; }
static double tan_(double x) { double s, c; sincos_(x, &s, &c); return s / c; }

static double atan_(double x)
{
    if (isnan_(x))
        return x;
    int neg = x < 0;
    if (neg)
        x = -x;
    double off = 0;
    int inv = x > 1;
    if (inv)
        x = 1 / x;
    if (x > 0.26794919243112270647) {   /* tan(pi/12): atan x = pi/6 + atan((x sqrt3 - 1) / (sqrt3 + x)) */
        off = PI / 6;
        x = (x * 1.73205080756887729353 - 1) / (1.73205080756887729353 + x);
    }
    double x2 = x * x, t = x, s = 0;
    for (int i = 1; i < 60; i += 2) {
        s += t / i;
        t *= -x2;
    }
    s += off;
    if (inv)
        s = PI / 2 - s;
    return neg ? -s : s;
}
static double asin_(double x)
{
    if (x > 1 || x < -1)
        return 0.0 / 0.0;
    if (x == 1 || x == -1)
        return x * (PI / 2);
    return atan_(x / sqrt_(1 - x * x));
}
static double acos_(double x) { return PI / 2 - asin_(x); }
static double sinh_(double x) { double e = exp_(x); return 0.5 * (e - 1 / e); }
static double cosh_(double x) { double e = exp_(x); return 0.5 * (e + 1 / e); }
static double tanh_(double x)
{
    if (x > 20)
        return 1;
    if (x < -20)
        return -1;
    double e = exp_(2 * x);
    return (e - 1) / (e + 1);
}
/* y to the x */
static double pow_(double y, double x)
{
    if (x == 0)
        return 1;
    if (y == 0)
        return x > 0 ? 0 : 1e308 * 10;
    if (y < 0) {
        if (floor_(x) != x)
            return 0.0 / 0.0;
        double r = exp_(x * log_(-y));
        return fabs_(x - 2 * floor_(x / 2)) == 1 ? -r : r;
    }
    return exp_(x * log_(y));
}
static double log10_(double x) { return log_(x) * 0.43429448190325182765; }

static s32 fix(double d)                /* toward zero, saturated */
{
    if (isnan_(d))
        return 0;
    if (d >= 2147483647.0)
        return 0x7FFFFFFF;
    if (d <= -2147483648.0)
        return (s32)0x80000000u;
    return (s32)d;
}
static s32 cmp(double a, double b) { return a < b ? -1 : a > b ? 1 : 0; }

/* ---- mathffp (fd: SPFix(d0) SPFlt(d0) SPCmp(d1,d0) SPTst(d1) SPAbs SPNeg SPAdd(d1,d0) SPSub SPMul SPDiv SPFloor
 * SPCeil) and mathtrans (SPAtan ... SPLog10; SPSincos(cosResult d1, parm d0), SPPow(power d1, arg d0)) ---- */
s32 c_SPFix(u32 a) { return fix(ffp2f(a)); }
u32 c_SPFlt(s32 i) { return f2ffp((float)i); }
s32 c_SPCmp(u32 a1, u32 a0) { return cmp(ffp2f(a0), ffp2f(a1)); }    /* GT: d0 > d1 */
s32 c_SPTst(u32 a) { return cmp(ffp2f(a), 0); }
u32 c_SPAbs(u32 a) { return f2ffp(ffp2f(a) < 0 ? -ffp2f(a) : ffp2f(a)); }
u32 c_SPNeg(u32 a) { return f2ffp(-ffp2f(a)); }
u32 c_SPAdd(u32 a1, u32 a0) { return f2ffp(ffp2f(a0) + ffp2f(a1)); }
u32 c_SPSub(u32 a1, u32 a0) { return f2ffp(ffp2f(a0) - ffp2f(a1)); }
u32 c_SPMul(u32 a1, u32 a0) { return f2ffp(ffp2f(a0) * ffp2f(a1)); }
u32 c_SPDiv(u32 a1, u32 a0) { return f2ffp(ffp2f(a0) / ffp2f(a1)); }
u32 c_SPFloor(u32 a) { return f2ffp((float)floor_(ffp2f(a))); }
u32 c_SPCeil(u32 a) { return f2ffp((float)ceil_(ffp2f(a))); }
#define FFP1(n, f) u32 c_SP##n(u32 a) { return f2ffp((float)f((double)ffp2f(a))); }
FFP1(Atan, atan_) FFP1(Sin, sin_) FFP1(Cos, cos_) FFP1(Tan, tan_) FFP1(Sinh, sinh_) FFP1(Cosh, cosh_)
FFP1(Tanh, tanh_) FFP1(Exp, exp_) FFP1(Log, log_) FFP1(Sqrt, sqrt_) FFP1(Asin, asin_) FFP1(Acos, acos_)
FFP1(Log10, log10_)
u32 c_SPSincos(u32 *cosp, u32 a)
{
    double s, c;
    sincos_(ffp2f(a), &s, &c);
    if (cosp && c_TypeOfMem((u32)cosp))
        *cosp = f2ffp((float)c);
    return f2ffp((float)s);
}
u32 c_SPPow(u32 p, u32 a) { return f2ffp((float)pow_(ffp2f(a), ffp2f(p))); }
u32 c_SPTieee(u32 a) { return retf(ffp2f(a)); }
u32 c_SPFieee(u32 a) { return f2ffp(flt(a)); }

/* ---- mathieeesingbas / mathieeesingtrans (all in d0, d1; IEEESPSincos(cosptr a0, parm d0); IEEESPPow(exp d1,
 * arg d0) = arg to the exp) ---- */
s32 c_SFix(u32 a) { return fix(flt(a)); }
u32 c_SFlt(s32 i) { return retf((float)i); }
s32 c_SCmp(u32 a, u32 b) { return cmp(flt(a), flt(b)); }
s32 c_STst(u32 a) { return cmp(flt(a), 0); }
u32 c_SAbs(u32 a) { return retf(flt(a & 0x7FFFFFFFu)); }
u32 c_SNeg(u32 a) { return retf(flt(a ^ 0x80000000u)); }
u32 c_SAdd(u32 a, u32 b) { return retf(flt(a) + flt(b)); }
u32 c_SSub(u32 a, u32 b) { return retf(flt(a) - flt(b)); }
u32 c_SMul(u32 a, u32 b) { return retf(flt(a) * flt(b)); }
u32 c_SDiv(u32 a, u32 b) { return retf(flt(a) / flt(b)); }
u32 c_SFloor(u32 a) { return retf((float)floor_(flt(a))); }
u32 c_SCeil(u32 a) { return retf((float)ceil_(flt(a))); }
#define S1(n, f) u32 c_S##n(u32 a) { return retf((float)f((double)flt(a))); }
S1(Atan, atan_) S1(Sin, sin_) S1(Cos, cos_) S1(Tan, tan_) S1(Sinh, sinh_) S1(Cosh, cosh_) S1(Tanh, tanh_)
S1(Exp, exp_) S1(Log, log_) S1(Sqrt, sqrt_) S1(Asin, asin_) S1(Acos, acos_) S1(Log10, log10_)
u32 c_SSincos(u32 *cosp, u32 a)
{
    double s, c;
    sincos_(flt(a), &s, &c);
    if (cosp && c_TypeOfMem((u32)cosp))
        *cosp = retf((float)c);
    return retf((float)s);
}
u32 c_SPow(u32 e, u32 a) { return retf((float)pow_(flt(a), flt(e))); }
u32 c_STieee(u32 a) { return retf(flt(a)); }       /* to and from single: the same */
u32 c_SFieee(u32 a) { return retf(flt(a)); }

/* ---- mathieeedoubbas / mathieeedoubtrans (y in d0/d1, z in d2/d3; IEEEDPSincos(pf2 a0, parm d0/d1); IEEEDPPow(exp
 * d2/d3, arg d0/d1) = arg to the exp; IEEEDPFieee(single d0)) ---- */
s32 c_DFix(u32 h, u32 l) { return fix(dbl(h, l)); }
u64 c_DFlt(s32 i) { return ret((double)i); }
s32 c_DCmp(u32 h, u32 l, u32 h2, u32 l2) { return cmp(dbl(h, l), dbl(h2, l2)); }
s32 c_DTst(u32 h, u32 l) { return cmp(dbl(h, l), 0); }
u64 c_DAbs(u32 h, u32 l) { return ret(dbl(h & 0x7FFFFFFFu, l)); }
u64 c_DNeg(u32 h, u32 l) { return ret(dbl(h ^ 0x80000000u, l)); }
u64 c_DAdd(u32 h, u32 l, u32 h2, u32 l2) { return ret(dbl(h, l) + dbl(h2, l2)); }
u64 c_DSub(u32 h, u32 l, u32 h2, u32 l2) { return ret(dbl(h, l) - dbl(h2, l2)); }
u64 c_DMul(u32 h, u32 l, u32 h2, u32 l2) { return ret(dbl(h, l) * dbl(h2, l2)); }
u64 c_DDiv(u32 h, u32 l, u32 h2, u32 l2) { return ret(dbl(h, l) / dbl(h2, l2)); }
u64 c_DFloor(u32 h, u32 l) { return ret(floor_(dbl(h, l))); }
u64 c_DCeil(u32 h, u32 l) { return ret(ceil_(dbl(h, l))); }
#define D1(n, f) u64 c_D##n(u32 h, u32 l) { return ret(f(dbl(h, l))); }
D1(Atan, atan_) D1(Sin, sin_) D1(Cos, cos_) D1(Tan, tan_) D1(Sinh, sinh_) D1(Cosh, cosh_) D1(Tanh, tanh_)
D1(Exp, exp_) D1(Log, log_) D1(Sqrt, sqrt_) D1(Asin, asin_) D1(Acos, acos_) D1(Log10, log10_)
u64 c_DSincos(u32 *cosp, u32 h, u32 l)
{
    double s, c;
    sincos_(dbl(h, l), &s, &c);
    if (cosp && c_TypeOfMem((u32)cosp)) {
        union f64 v;
        v.d = c;
        cosp[0] = v.w[0], cosp[1] = v.w[1];
    }
    return ret(s);
}
u64 c_DPow(u32 eh, u32 el, u32 h, u32 l) { return ret(pow_(dbl(h, l), dbl(eh, el))); }
u32 c_DTieee(u32 h, u32 l) { return retf((float)dbl(h, l)); }
u64 c_DFieee(u32 a) { return ret((double)flt(a)); }

/* ---- the tables (offsets from the .fd files: the basic libraries 30-96, the transcendental 30-126) ---- */
#define F(x) extern void zs_##x(void)
F(SPFix); F(SPFlt); F(SPCmp); F(SPTst); F(SPAbs); F(SPNeg); F(SPAdd); F(SPSub); F(SPMul); F(SPDiv); F(SPFloor);
F(SPCeil); F(SPAtan); F(SPSin); F(SPCos); F(SPTan); F(SPSincos); F(SPSinh); F(SPCosh); F(SPTanh); F(SPExp);
F(SPLog); F(SPPow); F(SPSqrt); F(SPTieee); F(SPFieee); F(SPAsin); F(SPAcos); F(SPLog10);
F(SFix); F(SFlt); F(SCmp); F(STst); F(SAbs); F(SNeg); F(SAdd); F(SSub); F(SMul); F(SDiv); F(SFloor); F(SCeil);
F(SAtan); F(SSin); F(SCos); F(STan); F(SSincos); F(SSinh); F(SCosh); F(STanh); F(SExp); F(SLog); F(SPow); F(SSqrt);
F(STieee); F(SFieee); F(SAsin); F(SAcos); F(SLog10);
F(DFix); F(DFlt); F(DCmp); F(DTst); F(DAbs); F(DNeg); F(DAdd); F(DSub); F(DMul); F(DDiv); F(DFloor); F(DCeil);
F(DAtan); F(DSin); F(DCos); F(DTan); F(DSincos); F(DSinh); F(DCosh); F(DTanh); F(DExp); F(DLog); F(DPow); F(DSqrt);
F(DTieee); F(DFieee); F(DAsin); F(DAcos); F(DLog10);
static const u16 bas_lvo[] = { 30, 36, 42, 48, 54, 60, 66, 72, 78, 84, 90, 96 };
static const u16 tr_lvo[] = { 30, 36, 42, 48, 54, 60, 66, 72, 78, 84, 90, 96, 102, 108, 114, 120, 126 };
static void (*const ffp_fn[])(void) = { zs_SPFix, zs_SPFlt, zs_SPCmp, zs_SPTst, zs_SPAbs, zs_SPNeg, zs_SPAdd,
    zs_SPSub, zs_SPMul, zs_SPDiv, zs_SPFloor, zs_SPCeil };
static void (*const ffpt_fn[])(void) = { zs_SPAtan, zs_SPSin, zs_SPCos, zs_SPTan, zs_SPSincos, zs_SPSinh, zs_SPCosh,
    zs_SPTanh, zs_SPExp, zs_SPLog, zs_SPPow, zs_SPSqrt, zs_SPTieee, zs_SPFieee, zs_SPAsin, zs_SPAcos, zs_SPLog10 };
static void (*const sb_fn[])(void) = { zs_SFix, zs_SFlt, zs_SCmp, zs_STst, zs_SAbs, zs_SNeg, zs_SAdd, zs_SSub,
    zs_SMul, zs_SDiv, zs_SFloor, zs_SCeil };
static void (*const st_fn[])(void) = { zs_SAtan, zs_SSin, zs_SCos, zs_STan, zs_SSincos, zs_SSinh, zs_SCosh, zs_STanh,
    zs_SExp, zs_SLog, zs_SPow, zs_SSqrt, zs_STieee, zs_SFieee, zs_SAsin, zs_SAcos, zs_SLog10 };
static void (*const db_fn[])(void) = { zs_DFix, zs_DFlt, zs_DCmp, zs_DTst, zs_DAbs, zs_DNeg, zs_DAdd, zs_DSub,
    zs_DMul, zs_DDiv, zs_DFloor, zs_DCeil };
static void (*const dt_fn[])(void) = { zs_DAtan, zs_DSin, zs_DCos, zs_DTan, zs_DSincos, zs_DSinh, zs_DCosh, zs_DTanh,
    zs_DExp, zs_DLog, zs_DPow, zs_DSqrt, zs_DTieee, zs_DFieee, zs_DAsin, zs_DAcos, zs_DLog10 };

struct zb_mlib { const char *name; u16 slots; const u16 *lvo; void (*const *fn)(void); u16 n; };
const struct zb_mlib zb_mlibs[6] = {
    { "mathffp.library", 16, bas_lvo, ffp_fn, 12 },
    { "mathtrans.library", 21, tr_lvo, ffpt_fn, 17 },
    { "mathieeedoubbas.library", 16, bas_lvo, db_fn, 12 },
    { "mathieeedoubtrans.library", 21, tr_lvo, dt_fn, 17 },
    { "mathieeesingbas.library", 16, bas_lvo, sb_fn, 12 },
    { "mathieeesingtrans.library", 21, tr_lvo, st_fn, 17 },
};
