/* float.h — colang 自制最小标准头 (P9.1)
 * 浮点特性宏。目标模型：Windows x86_64 / MSVC 语义 —— float=IEEE754 binary32,
 * double=binary64, long double==double（MSVC 不提供 80 位扩展）。
 * 整型特性宏精确；极值浮点宏给出十进制近似（不使用 F/L 后缀以规避词法差异）。
 */
#ifndef _COLANG_FLOAT_H
#define _COLANG_FLOAT_H

#define FLT_RADIX       2
#define FLT_ROUNDS      1
#define FLT_EVAL_METHOD 0

#define DECIMAL_DIG     17

#define FLT_MANT_DIG    24
#define DBL_MANT_DIG    52
#define LDBL_MANT_DIG   52

#define FLT_DIG         6
#define DBL_DIG         15
#define LDBL_DIG        15

#define FLT_MIN_EXP     (-125)
#define DBL_MIN_EXP     (-1021)
#define LDBL_MIN_EXP    (-1021)
#define FLT_MIN_10_EXP  (-37)
#define DBL_MIN_10_EXP  (-307)
#define LDBL_MIN_10_EXP (-307)
#define FLT_MAX_EXP     128
#define DBL_MAX_EXP     1024
#define LDBL_MAX_EXP    1024
#define FLT_MAX_10_EXP  38
#define DBL_MAX_10_EXP  308
#define LDBL_MAX_10_EXP 308

#define FLT_EPSILON     1.1920928955078125e-7
#define DBL_EPSILON     2.2204460492503131e-16
#define LDBL_EPSILON    2.2204460492503131e-16

#define FLT_MIN         1.1754943508222875e-38
#define FLT_MAX         3.4028234663852886e38
#define DBL_MIN         2.2250738585072014e-308
#define DBL_MAX         1.7976931348623157e308
#define LDBL_MIN        2.2250738585072014e-308
#define LDBL_MAX        1.7976931348623157e308

#endif /* _COLANG_FLOAT_H */
