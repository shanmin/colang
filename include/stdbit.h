/* stdbit.h — colang 自制最小标准头 (P9.26)
 * 标准位操作（C23 §7.18）。stdc_<op>[_uc|_us|_ui|_ul|_ull] 由 colang 前端作为
 * 标准内建按名识别（与 va_ 系列、ckd_ 系列同路数）：colang 直建 IR、本机 clang 亦无
 * stdbit.h，故 lowering 在 codegen 完成（映射到 llvm.ctpop/ctlz/cttz + 算术）。
 *
 * 语义严格对齐发布版 ISO/IEC 9899:2024 §7.18.3–7.18.16 的 Returns 条款：
 *   - 计数类返回 unsigned int：
 *       leading_zeros/leading_ones/trailing_zeros/trailing_ones：自对应端起连续同类位数
 *       count_ones/count_zeros：整体 1/0 位个数（count_zeros = W - count_ones）
 *       first_leading_zero/one、first_trailing_zero/one：上述连续计数 +1，若全同类则 0
 *       bit_width = W - clz(value)（value=0 时为 0）
 *   - has_single_bit 返回 bool（colang 以 i32 0/1 承载）：value 恰含单个 1 位
 *   - bit_floor/bit_ceil 返回同宽无符号值；bit_ceil 结果不可表示时返回 0
 *   - 参数按无符号 W 位处理；签名后缀固定宽度，无后缀者按实参类型宽度（泛型宏）
 *
 * 用法：unsigned n = stdc_count_ones(0xF0u);            // 泛型宏
 *       unsigned m = stdc_count_ones_ui(0xF0u);         // 显式 unsigned int
 */
#ifndef _COLANG_STDBIT_H
#define _COLANG_STDBIT_H

#define __STDC_VERSION_STDBIT_H__ 202311L

/* 字节序（本目标 x86/小端）*/
#define __STDC_ENDIAN_LITTLE__ 1234
#define __STDC_ENDIAN_BIG__    4321
#define __STDC_ENDIAN_NATIVE__ __STDC_ENDIAN_LITTLE__

/* stdc_* 为前端内建（codegen 按名识别，§7.18），此处不作宏定义以免与内建名冲突。
 * 包含本头仅提供版本宏/字节序常量与可移植性占位。 */

#endif /* _COLANG_STDBIT_H */
