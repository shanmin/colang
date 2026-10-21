/* limits.h — colang 自制最小标准头 (P9.0)
 * 宿主环境整型/字符极限：按 LLP64(Windows x86_64)，char=1 short=2 int=4 long=4 long long=8。
 * 仅列常用宏；浮点极限(__*_MAX__)本批省略（前端未涉及浮点极限求值）。
 */
#ifndef _COLANG_LIMITS_H
#define _COLANG_LIMITS_H

#define CHAR_BIT     8
#define SCHAR_MIN    (-128)
#define SCHAR_MAX    127
#define UCHAR_MAX    255
#define CHAR_MIN     SCHAR_MIN
#define CHAR_MAX     SCHAR_MAX

#define SHRT_MIN     (-32768)
#define SHRT_MAX     32767
#define USHRT_MAX    65535

#define INT_MIN      (-2147483647 - 1)
#define INT_MAX      2147483647
#define UINT_MAX     4294967295U

#define LONG_MIN     (-2147483647L - 1)
#define LONG_MAX     2147483647L
#define ULONG_MAX    4294967295UL

#define LLONG_MIN    (-9223372036854775807LL - 1)
#define LLONG_MAX    9223372036854775807LL
#define ULLONG_MAX   18446744073709551615ULL

#define MB_LEN_MAX   5

#endif /* _COLANG_LIMITS_H */
