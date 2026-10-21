/* iso646.h — colang 自制最小标准头 (P9.31)
 * C §7.13：交替拼写的标点记号（alternative spellings）。纯对象式宏、文本/token 替换，
 * 前端零改动（由预处理器展开，与关键字/运算符无冲突：colang 未把 and/or/not/xor/compl/
 * bitand/bitor 识别为运算符或关键字）。此处仅提供 §7.13 保证的 14 个记号；
 * altEOF/altEOL/altBlanks/altGrid/altEpt 为实现可选（optional），本机未采用故不定义。
 */
#ifndef _COLANG_ISO646_H
#define _COLANG_ISO646_H

#define and    &&
#define and_eq &=
#define bitand &
#define bitor  |
#define compl  ~
#define not    !
#define not_eq !=
#define or     ||
#define or_eq  |=
#define xor    ^
#define xor_eq ^=

#endif /* _COLANG_ISO646_H */
