/* stdckdint.h — colang 自制最小标准头 (P9.25)
 * checked integer arithmetic（C23 §7.20）。ckd_add/ckd_sub/ckd_mul(result, a, b)
 * 由 colang 前端作为标准内建按名识别（与 va_* 同路数）：colang 直建 IR、clang 前端的
 * __builtin_{add,sub,mul}_overflow 对生成码不可用，故 lowering 在 codegen 完成。
 *
 * 语义（本机 clang ground-truth 校准，C23 §7.20.2）：
 *   - 溢出判定相对 *result 的类型 T（宽度 + 符号），而非运算数的默认提升类型；
 *   - 在 CW=max(w(a),w(b),w(T)) 上精确计算，*result 存入回绕后的值；
 *   - 当操作数严格宽于 T（CW>TW）时，额外检查数学结果是否落在 T 可表示区间，
 *     超界即返回真；返回真（bool，colang 以 i32 0/1 承载）当且仅当发生溢出。
 *
 * 用法：int r; if (ckd_add(&r, a, b)) { /* 溢出，r 为回绕值 *\/ }
 */
#ifndef _COLANG_STDCKDINT_H
#define _COLANG_STDCKDINT_H

/* ckd_add/ckd_sub/ckd_mul 为前端内建（codegen 按名识别，§7.20.2），此处不作宏定义
 * 以免与内建名冲突。包含本头仅为可读性与可移植性占位。 */

#endif /* _COLANG_STDCKDINT_H */
