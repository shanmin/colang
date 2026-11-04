/* float.h — colang 自制最小标准头 (P9.1)
 * 浮点特性宏。目标模型：Windows x86_64 / MSVC 语义 —— float=IEEE754 binary32,
 * double=binary64, long double==double（MSVC 不提供 80 位扩展）。
 * 整型特性宏精确；极值浮点宏给出十进制近似。
 * P373 订正：本头原注「不使用 F/L 后缀以规避词法差异」的前提经实测不成立——
 * colang 的数值后缀状态机（src/parser/parser.cpp scan_numeric_suffix）接受浮点上的
 * f/F 与 l/L，且 denorm 字面量折叠与本机 clang 23 逐值一致（_tmp/p373/sfx.c 双端同输出）。
 * 故下方 C11/C23 组的三个 TRUE_MIN 直接采用 clang 23 __float_float.h 的原文文本（含后缀），
 * 不做十进制改写，以免与参照编译器出现末位差异。
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

/* P373 补录：C11 起 / C23 标准化的九个浮点特性宏。真值源＝本机 clang 23.1.0 资源头
 * lib/clang/23/include/__float_float.h（其文本转自 __FLT_DECIMAL_DIG__ / __FLT_HAS_DENORM__ /
 * __FLT_DENORM_MIN__ 等目标内建宏），取值经 _tmp/p373/macros.txt 三档实测：
 * clang 默认档（C17）与 -std=c23 档**逐行相同**，故此组无方言门控、无条件定义。
 * 注：clang 在 _MSC_VER 下先 include_next <float.h>（UCRT），再用上述内建宏覆盖，
 * 因此 FLT_TRUE_MIN 取 clang 的 1.40129846e-45F 而非 UCRT 的 1.401298464e-45F；
 * UCRT 根本没有 LDBL_DECIMAL_DIG，此名只可能来自 clang 头（值 17）。 */
#define FLT_DECIMAL_DIG  9
#define DBL_DECIMAL_DIG  17
#define LDBL_DECIMAL_DIG 17

#define FLT_HAS_SUBNORM  1
#define DBL_HAS_SUBNORM  1
#define LDBL_HAS_SUBNORM 1

#define FLT_TRUE_MIN     1.40129846e-45F
#define DBL_TRUE_MIN     4.9406564584124654e-324
#define LDBL_TRUE_MIN    4.9406564584124654e-324L

#endif /* _COLANG_FLOAT_H */
