// ============================================================
// link.h — 进程内链接（P0.3.0 自研链接器：内嵌 lld COFF）
//
// 用 LLVM 自带的 lld::coff::link 在进程内把 COFF .obj 链成 exe 或 dll，
// 不再 shell out 外部 clang.exe / link.exe。
// 目标平台：x86_64-pc-windows-msvc（exe：静态 CRT / 默认控制台子系统、可切 Windows GUI；dll：导出表）。
// ============================================================
#pragma once

#include <string>
#include <vector>

#include "colang/common.h"

namespace colang
{

// 把 objs（COFF .obj 路径）链接为 out_path。
// out_path 以 .dll 结尾时产出动态库（导出表来自 public 符号的 /EXPORT .drectve）；
// 否则产出可执行文件。
// exe 复刻 clang 现供的链接参数（静态 CRT：libcmt+oldnames+legacy_stdio_definitions，
// 16MB 栈；默认子系统/入口由 libcmt 的 .drectve 提供）。
// P229：windows_subsystem=true → 传 -subsystem:windows（覆盖 .drectve 的控制台）；
//   use_winmain=true → 传 -entry:WinMainCRTStartup（覆盖 .drectve 的 mainCRTStartup）。
// PE 资源（档②）：res_files 为用户自备的预编译 .res 路径，直接喂 lld COFF（原生转 .rsrc 段嵌入）；
//   exe/dll 通用；空则不嵌入。非 Windows 构建（link_exe stub）忽略此参。
// 成功返回 true；失败把 lld 的诊断追加到 errors 并返回 false。
bool link_exe(const std::vector<std::string> &objs, const std::string &out_path,
              bool windows_subsystem, bool use_winmain,
              const std::vector<std::string> &res_files,
              std::vector<Error> &errors);

// 把 objs（ELF .o 路径）静态链接为 out_path（Linux 原生，P2xx 最小闭环）。
// 进程内 lld::elf：ld.lld -static，喂 crt1.o/crti.o + objs + -lc + crtn.o，
// crt/libc.a 由内部发现器按 env(COLANG_CRT_DIR/COLANG_LINUX_SYSROOT) → Debian 多架构
// → /usr/lib64 → /usr/lib 优先级定位；缺件给清晰诊断。
// 成功返回 true；失败把 lld 诊断追加到 errors 并返回 false。
// 非 Windows 构建为真实实现；Windows 构建（仅编入 lldCOFF）返回诊断的 stub。
bool link_elf(const std::vector<std::string> &objs, const std::string &out_path,
              std::vector<Error> &errors);

}  // namespace colang
