/* stdarg.h — colang 自制最小标准头 (P9.2)
 * 可变参数处理（C §7.16）。va_list 按 Windows x86_64/MSVC ABI 定义为 char*（8 字节指针）。
 * va_start/va_arg/va_end/va_copy 由 colang 前端作为标准内建识别（§7.16 允许实现同时以函数/内建提供），
 * 与 clang Win64 产出的 IR 对齐：va_start/va_end/va_copy 走 llvm intrinsic，va_arg 走指针算术。
 */
#ifndef _COLANG_STDARG_H
#define _COLANG_STDARG_H

#ifndef _VA_LIST
#define _VA_LIST
typedef char *va_list;
#endif

#endif /* _COLANG_STDARG_H */
