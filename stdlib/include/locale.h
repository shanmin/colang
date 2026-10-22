/* locale.h — colang 自制最小标准头
 * C §7.11 本地化。类别宏取本机 UCRT ground-truth 数值（LC_ALL=0 .. LC_TIME=5）。
 * struct lconv 字段布局按 C 标准顺序；实际使用 localeconv() 的代码多为指针访问。
 * 边界：面向 Windows/UCRT 宿主。 */
#ifndef _COLANG_LOCALE_H
#define _COLANG_LOCALE_H

#include <stddef.h>

#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5

#define LC_MIN  LC_ALL
#define LC_MAX  LC_TIME

struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char  int_frac_digits;
    char  frac_digits;
    char  p_cs_precedes;
    char  p_sep_by_space;
    char  n_cs_precedes;
    char  n_sep_by_space;
    char  p_sign_posn;
    char  n_sign_posn;
    char  int_p_cs_precedes;
    char  int_p_sep_by_space;
    char  int_n_cs_precedes;
    char  int_n_sep_by_space;
    char  int_p_sign_posn;
    char  int_n_sign_posn;
};

extern char *setlocale(int category, const char *locale);
extern struct lconv *localeconv(void);

#endif /* _COLANG_LOCALE_H */
