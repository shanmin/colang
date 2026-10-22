/* sys/time.h — POSIX 兼容头：struct timeval / gettimeofday（C11 之外的扩展）。
 * 本机 UCRT 无此头；布局按 Windows x64（long 为 32 位）给出。仅声明，不提供符号，
 * 调用 gettimeofday 会链接失败——CTS 中该头多出现在 #if defined(__linux) 保护块内。 */
#ifndef _COLANG_SYS_TIME_H
#define _COLANG_SYS_TIME_H

#include <time.h>

struct timeval {
    long tv_sec;   /* 秒 */
    long tv_usec;  /* 微秒 */
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

extern int gettimeofday(struct timeval *tv, void *tz);

#endif /* _COLANG_SYS_TIME_H */
