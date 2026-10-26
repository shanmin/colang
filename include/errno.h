/* errno.h — colang 自制最小标准头 (P9.13)
 * C §7.5：errno 为可修改的 int 左值；EDOM/EILSEQ/ERANGE 为必备错误码（正整数、互异）。
 * errno 宏与错误码数值均由宿主 CRT 决定，colib 对齐本机 UCRT：
 *   - UCRT/MSVC 不提供名为 errno 的数据符号，其标准头以 (*_errno()) 实现该宏
 *     （_errno 返回线程局部 int*）；此处沿用同一 UCRT 稳定契约，已由 clang.exe+UCRT 实测可链接。
 *   - 错误码数值取自本机 UCRT <errno.h>（用 workspace 内 clang.exe 编译系统头探针取得的 ground-truth），
 *     以保证真实 CRT 函数（如 strtol/strtod 溢出置 ERANGE）的设值可与本头常量正确比较。
 * 边界：面向 Windows/UCRT 宿主；glibc(Linux) 下 errno 为 (*__errno_location()) 且数值不同，
 *       待 colang 引入平台预定义宏后再按平台分派。
 */
#ifndef _COLANG_ERRNO_H
#define _COLANG_ERRNO_H

extern int* _errno(void);
#define errno (*_errno())

/* C 标准必备错误码（§7.5） */
#define EDOM   33
#define ERANGE 34
#define EILSEQ 42

/* 常用附加码（值与本机 UCRT 对齐；非标准必备，仅为互操作便利） */
#define EPERM  1
#define ENOENT 2
#define EBADF  9
#define EACCES 13
#define ENOMEM 12
#define EINVAL 22

/* Annex K（C11 §K.3 边界检查接口，P9.30 务实子集）：
 * 实现侧宏 __STDC_LIB_EXT1__ 恒定义以告示“部分支持 Annex K”。
 * errno_t 为 Annex K 函数返回类型；用户须在包含各头之前 #define __STDC_WANT_LIB_EXT1__ 1，
 * 相应 _s 声明才可见（§K.3 门控）。EOK=0 表成功。 */
#define __STDC_LIB_EXT1__ 200509L
#if defined(__STDC_WANT_LIB_EXT1__) && (__STDC_WANT_LIB_EXT1__ == 1)
typedef int errno_t;
#ifndef EOK
#define EOK 0
#endif
#endif

#endif /* _COLANG_ERRNO_H */
