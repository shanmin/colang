/* setjmp.h — colang 自制最小标准头 (P9.15)
 * C §7.13：setjmp / longjmp / jmp_buf。仅纳入 C 标准三件套（不做 POSIX sigsetjmp 等厂商扩展，见 ADR-104）。
 * jmp_buf 尺寸/对齐取自本机 UCRT ground-truth：sizeof(jmp_buf)=256、_Alignof=16。
 *   UCRT _setjmp 用 16 字节对齐的 movaps 存 XMM 态，缓冲区欠对齐则 longjmp 回跳访问违例。
 *   标准 C（§6.7.2.1）规定对齐说明符只适用于对象/位域，不得用于 typedef——故不能写
 *   `typedef _Alignas(16) long long jmp_buf[32];`（clang/gcc 均报错）。合规做法：以带
 *   _Alignas(16) 成员的 struct 包裹数组，再 typedef 该 struct 的数组，使每个 jmp_buf
 *   对象天然继承 struct 的 16 字节对齐（取代早期 codegen 对 [32 x i64] 落位类型的硬对齐 hack）。
 * _setjmp 是 UCRT 稳定导出符号；x64 上它需调用者帧地址作隐藏第二参数（各 C 编译器前端均注入
 *   llvm.frameaddress(0)）。colang 走 .bc 后端不经前端重写，故由 codegen 在调用点补齐——因此本头
 *   将 _setjmp 声明为 2 参（第二参 void* _Frame 仅用于对齐 IR 类型，源码层从不显式传它）；
 *   setjmp(env) 宏只传 env 一参，第二参由 codegen 追加。
 * longjmp 本身即 C 标准函数（非宏），UCRT(x64) 导出真符号 longjmp（实测无 _longjmp），无需帧地址参数。
 * 边界：面向 Windows/UCRT；glibc(Linux) 下 jmp_buf 尺寸/元素类型/ABI 均不同，待平台预定义宏后分派。
 */
#ifndef _COLANG_SETJMP_H
#define _COLANG_SETJMP_H

/* 带 _Alignas(16) 成员的 struct 包裹：令每个 jmp_buf 对象 16 字节对齐（合规 C）。 */
struct __jmp_buf_impl { _Alignas(16) long long __buf[32]; };
typedef struct __jmp_buf_impl jmp_buf[1];   /* 256 字节，继承 struct 的 16 对齐 */

extern int  _setjmp(struct __jmp_buf_impl *_Env, void *_Frame);
extern void longjmp(struct __jmp_buf_impl *_Env, int _Value);

#define setjmp(env)  _setjmp(env)   /* 第二参（帧地址）由 colang codegen 注入 */

#endif /* _COLANG_SETJMP_H */
