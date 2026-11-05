/* threads.h — colang 自制最小标准头 (P9.32)
 * C11 标准线程支持（threads）。符号由本机 MSVC/UCRT 提供（VS2022 起 UCRT 完整实现
 * C11 threads：thrd_/mtx_/cnd_/tss_/call_once，已由 clang+UCRT 实测可链接、可真实多线程运行）。
 * 句柄类型 thrd_t/mtx_t/cnd_t/once_flag 按其「不透明」性质以等宽 size_t 数组承载，
 * 尺寸/对齐取自本机 ground-truth（thrd_t=16/8, mtx_t=32/8, cnd_t=8/8, once_flag=8/8）；
 * tss_t 为 4 字节整型（键值，非句柄）。
 * C11 定时变体（thrd_sleep/mtx_timedlock/cnd_timedwait）已补齐：本头 #include <time.h>
 * 以保证 struct timespec 自洽可见（C11 要求；外部 C-03 第②项已闭合）。
 * 边界：面向 Windows/MSVC ABI；glibc(Linux) 下各句柄尺寸不同（如 thrd_t=8、mtx_t=48…），
 *       待 colang 引入平台分派后按平台取布局（平台预定义宏清单见项目内部设计文档）。
 */
#ifndef _COLANG_THREADS_H
#define _COLANG_THREADS_H

#include <stddef.h>
#include <time.h>   /* struct timespec（C11 要求 <threads.h> 保证其可见） */

typedef int (*thrd_start_t)(void *arg);
typedef void (*tss_dtor_t)(void *val);
typedef void (*thrd_cb_t)(void);   /* call_once 回调（colang 不支持内联函数指针形参，故 typedef 化）*/

typedef struct { size_t _H[2]; } thrd_t;      /* 16 字节 / 对齐 8 */
typedef struct { size_t _H[4]; } mtx_t;       /* 32 字节 / 对齐 8 */
typedef struct { size_t _H; } cnd_t;          /* 8 字节 / 对齐 8 */
typedef unsigned int tss_t;                   /* 4 字节 / 对齐 4 */
typedef struct { size_t _H; } once_flag;      /* 8 字节 / 对齐 8 */

#define ONCE_FLAG_INIT { 0 }

/* thrd_* 结果码与 mtx_* 类型：值对齐本机 MSVC ground-truth（实现定义常量，C 标准） */
#define thrd_success  0
#define thrd_nomem    1
#define thrd_timedout 2
#define thrd_busy     3
#define thrd_error    4

#define mtx_plain     0
#define mtx_recursive 1
#define mtx_timed     2

#define TSS_DTOR_ITERATIONS 4

/* —— 线程（C 标准）—— 注：thrd_join/thrd_detach/thrd_equal/thrd_current 以 by-value 传/回 thrd_t */
extern int thrd_create(thrd_t *thr, thrd_start_t func, void *arg);
extern int thrd_equal(thrd_t lhs, thrd_t rhs);
extern thrd_t thrd_current(void);
extern int thrd_detach(thrd_t thr);
extern int thrd_join(thrd_t thr, int *res);
extern void thrd_yield(void);

/* —— 互斥量（C 标准）—— */
extern int mtx_init(mtx_t *mtx, int type);
extern void mtx_destroy(mtx_t *mtx);
extern int mtx_lock(mtx_t *mtx);
extern int mtx_trylock(mtx_t *mtx);
extern int mtx_unlock(mtx_t *mtx);

/* —— 条件变量（C 标准）—— */
extern int cnd_init(cnd_t *cond);
extern void cnd_destroy(cnd_t *cond);
extern int cnd_signal(cnd_t *cond);
extern int cnd_broadcast(cnd_t *cond);
extern int cnd_wait(cnd_t *cond, mtx_t *mtx);

/* —— 线程存储（C 标准）—— */
extern int tss_create(tss_t *key, tss_dtor_t dtor);
extern void tss_delete(tss_t key);
extern int tss_set(tss_t key, void *val);
extern void* tss_get(tss_t key);

/* —— 一次性调用（C 标准）—— */
extern void call_once(once_flag *flag, thrd_cb_t func);

/* —— C11 定时变体（thrd_sleep / mtx_timedlock / cnd_timedwait）——
 * UCRT 不直接导出这三个 C11 名，真实 MSVC <threads.h> 以 static inline 包装 _*64 家族。
 * 本仓 time_t 恒 8 字节（等价 MSVC 的 64 位分支），故只取 _*64 变体、省去 _USE_32BIT_TIME_T /
 * _CRT_NO_TIME_T 分岔。_timespec64 与 <time.h> 的 struct timespec 布局逐字节相同（{ __int64, long }），
 * 包装层在二者间做指针转型（与 MSVC 做法一致）。 */
struct _timespec64 { long long tv_sec; long tv_nsec; };
extern int _thrd_sleep64(const struct _timespec64 *duration, struct _timespec64 *remaining);
extern int _mtx_timedlock64(mtx_t *mtx, const struct _timespec64 *ts);
extern int _cnd_timedwait64(cnd_t *cond, mtx_t *mtx, const struct _timespec64 *ts);

static inline int thrd_sleep(const struct timespec *duration, struct timespec *remaining) {
	return _thrd_sleep64((const struct _timespec64 *) duration, (struct _timespec64 *) remaining);
}
static inline int mtx_timedlock(mtx_t *mtx, const struct timespec *ts) {
	return _mtx_timedlock64(mtx, (const struct _timespec64 *) ts);
}
static inline int cnd_timedwait(cnd_t *cond, mtx_t *mtx, const struct timespec *ts) {
	return _cnd_timedwait64(cond, mtx, (const struct _timespec64 *) ts);
}

#endif /* _COLANG_THREADS_H */
