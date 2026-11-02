/* math.h — colang 自制最小标准头 (P9.1)
 * 常用数学函数原型（double / float / long double 三档），符号由宿主 CRT(UCRT/MSVC) 提供。
 * 省略类型通用宏（_Generic 形式 fabs(X)→fabsf 等）与 HUGE_VAL*（需 +infinity 常量/内建，本批不做）。
 * long double 档（*l）已补全，见文末专节；本机 long double == double，与宿主符号 ABI 等价。
 */
#ifndef _COLANG_MATH_H
#define _COLANG_MATH_H

/* 错误处理指示（C 标准）：值仅为声明性，不影响本批函数行为 */
#define MATH_ERRNO     1
#define MATH_ERREXCEPT 2
#define math_errhandling 3 /* MATH_ERRNO | MATH_ERREXCEPT */

/* 三角函数 */
extern double sin(double x);
extern double cos(double x);
extern double tan(double x);
extern double asin(double x);
extern double acos(double x);
extern double atan(double x);
extern double atan2(double y, double x);
extern float sinf(float x);
extern float cosf(float x);
extern float tanf(float x);
extern float atan2f(float y, float x);

/* 双曲函数 */
extern double sinh(double x);
extern double cosh(double x);
extern double tanh(double x);

/* 指数/对数/幂 */
extern double exp(double x);
extern double exp2(double x);
extern double log(double x);
extern double log2(double x);
extern double log10(double x);
extern double pow(double x, double y);
extern double sqrt(double x);
extern double cbrt(double x);
extern float sqrtf(float x);
extern float expf(float x);
extern float logf(float x);
extern float powf(float x, float y);

/* 取整/绝对/余数 */
extern double fabs(double x);
extern double ceil(double x);
extern double floor(double x);
extern double round(double x);
extern double trunc(double x);
extern double fmod(double x, double y);
extern float fabsf(float x);
extern float ceilf(float x);
extern float floorf(float x);
extern float roundf(float x);
extern float truncf(float x);
extern float fmodf(float x, float y);

/* 分解/组装（C 标准，CTS P184 G 族：缺声明致隐式 int 返回、modf  fractional 打 0） */
extern double modf(double x, double *iptr);
extern double frexp(double x, int *exp);
extern double ldexp(double x, int exp);
extern float modff(float x, float *iptr);
extern float frexpf(float x, int *exp);
extern float ldexpf(float x, int exp);

/* 反双曲函数（C 标准，CTS P187：缺声明致隐式 int 返回、结果污染） */
extern double asinh(double x);
extern double acosh(double x);
extern double atanh(double x);
extern float asinhf(float x);
extern float acoshf(float x);
extern float atanhf(float x);

/* 反三角 float 档 + 双曲 float 档（补齐 C 标准） */
extern float asinf(float x);
extern float acosf(float x);
extern float atanf(float x);
extern float sinhf(float x);
extern float coshf(float x);
extern float tanhf(float x);

/* 指数/对数 补充档（C 标准：expm1/log1p/exp2f/log2f/log10f 等） */
extern double expm1(double x);
extern double log1p(double x);
extern float exp2f(float x);
extern float log2f(float x);
extern float log10f(float x);
extern float expm1f(float x);
extern float log1pf(float x);
extern float cbrtf(float x);

/* 特殊函数（C 标准：hypot/remainder/fmin/fmax/fdim 及 f 档） */
extern double hypot(double x, double y);
extern double remainder(double x, double y);
extern double fmin(double x, double y);
extern double fmax(double x, double y);
extern double fdim(double x, double y);
extern float hypotf(float x, float y);
extern float remainderf(float x, float y);
extern float fminf(float x, float y);
extern float fmaxf(float x, float y);
extern float fdimf(float x, float y);

/* 取整族补全（C 标准：rint/nearbyint/lrint/llrint/lround/llround 及 f 档） */
extern double rint(double x);
extern double nearbyint(double x);
extern long lrint(double x);
extern long long llrint(double x);
extern long lround(double x);
extern long long llround(double x);
extern float rintf(float x);
extern float nearbyintf(float x);
extern long lrintf(float x);
extern long long llrintf(float x);

/* 分解/组装补全（C 标准：scalbn/scalbln/ilogb/logb/nextafter/nexttoward）
 * 注：本机 long double == double（MSVC 数据模型），nexttoward 第二参按 double 声明以贴合宿主符号。 */
