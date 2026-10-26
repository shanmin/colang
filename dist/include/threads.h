/* threads.h — colang 自制最小标准头 (P9.32)
 * C11 §7.26 线程支持（threads）。符号由本机 MSVC/UCRT 提供（VS2022 起 UCRT 完整实现
 * C11 threads：thrd_/mtx_/cnd_/tss_/call_once，已由 clang+UCRT 实测可链接、可真实多线程运行）。
 * 句柄类型 thrd_t/mtx_t/cnd_t/once_flag 按其「不透明」性质以等宽 size_t 数组承载，
 * 尺寸/对齐取自本机 ground-truth（thrd_t=16/8, mtx_t=32/8, cnd_t=8/8, once_flag=8/8）；
 * tss_t 为 4 字节整型（键值，非句柄）。
 * 务实子集：暂不含需 <time.h> struct timespec 的定时变体（thrd_sleep/mtx_timedlock/cnd_timedwait）。
 * 边界：面向 Windows/MSVC ABI；glibc(Linux) 下各句柄尺寸不同（如 thrd_t=8、mtx_t=48…），
 *       待 colang 引入平台分派后按平台取布局（见 PROJECT.md §六 平台预定义宏）。
 */
#ifndef _COLANG_THREADS_H
#define _COLANG_THREADS_H

#include <stddef.h>

typedef int (*thrd_start_t)(void *arg);
typedef void (*tss_dtor_t)(void *val);
typedef void (*thrd_cb_t)(void);   /* call_once 回调（colang 不支持内联函数指针形参，故 typedef 化）*/

typedef struct { size_t _H[2]; } thrd_t;      /* 16 字节 / 对齐 8 */
typedef struct { size_t _H[4]; } mtx_t;       /* 32 字节 / 对齐 8 */
typedef struct { size_t _H; } cnd_t;          /* 8 字节 / 对齐 8 */
typedef unsigned int tss_t;                   /* 4 字节 / 对齐 4 */
typedef struct { size_t _H; } once_flag;      /* 8 字节 / 对齐 8 */

#define ONCE_FLAG_INIT { 0 }

/* thrd_* 结果码与 mtx_* 类型：值对齐本机 MSVC ground-truth（实现定义常量，§7.26.2.1/§7.26.2.4） */
#define thrd_success  0
#define thrd_nomem    1
#define thrd_timedout 2
#define thrd_busy     3
#define thrd_error    4

#define mtx_plain     0
#define mtx_recursive 1
#define mtx_timed     2

#define TSS_DTOR_ITERATIONS 4

/* —— 线程（§7.26.4）—— 注：thrd_join/thrd_detach/thrd_equal/thrd_current 以 by-value 传/回 thrd_t */
extern int thrd_create(thrd_t *thr, thrd_start_t func, void *arg);
extern int thrd_equal(thrd_t lhs, thrd_t rhs);
extern thrd_t thrd_current(void);
extern int thrd_detach(thrd_t thr);
extern int thrd_join(thrd_t thr, int *res);
extern void thrd_yield(void);

/* —— 互斥量（§7.26.4）—— */
extern int mtx_init(mtx_t *mtx, int type);
extern void mtx_destroy(mtx_t *mtx);
extern int mtx_lock(mtx_t *mtx);
extern int mtx_trylock(mtx_t *mtx);
extern int mtx_unlock(mtx_t *mtx);

/* —— 条件变量（§7.26.4）—— */
extern int cnd_init(cnd_t *cond);
extern void cnd_destroy(cnd_t *cond);
extern int cnd_signal(cnd_t *cond);
extern int cnd_broadcast(cnd_t *cond);
extern int cnd_wait(cnd_t *cond, mtx_t *mtx);

/* —— 线程存储（§7.26.4）—— */
extern int tss_create(tss_t *key, tss_dtor_t dtor);
extern void tss_delete(tss_t key);
extern int tss_set(tss_t key, void *val);
extern void* tss_get(tss_t key);

/* —— 一次性调用（§7.26.11）—— */
extern void call_once(once_flag *flag, thrd_cb_t func);

#endif /* _COLANG_THREADS_H */
