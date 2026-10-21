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
extern FILE *fopen(const char *path, const char *mode);
extern int fclose(FILE *stream);

/* stdin/stdout/stderr — C §7.21.1，类型为 FILE* 的外部对象式宏。
 * UCRT/MSVC 不提供名为 stderr 的数据符号，其标准头用 __acrt_iob_func(0/1/2) 实现
 * 这三个宏（返回不透明 FILE*，无需 sizeof(FILE)）；此处沿用同一 UCRT 稳定契约，
 * 已由 clang.exe+UCRT 实测可链接且可作 fprintf 目标。
 * 边界：面向 Windows/UCRT 宿主；glibc(Linux) 下 stderr 为 extern FILE* 数据符号，
 *       待 colang 引入平台预定义宏后再按平台分派。*/
extern FILE *__acrt_iob_func(unsigned);

#define stdin  (__acrt_iob_func(0))
#define stdout (__acrt_iob_func(1))
#define stderr (__acrt_iob_func(2))

/* Annex K（§K.3.5，P9.30 务实子集）：边界检查格式化输出。直连本机 UCRT 导出的 ISO 同名符号。 */
#if defined(__STDC_WANT_LIB_EXT1__) && (__STDC_WANT_LIB_EXT1__ == 1)
extern int sprintf_s(char *s, rsize_t n, const char *fmt, ...);
#endif

#endif /* _COLANG_STDIO_H */