extern double scalbn(double x, int n);
extern double scalbln(double x, long n);
extern int ilogb(double x);
extern double logb(double x);
extern double nextafter(double x, double y);
extern double nexttoward(double x, double y);

/* Gamma 函数（C 标准） */
extern double lgamma(double x);
extern double tgamma(double x);
extern float lgammaf(float x);
extern float tgammaf(float x);

/* 误差函数（C 标准附：erf/erfc 及 f 档，CTS P187 二次补录） */
extern double erf(double x);
extern double erfc(double x);
extern float erff(float x);
extern float erfcf(float x);

/* 其余 float 档与漏声明补全（CTS P187：logbf/ilogbf/nexttowardf/scalbnf/scalblnf/remquo/fma/lroundf/llroundf） */
extern float logbf(float x);
extern int ilogbf(float x);
extern float nexttowardf(float x, double y);
extern float scalbnf(float x, int n);
extern float scalblnf(float x, long n);
extern double remquo(double x, double y, int *quo);
extern float remquof(float x, float y, int *quo);
extern double fma(double x, double y, double z);
extern float fmaf(float x, float y, float z);
extern long lroundf(float x);
extern long long llroundf(float x);

/* long double 档（C 标准全族 56 名）
 * 修 CTS 0112_0112（差异恰 3 行，全在 long_asinhl/long_acoshl/long_atanh 段，余 17 行逐字正确）
 * 与 0055_0003/0055_0004/0055_0005（sqrtl 被当 int 返回、打出 NG_<每次不同的垃圾整数>）。
 * 此前本头零枚 l 后缀声明，隐式声明策略按 int 返回，调用点读 EAX 而非 XMM0，静默错值且无任何诊断。
 * 原型逐条取自宿主 corecrt_math.h（__inline ... __CRTDECL 与 _ACRTIMP ... __cdecl 两族），不凭标准记忆；
 * 宿主与 C 标准在个别签名上有差异（如 nexttoward 第二参宿主取 double），本处以贴合宿主符号为准。 */

/* 三角（C 标准） */
extern long double sinl(long double x);
extern long double cosl(long double x);
extern long double tanl(long double x);
extern long double asinl(long double x);
extern long double acosl(long double x);
extern long double atanl(long double x);
extern long double atan2l(long double y, long double x);

/* 双曲与反双曲（C 标准） */
extern long double sinhl(long double x);
extern long double coshl(long double x);
extern long double tanhl(long double x);
extern long double asinhl(long double x);
extern long double acoshl(long double x);
extern long double atanhl(long double x);

/* 指数/对数/幂（C 标准） */
extern long double expl(long double x);
extern long double exp2l(long double x);
extern long double expm1l(long double x);
extern long double logl(long double x);
extern long double log2l(long double x);
extern long double log10l(long double x);
extern long double log1pl(long double x);
extern long double logbl(long double x);
extern long double powl(long double x, long double y);
extern long double sqrtl(long double x);
extern long double cbrtl(long double x);

/* 取整/绝对（C 标准） */
extern long double fabsl(long double x);
extern long double ceill(long double x);
extern long double floorl(long double x);
extern long double roundl(long double x);
extern long double truncl(long double x);
extern long double rintl(long double x);
extern long double nearbyintl(long double x);
extern long lrintl(long double x);
extern long long llrintl(long double x);
extern long lroundl(long double x);
extern long long llroundl(long double x);

/* 分解/组装（C 标准） */
extern long double modfl(long double x, long double *iptr);
extern long double ldexpl(long double x, int exp);
extern long double scalbnl(long double x, int n);
extern long double scalblnl(long double x, long n);
extern int ilogbl(long double x);

/* 特殊函数（C 标准：运算/最值/差值/符号/邻值/NAN） */
extern long double fmodl(long double x, long double y);
extern long double remainderl(long double x, long double y);
extern long double remquol(long double x, long double y, int *quo);
extern long double fdiml(long double x, long double y);
extern long double fminl(long double x, long double y);
extern long double fmaxl(long double x, long double y);
extern long double hypotl(long double x, long double y);
extern long double fmal(long double x, long double y, long double z);
extern long double copysignl(long double number, long double sign);
extern long double nextafterl(long double x, long double y);
extern long double nexttowardl(long double x, long double y);
extern long double nanl(const char *tag);

/* Gamma 与误差函数（C 标准附） */
extern long double lgammal(long double x);
extern long double tgammal(long double x);
extern long double erfl(long double x);
extern long double erfcl(long double x);

#endif /* _COLANG_MATH_H */
