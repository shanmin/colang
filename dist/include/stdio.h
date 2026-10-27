/* stdio.h — colang 自制最小标准头 (P9.0)
 * 仅声明常用 I/O 函数与宏，符号由链接阶段的外部 clang → 宿主 CRT(UCRT/MSVC) 提供。
 * FILE 以不完整类型前向声明（不暴露 CRT 内部布局，够用于 FILE* 传参）。
 */
#ifndef _COLANG_STDIO_H
#define _COLANG_STDIO_H

#include <stddef.h>

typedef struct _colang_FILE FILE;

#define EOF      (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define BUFSIZ   8192
#define FILENAME_MAX 260
#define FOPEN_MAX  20
#define TMP_MAX    238328

extern int printf(const char *fmt, ...);
extern int sprintf(char *s, const char *fmt, ...);
extern int snprintf(char *s, size_t n, const char *fmt, ...);
extern int sscanf(const char *s, const char *fmt, ...);
extern int puts(const char *s);
extern int putchar(int c);
extern int getchar(void);
extern int fprintf(FILE *stream, const char *fmt, ...);
extern int fscanf(FILE *stream, const char *fmt, ...);
extern FILE* fopen(const char *path, const char *mode);
extern int fclose(FILE *stream);

/* stdin/stdout/stderr — C §7.21.1。按目标平台分派（P2xx 引入数据模型/OS 宏后）：
 *   Windows/UCRT：无名为 stderr 的数据符号，用 __acrt_iob_func(0/1/2) 实现三宏（不透明 FILE*）；
 *   Linux/glibc：stdin/stdout/stderr 为 extern FILE* 数据符号。 */
#if defined(__linux__)
extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;
#else
extern FILE* __acrt_iob_func(unsigned);

#define stdin  (__acrt_iob_func(0))
#define stdout (__acrt_iob_func(1))
#define stderr (__acrt_iob_func(2))
#endif

/* Annex K（§K.3.5，P9.30 务实子集）：边界检查格式化输出。直连本机 UCRT 导出的 ISO 同名符号。 */
#if defined(__STDC_WANT_LIB_EXT1__) && (__STDC_WANT_LIB_EXT1__ == 1)
extern int sprintf_s(char *s, rsize_t n, const char *fmt, ...);
#endif

#endif /* _COLANG_STDIO_H */
