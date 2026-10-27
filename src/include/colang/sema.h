// ============================================================
// sema.h — 语义分析（P3 落地）
//
// 类型检查、标准转换（整型提升 / usual arithmetic conversions /
// 隐式指针转换）、作用域与链接、约束项检查（6.3 各条）。
// 是 C 语义正确性的主战场，逐条对照标准实现。
// ============================================================
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "colang/ast.h"
#include "colang/driver.h"

namespace colang
{

// ============================================================
// 路 B 阶段1b：ModuleCatalog —— 逐模块「自有 public 顶层实体」裸名→AST 指针五表。
//
// 供 run_host() 建 public 视图、限定访问 m.f()/m.x 绑定、撞名分流①~⑧裁定。
// 「自有」＝源项 source_module_key 归属本模块（含其 #include 带入的原型不入导出面，
//   即 ambient 切断在导出侧的体现：catalog 只收模块显式声明的 public 顶层实体）。
// 指针指向合并后主 TU 的 AST 内部（parse 后 vector 不再重分配、稳定）。
// ============================================================
struct ModuleCatalog
{
	// 单模块 public 视图（五表；缺项类型不出现在对应表）
	struct View
	{
		std::unordered_map<std::string, const FuncDecl *> funcs;   // 裸名 → public 函数
		std::unordered_map<std::string, const GlobalVar *> globals; // 裸名 → public 全局变量
		std::unordered_map<std::string, const StructDef *> structs;  // tag → public struct/union 定义
		std::unordered_map<std::string, const EnumDef *> enums;      // tag → public enum 定义
		std::unordered_map<std::string, std::int64_t> consts;        // 名 → public const int
	};
	// module key（规范化绝对路径）→ 本模块 public 视图
	std::unordered_map<std::string, View> by_module;
	// 限定访问名（默认文件名 stem / `as ModAlias`）→ module key；供 m.f()/m.x 解析模块归属
	std::unordered_map<std::string, std::string> qualified;
};

// 语义分析：对 AST 做类型检查与规范化。接口为框架占位，实现时细化。
// 旧单遍路径（.c、无 import 的 .co）走本入口。
bool analyze(const CompileOptions &opts, CompileState &state, std::vector<Error> &errors);

// 路 B 阶段1b：per-module 转正路径入口（默认为 `.co` 含 import 的新时序）。
//   建 ModuleCatalog + 逐模块闭包 + 宿主 public 视图 + 撞名分流 + 限定访问绑定。
bool analyze_per_module(const CompileOptions &opts, CompileState &state, std::vector<Error> &errors,
                        ModuleCatalog *out_catalog = nullptr);

}  // namespace colang
