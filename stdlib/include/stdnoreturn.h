/* stdnoreturn.h — colang 自制最小标准头 (P9.31)
 * C §7.33：提供 noreturn 作为 _Noreturn 的宏别名。colang 前端已实现 _Noreturn
 * 函数说明符（P5），故本头仅 #define noreturn _Noreturn，纯预处理、零前端改动。
 * （C23 未把 noreturn 提升为关键字，仍由本头以宏提供，与本机 clang 一致。）
 */
#ifndef _COLANG_STDNORETURN_H
#define _COLANG_STDNORETURN_H

#define noreturn _Noreturn

#endif /* _COLANG_STDNORETURN_H */
