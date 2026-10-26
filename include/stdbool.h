/* stdbool.h — colang 自制最小标准头 (P9.0)
 * colang 已把 bool/true/false 作为 C23 关键字处理，故此处仅提供 __bool_true_false_are_defined 哨兵宏，
 * 不再 #define（避免与关键字冲突）。
 */
#ifndef _COLANG_STDBOOL_H
#define _COLANG_STDBOOL_H

#define __bool_true_false_are_defined 1

#endif /* _COLANG_STDBOOL_H */
