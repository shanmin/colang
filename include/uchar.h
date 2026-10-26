/* uchar.h — colang 自制最小标准头
 * C11 §7.28：char16_t / char32_t 与 UTF 转换。char16_t/char32_t 在 colang 中为内置
 * 关键字类型（见 ADR-104，不重复 typedef），本头仅补齐 mbstate_t 与转换函数声明。
 * 边界：面向 Windows/UCRT 宿主（mbrtoc16/c16rtomb/mbrtoc32/c32rtomb 由 UCRT 提供）。 */
#ifndef _COLANG_UCHAR_H
#define _COLANG_UCHAR_H

#include <stddef.h>
#include <wchar.h>   /* mbstate_t（uchar.h 与 wchar.h 共用同一状态类型） */

extern size_t mbrtoc16(char16_t *pc16, const char *s, size_t n, mbstate_t *ps);
extern size_t c16rtomb(char *s, char16_t c16, mbstate_t *ps);
extern size_t mbrtoc32(char32_t *pc32, const char *s, size_t n, mbstate_t *ps);
extern size_t c32rtomb(char *s, char32_t c32, mbstate_t *ps);

#endif /* _COLANG_UCHAR_H */
