// ============================================================
// type_spell.h — 诊断用的**类型拼写**入口（clang 同形口径）
//
// 由 P252/P253 在 parser 内落地，P254 起提为公共头，供 parser（`_Alignas` 少诊断）
// 与 sema（`const` 只读强制诊断）**共用同一拼写**——BACKLOG §3.1「双轨拼写只服务
// _Alignas 一条」的收拢起点：新带类型诊断一律走 `describe_type`，不再各自手拼。
//
// 覆盖：标量（含 unsigned 家族）/ struct·union·enum 标签 / _BitInt /
//   const·volatile·restrict·_Atomic 限定 / typedef 别名还原 / 指针 `T *` /
//   数组 `T[N]` / 函数指针 `R (*)(P)`。
// 双轨：sugar 轨保留书写处的 typedef 别名、plain 轨一律去别名；两轨不同才补 `(aka '…')`，
//   与 clang 同形：`'MyInt' (aka 'int')`、`'int (*)(MyInt)' (aka 'int (*)(int)')`、
//   `'int *const volatile'`、`'_Atomic(int)'`、`'MyInt[4]' (aka 'int[4]')`。
// 仍不及 clang（已登 BACKLOG）：限定符写在声明符之后（`int x volatile;`）两家**均拒收**、但文本不同
//   （clang `expected ';' after top level declarator` @20、colang `expected declaration or statement` @21）。
//
// 本头**只读 `CType` 的诊断专用位**（`alias`/`is_volatile`/`is_restrict`/`is_atomic`），
//   不参与大小、对齐、布局与 `types_compatible`，故对代码生成零影响。
// ============================================================
#pragma once

#include <cstddef>
#include <string>

#include "colang/ast.h"

