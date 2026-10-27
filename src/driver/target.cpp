// ============================================================
// target.cpp — 依三元组解析 TargetInfo（链 LLVM，见 target.h 设计说明）
//
// 这是"单一 target 源"的解析点：驱动层调用一次，结果经 CompileOptions 下传
// 预处理/语法/代码生成/链接，保证各层对 OS 与数据模型的判断逐字一致。
// 本 TU 可自由依赖 LLVM（driver 层已链 LLVM）。
// ============================================================

#include "colang/target.h"

#include <llvm/TargetParser/Host.h>   // llvm::sys::getDefaultTargetTriple
#include <llvm/TargetParser/Triple.h>

namespace colang
{

TargetInfo resolve_target(const std::string &triple_or_empty)
{
	TargetInfo ti;

	// 空串 → 宿主默认三元组（Linux 主机上 colang 原生构建即得 linux 三元组）；
	// 非空 → 显式 --target（Windows 主机上亦可交叉预检前端数据模型）。
	const std::string raw = triple_or_empty.empty()
		? llvm::sys::getDefaultTargetTriple()
		: triple_or_empty;
	const llvm::Triple T(raw);
	ti.triple = T.str();

	const llvm::Triple::OSType os = T.getOS();
	ti.is_windows = (os == llvm::Triple::Win32);   // Win32 枚举涵盖 32/64 位 Windows
	ti.is_linux = (os == llvm::Triple::Linux);
	if (!ti.is_windows && !ti.is_linux)
		ti.is_windows = true; // 未知 OS 保守归 Windows（与既有默认恒等）

	const bool is64 = T.isArch64Bit();
	ti.sizeof_pointer = is64 ? 8 : 4;

	if (ti.is_linux)
	{
		// LP64：long=8、指针=8；32 位则 ILP32（long=4）。glibc wchar_t = int（4 字节）。
		ti.sizeof_long = is64 ? 8 : 4;
		ti.alignof_long = ti.sizeof_long;
		ti.sizeof_wchar = 4;
		ti.wchar_signed = true; // glibc wchar_t = int（有符号 32 位）
	}
	else // Windows：LLP64——long 恒 4（不随字长涨）、wchar_t = unsigned short（2 字节）。
	{
		ti.sizeof_long = 4;
		ti.alignof_long = 4;
		ti.sizeof_wchar = 2;
		ti.wchar_signed = false; // MSVC wchar_t = unsigned short（无符号 16 位）
	}

	return ti;
}

}  // namespace colang
