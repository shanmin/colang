/* string.h — colang 自制最小标准头 (P9.0)
 * 常用内存/字符串操作声明，符号由宿主 CRT 提供。
 */
#ifndef _COLANG_STRING_H
#define _COLANG_STRING_H

#include <stddef.h>

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memmove(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int c, size_t n);
extern int memcmp(const void *a, const void *b, size_t n);
extern void *memchr(const void *s, int c, size_t n);

extern size_t strlen(const char *s);
extern char *strcpy(char *dst, const char *src);
extern char *strncpy(char *dst, const char *src, size_t n);
extern char *strcat(char *dst, const char *src);
extern int strcmp(const char *a, const char *b);
extern int strncmp(const char *a, const char *b, size_t n);
extern char *strchr(const char *s, int c);
extern char *strrchr(const char *s, int c);
extern char *strstr(const char *haystack, const char *needle);

/* Annex K（§K.3.7/K.3.8，P9.30 务实子集）：边界检查字符串/内存函数。
 * 仅在 #define __STDC_WANT_LIB_EXT1__ 1 后包含本头时可见。
 * 以下声明直连本机 UCRT 已导出的 ISO 同名符号（形参布局与 Annex K 一致）；
 * memcpy_s/memmove_s 按 Annex K 声明为 void（UCRT 实为 errno_t 返回，调用处忽略）。
 * memset_s / strnlen_s 本机 UCRT 无 ISO 符号，由 colang 前端按名内建 lowering（原型仅供 sema 定型，
 * 实际 lowering 在 codegen 于正常解析之前按名拦截，声明不冲突）：
 * memset_s 越界钳制后填并返回非 0/0；strnlen_s 返回 NUL 前长度或 max。 */
#if defined(__STDC_WANT_LIB_EXT1__) && (__STDC_WANT_LIB_EXT1__ == 1)
#include <errno.h>   /* errno_t */
extern errno_t strcpy_s(char *s1, rsize_t s1max, const char *s2);
extern errno_t strncpy_s(char *s1, rsize_t s1max, const char *s2, rsize_t n);
extern errno_t strcat_s(char *s1, rsize_t s1max, const char *s2);
extern errno_t strncat_s(char *s1, rsize_t s1max, const char *s2, rsize_t n);
extern errno_t strerror_s(char *buf, rsize_t bufsz, errno_t errnum);
extern void memcpy_s(void *s1, rsize_t s1max, const void *s2, rsize_t n);
extern void memmove_s(void *s1, rsize_t s1max, const void *s2, rsize_t n);
extern errno_t memset_s(void *s, rsize_t smax, int c, rsize_t n);   /* 前端内建 lowering */
extern rsize_t strnlen_s(const char *s, rsize_t n);                /* 前端内建 lowering */
#endif

#endif /* _COLANG_STRING_H */
