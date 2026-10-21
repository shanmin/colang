/* colang colib: <stdalign.h> —— C23 对齐支持头
 *
 * C23 中 alignas / alignof 已是语言关键字（colang 前端直接识别，无需宏映射），
 * 故本头在 C23 方言下基本为空地占位，仅保证 `#include <stdalign.h>` 可解析。
 * 与本机 clang 23 (-std=c2x) 行为一致：C23 分支不再定义 __alignas_is_defined /
 * __alignof_is_defined（那两个宏只在早于 C23 的方言里由本头补出）。
 *
 * 现状说明（colang 前端）：
 *   - alignof(T) / _Alignof(T)：已实现，codegen 取 DataLayout.getABITypeAlign，结果正确。
 *   - alignas(N) / _Alignas(N)：parser 接受并跳过（当作存储层属性），但 codegen 暂未
 *     据此提升对象对齐——即“语法可过、对齐不生效”，属已知前端欠账（对齐强制待做）。
 */
#ifndef _COLANG_STDALIGN_H
#define _COLANG_STDALIGN_H

/* C23：alignas/alignof 为关键字，本头无需（也不应）再宏定义它们。 */

#endif /* _COLANG_STDALIGN_H */
