/* stdlib.h — colang 自制最小标准头 (P9.0)
 * 内存分配 / 数值转换 / 进程控制等函数声明，符号由宿主 CRT 提供。
 */
#ifndef _COLANG_STDLIB_H
#define _COLANG_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     2147483647

typedef struct { int quot; int rem; } div_t;
typedef struct { long long quot; long long rem; } ldiv_t;

extern void *malloc(size_t size);
extern void *calloc(size_t nmemb, size_t size);
extern void *realloc(void *ptr, size_t size);
extern void free(void *ptr);

extern void abort(void);
extern void exit(int status);

extern int atoi(const char *s);
extern long atol(const char *s);
extern double atof(const char *s);
extern long strtol(const char *s, char **end, int base);
extern double strtod(const char *s, char **end);

extern int rand(void);
extern void srand(unsigned int seed);

extern int system(const char *cmd);
extern div_t div(int numer, int denom);

/* Annex K（§K.3，P9.30 务实子集）：
 * 约束处理三件套 set_constraint_handler_s/abort_handler_s/ignore_handler_s 本机 UCRT 无 ISO 符号，
 * 由 colang 前端按名内建 lowering（原型仅供 sema 定型，实际 lowering 在 codegen 于正常解析之前按名拦截，
 * 声明不冲突）；非严格符合（详注释见 string.h/PROJECT.md）。
 * 以下检索/排序直连 UCRT 已导出符号，比较器约定按 Annex K（qsort_s 首参 context、bsearch_s 末参 context）；
 * 本批未纳入回归实测（跨 ABI 比较器约定差异敏感）。 */
#if defined(__STDC_WANT_LIB_EXT1__) && (__STDC_WANT_LIB_EXT1__ == 1)
#include <errno.h>   /* errno_t */
typedef void (*constraint_handler_t)(const char *msg, void *ptr, errno_t error);
extern constraint_handler_t set_constraint_handler_s(constraint_handler_t handler);  /* 前端内建 */
extern void abort_handler_s(const char *msg, void *ptr, errno_t error);              /* 前端内建 */
extern void ignore_handler_s(const char *msg, void *ptr, errno_t error);             /* 前端内建 */
/* 比较器形参以 typedef 表达（colang 尚不能解析内联函数指针形参 `int (*compar)(...)`）*/
typedef int (*_colang_compar_key)(const void *, const void *, void *);
typedef int (*_colang_compar_ctx)(void *, const void *, const void *);
extern void *bsearch_s(const void *key, const void *base, rsize_t nmemb, rsize_t size,
                       _colang_compar_key compar, void *context);
extern void qsort_s(void *base, rsize_t nmemb, rsize_t size,
                    _colang_compar_ctx compar, void *context);
#endif

#endif /* _COLANG_STDLIB_H */
