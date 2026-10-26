/* stddef.h — colang 自制最小标准头 (P9.0；offsetof 于 P9.3 补齐)
 * 仅含前端可解析的 typedef 与宏；size_t/ptrdiff_t 按 LLP64(Windows x86_64)：8 字节。
 */
#ifndef _COLANG_STDDEF_H
#define _COLANG_STDDEF_H

typedef unsigned long long size_t;
typedef long long ptrdiff_t;

/* C23 §6.2.5/§7.19：nullptr_t —— nullptr 的类型。务实以 void* 别名承载：
 * 表示宽度/对齐同 void*（故 sizeof(nullptr_t)==sizeof(void*)），可隐转任意指针，
 * 与 nullptr 字面量（前端以 Void* 建模）互操作一致。 */
typedef void *nullptr_t;

#ifndef NULL
#define NULL 0
#endif

/* offsetof（C §6.5.3.4/§7.20）：以前端内建 `__builtin_offsetof` 实现，编译期展开为字节偏移常量。
 * 既能用于 ordinary 表达式/静态初始化，也能用于数组长度、case 标签等“需常量求值”的场合。
 * 内建采用对齐感知的标准 C 布局（与 codegen 的 LLVM StructType/DataLayout 一致），
 * 支持嵌套 `a.b`、数组下标 `a[i]`、union（成员恒偏移 0）。
 */
#ifndef offsetof
#define offsetof(TYPE, M) __builtin_offsetof(TYPE, M)
#endif

/* Annex K（§K.3，P9.30 务实子集）：rsize_t 与 RSIZE_MAX，仅在用户定义
 * __STDC_WANT_LIB_EXT1__ 为 1 时可见（与各 _s 声明同门控）。 */
#if defined(__STDC_WANT_LIB_EXT1__) && (__STDC_WANT_LIB_EXT1__ == 1)
typedef size_t rsize_t;
#ifndef RSIZE_MAX
#define RSIZE_MAX (~(size_t)0 >> 1)
#endif
#endif

#endif /* _COLANG_STDDEF_H */
