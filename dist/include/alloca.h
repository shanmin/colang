/* alloca.h — POSIX 兼容头：栈上分配。
 * 声明仅供 sema 定型；实际分配由 codegen 按名 lowering 为 LLVM alloca 指令
 * （见 codegen 的 alloca/__builtin_alloca 分支），符号不进入链接。 */
#ifndef _COLANG_ALLOCA_H
#define _COLANG_ALLOCA_H

#include <stddef.h>

extern void* alloca(size_t n);

#endif /* _COLANG_ALLOCA_H */
