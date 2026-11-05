/* complex.h — colang 自制标准头（C99 Annex G 复数·核心切片）
 * 本批落地核心切片：_Complex 类型 + + - * / 算术 + I/_Complex_I 虚数单位 + 访问器
 * （creal/cimag/conj/cabs/carg/projection，double 与 float 两型）。
 * 超越/幂对数函数（cexp/clog/csqrt/ctrig/cpow 等）留后续批。
 *
 * 特性测试宏策略（决策 4）：本批有意**保留** __STDC_NO_COMPLEX__=1——CTS 里 131 例靠该宏
 * 自跳过复数构造，若提前摘除会激活其 #ifndef 分支里本轮未实现的 cexp/clog 等、致回归。
 * 全量复数达标后另批摘除。代价：本头处于「宏谎报不支持、实则提供核心切片」的临时态，
 * 属工程折中，非最终符合性形态。
 *
 * 访问器为 colang 内建（codegen 按名 lowering 到 extractvalue/UCRT sqrt/atan2 等），
 * 下方原型仅供 sema 定型/元数检查，不产生外部链接需求。 */
#ifndef _COLANG_COMPLEX_H
#define _COLANG_COMPLEX_H

#define __STDC_NO_COMPLEX__ 1

/* 复数关键字别名与虚数单位。_Complex_I 为 lexer 关键字、parser 识别为内建常量 {0,1}；
 * I 经本宏展开到 _Complex_I（C 标准：Annex G 允许 _Imaginary 缺席、此时 I == _Complex_I）。 */
#define complex _Complex
#define I _Complex_I

/* 访问器（double 复数 → double 标量 / double 复数）。 */
double creal(double _Complex z);
double cimag(double _Complex z);
double cabs(double _Complex z);
double carg(double _Complex z);
double _Complex conj(double _Complex z);
double _Complex projection(double _Complex z);

/* 访问器（float 复数 → float 标量 / float 复数）。 */
float crealf(float _Complex z);
float cimagf(float _Complex z);
float cabsf(float _Complex z);
float cargf(float _Complex z);
float _Complex conjf(float _Complex z);
float _Complex projectionf(float _Complex z);

#endif /* _COLANG_COMPLEX_H */
