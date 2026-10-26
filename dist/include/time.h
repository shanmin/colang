/* time.h — colang 自制最小标准头 (P9.14)
 * C §7.27 日期与时间。宽度/结构体布局均取自本机 UCRT ground-truth
 * （用 workspace 内 clang.exe 编译系统 <time.h> 探针实测）：
 *   time_t = 8 字节(__int64)、clock_t = 4 字节(long)、
 *   struct tm = 9 个连续 int（tm_sec..tm_isdst，偏移 0..32，无 POSIX 扩展字段）、
 *   CLOCKS_PER_SEC = 1000。
 * 如此 colang 侧 struct tm 可与真实 CRT 函数按指针安全交换（字段偏移/宽度一致）。
 * 边界：面向 Windows/UCRT；glibc(Linux) 下 time_t/clock_t 宽度与 struct tm 字段
 *       （含 tm_gmtoff/tm_zone）不同，待 colang 引入平台预定义宏后按平台分派。
 */
#ifndef _COLANG_TIME_H
#define _COLANG_TIME_H

#include <stddef.h>

typedef long long time_t;   /* 8 字节，对齐 UCRT __int64 */
typedef long clock_t;       /* 4 字节，对齐 UCRT long */

/* POSIX 扩展：UCRT 亦在 <time.h> 提供该结构（tv_sec 为 8 字节 time_t） */
struct timespec {
    time_t tv_sec;   /* 秒 */
    long   tv_nsec;  /* 纳秒 */
};

struct tm {
    int tm_sec;    /* 秒 [0,60] */
    int tm_min;    /* 分 [0,59] */
    int tm_hour;   /* 时 [0,23] */
    int tm_mday;   /* 月内日 [1,31] */
    int tm_mon;    /* 月 [0,11] */
    int tm_year;   /* 自 1900 起的年数 */
    int tm_wday;   /* 星期 [0,6]，Sunday=0 */
    int tm_yday;   /* 年内日 [0,365] */
    int tm_isdst;  /* 夏令时标志 */
};

#define CLOCKS_PER_SEC 1000

/* 仅纳入以指针交换 struct tm / time_t 的函数（避免 struct 按值跨 ABI 传递） */
extern time_t  time(time_t *t);
extern clock_t clock(void);
extern double  difftime(time_t end, time_t start);
extern struct tm* gmtime(const time_t *t);
extern struct tm* localtime(const time_t *t);
extern time_t  mktime(struct tm *tm);
extern char* asctime(const struct tm *tm);
extern size_t  strftime(char *s, size_t max, const char *fmt, const struct tm *tm);

#endif /* _COLANG_TIME_H */
