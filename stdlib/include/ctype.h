/* ctype.h — colang 自制最小标准头 (P9.1)
 * 字符分类/转换：本批以纯函数声明提供（C §7.4 允许实现为函数或宏，函数形态可移植且无副作用）。
 * 符号由链接阶段的外部 clang → 宿主 CRT(UCRT/MSVC) 提供。
 */
#ifndef _COLANG_CTYPE_H
#define _COLANG_CTYPE_H

extern int isalnum(int c);
extern int isalpha(int c);
extern int isblank(int c);
extern int iscntrl(int c);
extern int isdigit(int c);
extern int isgraph(int c);
extern int islower(int c);
extern int isprint(int c);
extern int ispunct(int c);
extern int isspace(int c);
extern int isupper(int c);
extern int isxdigit(int c);

extern int tolower(int c);
extern int toupper(int c);

#endif /* _COLANG_CTYPE_H */
