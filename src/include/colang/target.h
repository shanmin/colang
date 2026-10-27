// ============================================================
// target.h — 目标平台 / 数据模型（data model）单一真值源
//
// 本层刻意不含任何 LLVM 依赖（POD 描述符），供预处理层与语法层共享：
// 由驱动层（driver/target.cpp，链 LLVM）依三元组解析填充后，经 CompileOptions
// 下传，统一驱动——预定义宏、类型宽度与对齐、DataLayout、链接后端——消除
// 过去「前端写死 Windows/LLP64、codegen 却按 host 三元组选 DataLayout」的分裂。
//
// 默认构造即 Windows/LLP64（long=4、wchar_t=2、指针=8），保证 Windows 主机
// 不带 --target 时行为逐字恒等。
// ============================================================
#pragma once

#include <string>

namespace colang
{

// 一次编译请求的目标描述符（值语义、可拷贝、不含 LLVM 类型）。
struct TargetInfo
{
	std::string triple;      // 解析后的规范三元组（供 codegen setTargetTriple）

	// OS 归类：Windows 与 Linux 二选一（当前仅这两种目标）。
	bool is_windows = true;
	bool is_linux = false;

	// —— 数据模型驱动的标量类型宽度/对齐（字节）——
	// Windows/LLP64：long=4、wchar_t=2；Linux/LP64：long=8、wchar_t=4；指针恒 8（x86-64）。
	int sizeof_long = 4;
	int alignof_long = 4;
	int sizeof_wchar = 2;
	bool wchar_signed = false; // wchar_t 符号性：Linux 有符号、Windows 无符号（默认 Win=false 保恒等）
	int sizeof_pointer = 8;
};

// 依三元组解析目标描述符。triple_or_empty 为空时取宿主默认三元组。
// 实现在 driver/target.cpp（链 LLVM，用 llvm::Triple 判 OS/架构）。
TargetInfo resolve_target(const std::string &triple_or_empty);

}  // namespace colang