namespace colang
{

// P253：把使用点书写的限定符并入别名串（clang 口径：`const MyInt`、`const volatile SS`）。
//   串内顺序恒为 const → volatile；同一限定符重复书写（含别名自带 + 使用点外加）不重复外显。
inline void alias_add_qual(std::string &al, bool vol)
{
	if (al.empty()) return;
	std::string bare = al;
	const bool had_c = bare.rfind("const ", 0) == 0;
	if (had_c) bare.erase(0, 6);
	const bool had_v = bare.rfind("volatile ", 0) == 0;
	if (had_v) bare.erase(0, 9);
	if (bare.empty()) return; // 理论不可达（别名本体不会只是一个限定符）
	const bool now_c = had_c || !vol;   // vol=false 的调用即「此次加 const」
	const bool now_v = had_v || vol;
	al = (now_c ? "const " : std::string()) + (now_v ? "volatile " : std::string()) + bare;
}

// 前置限定符（clang 顺序：const → volatile）。数组/函数节点自身的限定符同样落到本体拼写前。
inline std::string qual_pre(const CType *t)
{
	std::string s;
	if (t->is_const) s += "const ";
	if (t->is_volatile) s += "volatile ";
	return s;
}

// 指针顶层限定符（贴 `*` 之后，clang 顺序：const → volatile → restrict）。
inline std::string qual_suf(const CType *t)
{
	std::string s;
	if (t->is_const) s += "const";
	if (t->is_volatile) s += s.empty() ? "volatile" : " volatile";
	if (t->is_restrict) s += s.empty() ? "restrict" : " restrict";
	return s;
}

inline std::string spell_type_at(const CType *t, bool sugar);
inline std::string spell_params(const CType &f, bool sugar);

// P254：标签拼写去掉 colang 为「同一 tag 的内联/重复定义」追加的合成后缀——
//   `union U2 { int i; } u;` 的 ctype tag 是 `U2.__blk1`，而 clang 仍打印 `union U2`。
inline std::string spell_tag(const std::string &tag)
{
	const std::size_t p = tag.find(".__blk");
	return p == std::string::npos ? tag : tag.substr(0, p);
}

// 单轨类型拼写；`sugar` = 是否保留 typedef 别名书写。裸指针入口沿 base 链递归（类型树由 AST 持有）。
inline std::string spell_type_at(const CType *t, bool sugar)
{
	if (!t) return "<unknown>";
	// 别名一名覆盖整棵子树（`SS`、`const volatile MyInt` 均已在串内）
	if (sugar && !t->alias.empty()) return t->alias;
	switch (t->kind)
	{
	case TypeKind::Array:
	{
		// P252：多维数组按声明序拼写（外维在前）——clang 打印 `int[3][4]`；
		//   沿 base 递归会把内维排到前面，得到 `int[4][3]`。
		std::string dims;
		const CType *elem = t;
		for (; elem && elem->kind == TypeKind::Array; elem = elem->base.get())
		{
			// P253：内层数组若写作别名（`typedef int ROW[4]; ROW m[3];`），由该别名一名覆盖
			if (sugar && elem != t && !elem->alias.empty()) break;
			// 未定维/柔性数组：clang 打印 `int[]`；VLA 无编译期拼写可用，置空维
			std::string dim = (elem->is_vla || elem->array_size == 0) ? std::string() :
				std::to_string(elem->array_size);
			dims += "[" + dim + "]";
		}
		// clang：`const int arr[4]` → `const int[4]`（限定符贴最外，等价于落到元素拼写前）
		return qual_pre(t) + spell_type_at(elem, sugar) + dims;
	}
	case TypeKind::Pointer:
	{
		const CType *b = t->base.get();
		// 函数指针需把 `*` 嵌进参数括号内：`int (*)(void)`（clang 同形）；
		//   pointee 若写作别名（`typedef int F(int); F *p;`），clang 保留别名形 `F *`
		if (b && b->kind == TypeKind::Function && !t->is_atomic &&
			!(sugar && !b->alias.empty()))
			return spell_type_at(b->base.get(), sugar) + " (*)(" + spell_params(*b, sugar) + ")";
		std::string s = spell_type_at(b, sugar) + " *" + qual_suf(t);
		if (t->is_atomic) s = "_Atomic(" + s + ")"; // clang：`int *_Atomic` → `_Atomic(int *)`
		return s;
	}
	case TypeKind::Function:
		return qual_pre(t) + spell_type_at(t->base.get(), sugar) + " (" + spell_params(*t, sugar) + ")";
	default:
		break;
	}
	std::string core;
	switch (t->kind)
	{
	case TypeKind::Void: core = "void"; break;
	case TypeKind::Bool: core = "_Bool"; break;
	case TypeKind::Char: core = "char"; break;
	case TypeKind::Char8: core = "char8_t"; break;
	case TypeKind::Char16: core = "char16_t"; break;
	case TypeKind::Char32: core = "char32_t"; break;
	case TypeKind::WChar: core = "wchar_t"; break;
	case TypeKind::Short: core = t->is_unsigned ? "unsigned short" : "short"; break;
	case TypeKind::Int: core = t->is_unsigned ? "unsigned int" : "int"; break;
	case TypeKind::Long: core = t->is_unsigned ? "unsigned long" : "long"; break;
	case TypeKind::LongLong: core = t->is_unsigned ? "unsigned long long" : "long long"; break;
	case TypeKind::Float: core = "float"; break;
	case TypeKind::Double: core = "double"; break;
	case TypeKind::Enum: core = "enum " + spell_tag(t->tag); break;
	case TypeKind::Struct: core = "struct " + spell_tag(t->tag); break;
	case TypeKind::Union: core = "union " + spell_tag(t->tag); break;
	case TypeKind::BitInt:
		core = std::string(t->is_unsigned ? "unsigned _BitInt(" : "signed _BitInt(") +
			std::to_string(t->bit_width) + ")";
		break;
	default: core = "<type>"; break;
	}
	// clang：`_Atomic` 以 `_Atomic(T)` 包裹类型本体，其余限定符仍在其外（`volatile _Atomic(int)`）
	return qual_pre(t) + (t->is_atomic ? "_Atomic(" + core + ")" : core);
}

// 参数列表拼写（函数/函数指针类型用）。colang 不区分 `(void)` 与 `()`，
//   按常见原型形取 `void`（与 clang 对 `int (*)(void)` 的拼写一致）。
// P253：参数内的别名**不单独补 aka**——clang 把 aka 只给在整个类型拼写的最外层，故此处用单轨。
inline std::string spell_params(const CType &f, bool sugar)
{
	std::string s;
	for (std::size_t i = 0; i < f.params.size(); ++i)
	{
		if (i) s += ", ";
		s += spell_type_at(f.params[i].get(), sugar);
	}
	if (f.is_variadic) s += s.empty() ? std::string("...") : std::string(", ...");
	if (s.empty()) s = "void";
	return s;
}

// 双轨入口：带引号的 clang 形类型拼写（两轨相同则不补 aka）。
inline std::string describe_type(const CTypePtr &t)
{
	const std::string sugar = spell_type_at(t.get(), true);
	const std::string plain = spell_type_at(t.get(), false);
	if (sugar == plain) return "'" + sugar + "'";
	// P254：匿名记录的 de-sugar 形 clang 印作 `struct (anonymous struct at 文件:行:列)`，
	//   而对 `typedef struct {…} S;` 这类「别名→匿名记录」**不补 aka**（探针 12/40/42 例实测）
	//   → colang 同样只给 sugar 形（合成 tag `__anon_struct_N` 不得外泄到诊断）。
	if (plain.find("__anon_") != std::string::npos) return "'" + sugar + "'";
	return "'" + sugar + "' (aka '" + plain + "')";
}

}  // namespace colang
