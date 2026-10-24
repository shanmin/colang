/* wchar.h — colang 自制最小标准头 (P9.18)
 * C §7.29：宽字符支持。类型宽度/符号取自本机 UCRT/MSVC ground-truth：
 *   sizeof(wchar_t)=2、wchar_t 为无符号（__WCHAR_TYPE__=unsigned short, __WCHAR_UNSIGNED__=1）；
 *   sizeof(wint_t)=2、同为 unsigned short（MSVC 传统：wint_t 亦 2 字节，非 POSIX 的 4）；WEOF=0xFFFF。
 * wchar_t 是标准保留类型名，C 里通常由 <stddef.h>/<wchar.h> 以 typedef 提供；colang 为避免
 *   `short`→i32 折叠给不出 2 字节宽度，改由前端内置识别（见 ADR-104 裁定）：词法器把 wchar_t
 *   登记为类型关键字，parser 映射到 TypeKind::WChar，codegen 落 i16 且整型提升走零扩展。
 *   因此本头【不再】typedef wchar_t（会与内置类型名冲突），仅补齐其余类型/宏/函数声明。
 * 边界：面向 Windows/UCRT 宿主；glibc(Linux) 下 wchar_t/wint_t 均为 4 字节，待平台分派另议。
 */
#ifndef _COLANG_WCHAR_H
#define _COLANG_WCHAR_H

#include <stddef.h>   /* size_t / ptrdiff_t / NULL */
#include <stdio.h>    /* FILE */

/* wchar_t：内置关键字类型（2 字节无符号），此处不重复 typedef。 */

/* wint_t：与 wchar_t 同宽（本机 UCRT 均为 unsigned short）；可容 WEOF。 */
typedef wchar_t wint_t;

/* mbstate_t：移位状态，标准仅作对象类型用于 *r* 转换函数的指针形参；此处取最小可寻址尺寸。 */
typedef unsigned int mbstate_t;

#ifndef WEOF
#define WEOF ((wint_t)(0xFFFF))
#endif

/* —— 宽字符串/内存操作（符号由宿主 UCRT 提供）—— */
extern size_t wcslen(const wchar_t *s);
extern wchar_t* wcscpy(wchar_t *dst, const wchar_t *src);
extern wchar_t* wcsncpy(wchar_t *dst, const wchar_t *src, size_t n);
extern wchar_t* wcscat(wchar_t *dst, const wchar_t *src);
extern int wcscmp(const wchar_t *a, const wchar_t *b);
extern int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
extern wchar_t* wcschr(const wchar_t *s, wchar_t c);
extern wchar_t* wcsrchr(const wchar_t *s, wchar_t c);
extern wchar_t* wcsstr(const wchar_t *haystack, const wchar_t *needle);
extern wchar_t* wcstok(wchar_t *s, const wchar_t *delim, wchar_t **ptr);
extern int wcscoll(const wchar_t *a, const wchar_t *b);
extern size_t wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n);

extern void* wmemcpy(wchar_t *dst, const wchar_t *src, size_t n);
extern void* wmemmove(wchar_t *dst, const wchar_t *src, size_t n);
extern void* wmemset(wchar_t *dst, wchar_t c, size_t n);
extern int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);
extern void* wmemchr(const wchar_t *s, wchar_t c, size_t n);

/* —— 宽字符 I/O —— */
extern int fwide(FILE *stream, int mode);
extern wint_t fgetwc(FILE *stream);
extern wint_t fputwc(wchar_t c, FILE *stream);
extern wchar_t* fgetws(wchar_t *s, int n, FILE *stream);
extern int fputws(const wchar_t *s, FILE *stream);
extern wint_t getwc(FILE *stream);
extern wint_t getwchar(void);
extern wint_t putwc(wchar_t c, FILE *stream);
extern wint_t putwchar(wchar_t c);
extern wint_t ungetwc(wint_t c, FILE *stream);
extern int fwprintf(FILE *stream, const wchar_t *fmt, ...);
extern int fwscanf(FILE *stream, const wchar_t *fmt, ...);
extern int wprintf(const wchar_t *fmt, ...);
extern int swprintf(wchar_t *s, size_t n, const wchar_t *fmt, ...);

/* —— 字符/宽字符转换 —— */
extern wint_t btowc(int c);
extern int wctob(wint_t c);
extern int mbsinit(const mbstate_t *ps);
extern size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps);
extern size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps);
extern size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps);
extern size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *ps);

/* —— 数值转换 —— */
extern long wcstol(const wchar_t *s, wchar_t **endp, int base);
extern unsigned long wcstoul(const wchar_t *s, wchar_t **endp, int base);
extern long long wcstoll(const wchar_t *s, wchar_t **endp, int base);
extern unsigned long long wcstoull(const wchar_t *s, wchar_t **endp, int base);
extern double wcstod(const wchar_t *s, wchar_t **endp);
extern float wcstof(const wchar_t *s, wchar_t **endp);

#endif /* _COLANG_WCHAR_H */
