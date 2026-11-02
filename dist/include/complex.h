/* complex.h — colang 自制最小标准头
 * C 标准复数支持。colang 依 ADR-104 清单 A 不实现复数类型（_Complex / _Imaginary），
 * 故本头只发特性测试宏，不提供复数类型、不提供算术函数原型，也不定义 I。
 * 有意不定义 I：把 I 定义成实数（例如 1）会让 1.0 + 2.0 * I 静默算成 3.0 ——
 * 错值且零诊断，属最坏失败模式；宁缺毋错。真写复数构造的源仍会得到显式诊断
 * （unexpected token '_Complex' / 未声明标识符 'complex'），不会退化成实数运算。
 * 实测依据：CTS complex 桶 colang 独错 910 例中，131 例只写 #include <complex.h>
 * 而全不使用复数构造（usesI=0），本头即为救回它们所需的全部面。
 * 边界：面向 Windows/UCRT 宿主；完整的 _Complex 类型支持另立议题。 */
#ifndef _COLANG_COMPLEX_H
#define _COLANG_COMPLEX_H

/* C 标准：实现不支持复数时应将 __STDC_NO_COMPLEX__ 定义为 1，
 * 使可移植代码得以改走无复数分支。colang 内建预定义表尚未发出该宏
 * （属编译器侧议题），先由本头提供。 */
#define __STDC_NO_COMPLEX__ 1

#endif /* _COLANG_COMPLEX_H */
