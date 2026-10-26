/* assert.h — colang 自制最小标准头 (P9.9)
 * C §7.2.1.1：assert 为函数式宏。expr 求值为 0（假）时，向 stderr 写诊断
 * （含表达式原文、__FILE__、__LINE__）并调用 abort()；为非 0 时不作任何事。
 * 定义了 NDEBUG 时 assert 展开为 ((void)0)。
 *
 * <assert.h> 依标准可被多次包含，每次按当时的 NDEBUG 重定义 assert，
 * 故本头【不设整体 include guard】；其依赖的 stdio.h/stdlib.h 各自有 guard。
 *
 * 实现要点：采用与 libc 一致的 `cond ? 1 : (诊断, abort, 1)` 惰性三元形态
 * （expression 作条件仅求值一次；为真时走 then 臂 1，else 臂的 fprintf+abort 不执行）。
 * 这依赖 gen_ternary 已按缺陷 #25 修正为真控制流（仅求值被选一臂）；
 * 旧 select 急切求值会让 abort 无条件执行。
 */
#include <stdio.h>
#include <stdlib.h>

#undef assert

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#define assert(expression)                                                  \
	((void)((expression) ? 1 :                                                \
		(fprintf(stderr, "Assertion failed: %s, file %s, line %d\n",     \
			#expression, __FILE__, __LINE__),                          \
			abort(), 1)))
#endif
