// ============================================================
// link.h — 进程内链接（P0.3.0 自研链接器：内嵌 lld COFF）
//
// 用 LLVM 自带的 lld::coff::link 在进程内把 COFF .obj 链成 exe 或 dll，
// 不再 shell out 外部 clang.exe / link.exe。
// 目标平台：x86_64-pc-windows-msvc（exe：静态 CRT / 控制台子系统；dll：导出表）。
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
// 16MB 栈；子系统/入口由 libcmt 的 .drectve 提供）。
// 成功返回 true；失败把 lld 的诊断追加到 errors 并返回 false。
bool link_exe(const std::vector<std::string> &objs, const std::string &out_path,
              std::vector<Error> &errors);

}  // namespace colang
