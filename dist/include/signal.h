/* signal.h — colang 自制最小标准头 (P9.17)
 * C §7.14：sig_atomic_t / signal / raise / SIG_DFL / SIG_IGN / SIG_ERR / SIG* 信号号。
 * 仅纳入 C 标准集合（不做 POSIX sigaction/sigset_t/NSIG 等扩展，见 ADR-104）。
 * 信号号取值随平台而变，用 P9.16 落地的 _WIN32 预定义宏分派：
 *   - Windows/UCRT ground-truth（本机 clang 实测）：SIGABRT=22 SIGFPE=8 SIGILL=4 SIGINT=2 SIGSEGV=11 SIGTERM=15
 *   - POSIX/glibc 惯例值（移植参考）：SIGABRT=6 SIGFPE=8 SIGILL=4 SIGINT=2 SIGSEGV=11 SIGTERM=15
 * SIG_DFL/SIG_IGN/SIG_ERR 为函数指针常量 (void(*)(int))0/1/-1——两平台 UCRT/glibc 同值。
 *   其"整型常量→指针常量"折叠依赖 codegen 缺陷 #28 修复（全局初始化不再一律折成 null）。
 * 处理器类型用 typedef _sighandler_t 命名，规避 "函数返回函数指针" 的老式声明解析。
 * sig_atomic_t：C 要求可为 int；UCRT(x64) 实测 sizeof=4，即 int。
 */
#ifndef _COLANG_SIGNAL_H
#define _COLANG_SIGNAL_H

typedef int sig_atomic_t;

/* 信号处理器类型：void (*)(int) */
typedef void (*_colang_sighandler_t)(int);

#define SIG_DFL ((_colang_sighandler_t)0)
#define SIG_IGN ((_colang_sighandler_t)1)
#define SIG_ERR ((_colang_sighandler_t)-1)

#ifdef _WIN32
/* Windows/UCRT 信号号（本机 clang ground-truth） */
#define SIGABRT 22
#define SIGFPE  8
#define SIGILL  4
#define SIGINT  2
#define SIGSEGV 11
#define SIGTERM 15
#else
/* POSIX/glibc 信号号（移植参考值） */
#define SIGABRT 6
#define SIGFPE  8
#define SIGILL  4
#define SIGINT  2
#define SIGSEGV 11
#define SIGTERM 15
#endif

extern _colang_sighandler_t signal(int _Sig, _colang_sighandler_t _Func);
extern int raise(int _Sig);

#endif /* _COLANG_SIGNAL_H */
