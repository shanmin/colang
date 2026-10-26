/* colang colib: <stdcountof.h> —— C23 数组元素计数
 *
 * C23 引入 countof：既有关键字/运算符形态 `countof arr`（映射到 _Countof），
 * 也有宏形态 std_countof(arr)。colang 前端目前未实现 _Countof 运算符，故本头
 * 只提供可移植宏 std_countof（以 sizeof 求元素个数），保证数组元素数可直接取用。
 *
 * 用法：std_countof(a) —— a 须为数组（非指针）；对指针会得到 sizeof(ptr)/sizeof(*ptr)，
 * 结果无意义（与标准同为未定义/实现定义行为，不做诊断）。
 *
 * 欠账：`countof arr`（无括号运算符形式）需 parser/codegen 支持 _Countof，暂未做。
 */
#ifndef _COLANG_STDCOUNTOF_H
#define _COLANG_STDCOUNTOF_H

/* C23 7.22：数组元素个数 = 整个数组字节数 / 首元素字节数 */
#define std_countof(a) (sizeof(a) / sizeof((a)[0]))

#endif /* _COLANG_STDCOUNTOF_H */
