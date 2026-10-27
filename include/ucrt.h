/* ucrt.h — colang 自制扩展头：Microsoft/UCRT 私有扩展（非 C23）
 * 定位：汇总所有由 Microsoft Universal CRT（UCRT）提供、但【不属于 C23 标准】的声明。
 *   凡非 C 标准定义、又非 Win32 API（后者归 winapi.co），而由宿主 UCRT 直链提供的，
 *   统一收于此头；不与标称 C23 的 <string.h>/<wchar.h> 等标准头混编，以保持标准头纯粹。
 *   后续新增的 UCRT 私有函数（如 _wcsdup/_strdup 等）亦归此头。
 * 边界：所列内容在 ISO C 任何部分均无定义，属 UCRT 私有、仅 Windows/UCRT 宿主可用、
 *   不可移植（glibc/Linux 无同名符号；POSIX 侧大小写无关比较为 strcasecmp 等，待平台分派另议）。
 * 链接：符号由宿主 UCRT 提供、直链默认运行库，无需 [[lib]]。
 * 归类先例：与 alloca.h（POSIX 兼容头）同类——非 C23 者单列专头、明示来源。
 */
#ifndef _COLANG_UCRT_H
#define _COLANG_UCRT_H

#include <stddef.h>   /* size_t */
/* wchar_t：colang 内置关键字类型（2 字节无符号），直接使用、无需包含 <wchar.h>。 */

/* —— 大小写不敏感字符串比较（返回值 <0 / 0 / >0 表 a 小于/等于/大于 b，忽略 ASCII 大小写）—— */
extern int _stricmp(const char *a, const char *b);
extern int _strnicmp(const char *a, const char *b, size_t n);
extern int _wcsicmp(const wchar_t *a, const wchar_t *b);
extern int _wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n);

#endif /* _COLANG_UCRT_H */
