/* stdatomic.h — colang 自制最小标准头 (P9.33)
 * C11 §7.17 原子标量（atomic）。atomic_* 自由函数为「编译器内建」（clang 前端走 __c11_atomic_*），
 * colang 直建 IR、不用 clang 前端，故由 codegen 按名特判 lowering 到 LLVM 原子指令（atomicrmw/
 * cmpxchg/load·store ordering/fence），此处**不作函数声明**（与 stdbit/ckd/annexK 内建同路数）。
 * 务实子集：memory_order 实参一律被忽略、统一按 seq_cst；仅对自然字长类型（int 等）保证 lock-free
 * （x86-64 上 LLVM 原子指令天然 lock-free，ATOMIC_*_LOCK_FREE=2）。定时/弱比较交换(CAS_weak)暂不含。
 * _Atomic 目前为 colang parser 的透明类型限定符（尺寸/对齐同基类型），故 atomic_int 等以基类型 typedef。
 */
#ifndef _COLANG_STDATOMIC_H
#define _COLANG_STDATOMIC_H

typedef enum
{
	memory_order_relaxed,
	memory_order_consume,
	memory_order_acquire,
	memory_order_release,
	memory_order_acq_rel,
	memory_order_seq_cst
} memory_order;

/* lock-free 属性：本目标 x86-64 自然字长类型恒 2（always lock-free） */
#define ATOMIC_BOOL_LOCK_FREE    2
#define ATOMIC_CHAR_LOCK_FREE    2
#define ATOMIC_SHORT_LOCK_FREE   2
#define ATOMIC_INT_LOCK_FREE     2
#define ATOMIC_LONG_LOCK_FREE    2
#define ATOMIC_LLONG_LOCK_FREE   2
#define ATOMIC_POINTER_LOCK_FREE 2

/* 原子标量类型：_Atomic 为透明限定符，故以基类型 typedef（尺寸一致、可按名内建操作） */
typedef _Atomic int atomic_int;
typedef _Atomic char atomic_char;
typedef _Atomic short atomic_short;
typedef _Atomic long atomic_long;
typedef _Atomic long long atomic_llong;
typedef _Atomic unsigned int atomic_uint;

/* atomic_flag：以 { int _Lock } 承载（对齐 MSVC 实现；_Lock 偏移 0 供 i32 原子操作。
   colang parser 暂不支持 volatile 限定符，且原子性由 codegen 原子指令保证，无需 volatile） */
typedef struct { int _Lock; } atomic_flag;
#define ATOMIC_FLAG_INIT { 0 }

/* ATOMIC_VAR_INIT：C11 静态初始化辅助宏，C17 起弃用（直接初始化等价），此处保留兼容 */
#define ATOMIC_VAR_INIT(value) (value)

/* atomic_* / atomic_flag_* 自由函数：codegen 按名内建 lowering，不在此声明。 */

#endif /* _COLANG_STDATOMIC_H */
