/* fenv.h — colang 自制最小标准头 (P9.19)
 * C §7.6：浮点环境。用户定案（ADR-104 相关）：#pragma STDC FENV_ACCESS 采「no-op 桩口径」——
 *   colang 预处理器对非 once 的 #pragma 一律静默忽略（见 preproc.cpp handle_pragma），故该 pragma
 *   无需前端支持即天然被忽略；而 fe* 函数本身**直连宿主 UCRT 真实现**（非桩）。本机实测：
 *   fesetround/fegetround/feclearexcept/fesetenv(FE_DFL_ENV) 均可链可跑（clang 探针 RC=0）。
 * 类型/常量取自本机 UCRT/MSVC ground-truth：
 *   fenv_t = struct{unsigned long _Fe_ctl,_Fe_stat}=8B；fexcept_t = unsigned long=4B；
 *   舍入位段 _RC_NEAR=0/_RC_DOWN=0x100/_RC_UP=0x200/_RC_CHOP=0x300（映射 FE_TONEAREST/…）；
 *   异常位 _SW_INEXACT=0x1/_SW_UNDERFLOW=0x2/_SW_OVERFLOW=0x4/_SW_ZERODIVIDE=0x8/_SW_INVALID=0x10；
 *   UCRT 默认环境 _Fenv1 = {0x3f00003f, 0}（selectany，每 TU 内联定义，故此处以文件级 static 同值复刻）。
 * colang 表示选择：C 标准把 fenv_t 定义为「不透明对象类型」，仅约束其可作指针/取址/ sizeof；
 *   colang 结构体按 tag 查布局、匿名 struct typedef 支持有限，故以等宽 unsigned long long(8B) 承载 fenv_t
 *   （小端下 {0x3f00003f,0} 即整数值 0x3f00003f），与宿主 ABI 逐字节一致；用户从不触碰其内部字段。
 * 边界：面向 Windows/UCRT 宿主；FP_IEC_60559 等可选宏、glibc(Linux) fenv_t 布局另议。
 */
#ifndef _COLANG_FENV_H
#define _COLANG_FENV_H

/* fenv_t / fexcept_t：尺寸对齐本机 UCRT（8B / 4B）。fenv_t 采等宽不透明整数表示（见上方注释）。 */
typedef unsigned long long fenv_t;
typedef unsigned long fexcept_t;

/* —— 舍入方向宏（值对齐本机 UCRT 控制字位段，见上方 ground-truth）—— */
#define FE_TONEAREST   0x00000000
#define FE_DOWNWARD    0x00000100
#define FE_UPWARD      0x00000200
#define FE_TOWARDZERO  0x00000300

/* —— 异常标志宏（值对齐本机 UCRT _SW_* 位段）—— */
#define FE_INEXACT     0x00000001
#define FE_UNDERFLOW   0x00000002
#define FE_OVERFLOW    0x00000004
#define FE_DIVBYZERO   0x00000008
#define FE_INVALID     0x00000010
#define FE_ALL_EXCEPT  (FE_DIVBYZERO | FE_INEXACT | FE_INVALID | FE_OVERFLOW | FE_UNDERFLOW)

/* —— 默认环境 ——
 * 标准 FE_DFL_ENV 需为 const fenv_t*。UCRT 以 selectany 全局 _Fenv1={_Fe_ctl=0x3f00003f,_Fe_stat=0}
 * 实现；colang 无 selectany，改用文件级 static（内部链接，每个含本头的 TU 各持一份，无重复符号）
 * 以等宽整数承载同值：小端下 {0x3f00003f,0} == 0x3f00003fULL。 */
static fenv_t _colang_dfl_env = 0x3f00003f;
#define FE_DFL_ENV  (&_colang_dfl_env)

/* —— 异常管理（符号由宿主 UCRT 提供）—— */
extern int feclearexcept(int excepts);
extern int fegetexceptflag(fexcept_t *flagp, int excepts);
extern int feraiseexcept(int excepts);
extern int fesetexceptflag(const fexcept_t *flagp, int excepts);
extern int fetestexcept(int excepts);

/* —— 舍入方向 —— */
extern int fegetround(void);
extern int fesetround(int rounding_mode);

/* —— 整体环境存取 —— */
extern int fegetenv(fenv_t *envp);
extern int feholdexcept(fenv_t *envp);
extern int fesetenv(const fenv_t *envp);
extern int feupdateenv(const fenv_t *envp);

#endif /* _COLANG_FENV_H */
