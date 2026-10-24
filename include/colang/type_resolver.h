// ============================================================
// type_resolver.h — 表达式类型解析单一真值源（P140 M1）
//
// 背景：历史上表达式类型解析在三处并存且逻辑重复——
//   - parser::infer_auto_type（auto/typeof，仅字面量+指针，多数回退 int）
//   - sema::resolve_type（validate-only，算术/指针刻意返回 null）
//   - codegen::get_expr_ctype（最全：整型提升/常用算术转换/stdbit/stdckdint/成员/返回类型）
// 收敛路线（用户定案：抽共享解析器）：把最完备的解析算法收归此处的单一
//   resolve_expr_type，喂入符号表接口（模板 Env，零开销），三处逐里程碑挂载。
//
// 本文件为 header-only，仅依赖 ast.h（CType/Expr 模型 + make_* 工厂），
//   绝不触及 LLVM——纯函数只读写 CType。
//
// 里程碑：M1 仅 codegen 挂载（get_expr_ctype 恒等委托，行为逐字不变）；
//   M2 挂 sema::resolve_type、M3 挂 parser::infer_auto_type 留后续。
// ============================================================
#pragma once

#include <string>

#include "colang/ast.h"

namespace colang
{

// ============================================================
// 纯类型工具（不依赖符号表 / LLVM）——自 codegen 逐字迁入
// ============================================================

// 整型提升后的整数描述：位宽 + 符号
struct IntPromo { unsigned bits; bool is_unsigned; };

// 按 C 类型求提升后的整型描述（小于 int 者归入 signed int——int 可表其全部值）
inline IntPromo int_promo_of(const CTypePtr &t)
{
	if (!t) return {32, false};
	switch (t->kind)
	{
	case TypeKind::Bool: case TypeKind::Char: case TypeKind::Short: case TypeKind::WChar:
	case TypeKind::Char8: case TypeKind::Char16:
		return {32, false};
	case TypeKind::Char32:  // uint32 值域 int 表不下 → 提升为 unsigned int
		return {32, true};
	case TypeKind::Int: case TypeKind::Long: case TypeKind::Enum:
		return {32, t->is_unsigned};
	case TypeKind::LongLong:
		return {64, t->is_unsigned};
	case TypeKind::BitInt:
		return {t->bit_width ? t->bit_width : 32, t->is_unsigned};
	default:
		return {32, false};
	}
}

// 常用算术转换（§6.3.2.8）：等宽时混合/双无符号→无符号；不等宽由更宽者定符号
inline IntPromo int_uac(const IntPromo &a, const IntPromo &b)
{
	if (a.bits != b.bits)
		return a.bits > b.bits ? a : b;
	return { a.bits, a.is_unsigned || b.is_unsigned };
}

// 由提升后位宽+符号回构 CType（供报告复合表达式结果类型）
// >32 一律归 long long（宁可偏宽也不下截断；无 40 位普通整型）
inline CTypePtr int_ctype_of(unsigned bits, bool uns)
{
	TypeKind k = (bits > 32) ? TypeKind::LongLong
	           : (bits == 32) ? TypeKind::Int
	           : (bits >= 16) ? TypeKind::Short
	                          : TypeKind::Char;
	auto t = make_type(k);
	t->is_unsigned = uns;
	return t;
}

// 类型兼容性判定（C11 §6.5.1.1 _Generic 关联匹配用，环境无关）。
//   限定符在每一层都比较：依赖 lvalue 转换已将控制表达式顶层 const 剥除，
//   使 `const int:` 关联永不命中、`int:` 命中；而指向/元素 const 显著（`int*` ≠ `const int*`）。
inline bool types_compatible(const CTypePtr &a, const CTypePtr &b)
{
	if (!a || !b) return false;
	if (a->kind != b->kind) return false;
	if (a->is_const != b->is_const) return false;
	switch (a->kind)
	{
	case TypeKind::Pointer:
	case TypeKind::Array:
		return types_compatible(a->base, b->base); // 数组按元素兼容（忽略维度）
	case TypeKind::Function:
		if (a->is_variadic != b->is_variadic) return false;
		if (!types_compatible(a->base, b->base)) return false; // 返回类型
		if (a->params.size() != b->params.size()) return false;
		for (std::size_t i = 0; i < a->params.size(); ++i)
			if (!types_compatible(a->params[i], b->params[i])) return false;
		return true;
	case TypeKind::Struct:
	case TypeKind::Union:
	case TypeKind::Enum:
		return a->tag == b->tag;
	case TypeKind::BitInt:
		return a->bit_width == b->bit_width && a->is_unsigned == b->is_unsigned;
	default:
		return a->is_unsigned == b->is_unsigned; // 基本整/浮：符号一致
	}
}

// 位域存储单元位宽（根据基本类型确定）
inline unsigned get_bitfield_unit_bits(const CTypePtr &t)
{
	if (!t) return 32;
	switch (t->kind) {
	case TypeKind::Char: return 8;
	case TypeKind::Bool: return 8;
	case TypeKind::Char8: return 8;
	case TypeKind::Char16: return 16;
	case TypeKind::Char32: return 32;
	case TypeKind::Short: return 16;
	case TypeKind::Int: return 32;
	case TypeKind::Long: return 32;   // Windows long=32
	case TypeKind::LongLong: return 64;
	default: return 32;
	}
}

// <stdbit.h>（§7.18）：解析 stdc_<op>[_uc|_us|_ui|_ul|_ull]，返回后缀固定宽度（0=类型通用），输出 op 名 core
inline unsigned stdbit_parse(const std::string &name, std::string &core)
{
	if (name.rfind("stdc_", 0) != 0) { core = name; return 0; }
	static const struct { const char *sfx; unsigned bits; } tab[] =
		{{"_ull",64},{"_ul",32},{"_ui",32},{"_us",16},{"_uc",8}};
	core = name.substr(5);
	for (const auto &s : tab)
	{
		std::string t = s.sfx;
		if (core.size() > t.size() && core.compare(core.size()-t.size(), t.size(), t) == 0)
		{ core = core.substr(0, core.size()-t.size()); return s.bits; }
	}
	return 0;
}

// bit_floor/bit_ceil 返回同宽无符号值；其余计数类返回 unsigned int
inline bool stdbit_is_value_op(const std::string &core)
{
	return core == "bit_floor" || core == "bit_ceil";
}

inline CTypePtr stdbit_unsigned_ty(unsigned W)
{
	CTypePtr t;
	if (W <= 8) t = make_type(TypeKind::Char);
	else if (W <= 16) t = make_type(TypeKind::Short);
	else if (W <= 32) t = make_type(TypeKind::Int);
	else t = make_type(TypeKind::LongLong);
	t->is_unsigned = true;
	return t;
}

// ============================================================
// 符号表接口约定（模板 Env，编译期多态，零开销）
//   Env 须提供以下三个成员函数：
//     CTypePtr var_type(const std::string &name);
//         —— 查标识符类型（合并局部变量 + 全局变量），未找到返回 nullptr
//     CTypePtr struct_member(const std::string &tag, const std::string &field);
//         —— 按结构体 tag 查成员 field 的类型，未找到返回 nullptr
//     CTypePtr func_return(const std::string &name);
//         —— 查函数返回类型，未找到返回 nullptr
//   各消费方（codegen/sema/parser）以自身符号表实现 Env，实现单一算法复用。
// ============================================================

// 表达式类型解析（自 codegen::get_expr_ctype 逐字搬迁，行为恒等）
template <class Env>
CTypePtr resolve_expr_type(const ExprPtr &e, Env &env)
{
	if (!e) return nullptr;
	if (e->kind == ExprKind::VarRef)
	{
		return env.var_type(e->name);
	}
	// 复合字面量 (type){...}：类型由 parser 直接写入 InitList::type；
	// 缺失此项会让成员访问/取址解析拿不到基础类型，进而按空 tag 造出 opaque struct
	// （GEP into unsized type）。
	if (e->kind == ExprKind::InitList)
	{
		return e->type;
	}
	if (e->kind == ExprKind::Member)
	{
		// 获取基础表达式类型 → 查找成员名对应类型
		CTypePtr base_t = resolve_expr_type(e->lhs, env);
		if (!base_t) return nullptr;
		// 如果 base 是指针（对于 -> 操作）取 pointed-to；数组经 -> 亦按衰减为元素指针处理（P173：sizeof(cases->c)）
		if (e->op == "->" && (base_t->kind == TypeKind::Pointer || base_t->kind == TypeKind::Array))
			base_t = base_t->base;
		// 按 tag 查找成员类型
		return env.struct_member(base_t->tag, e->name);
	}
	if (e->kind == ExprKind::Unary && e->op == "*")
	{
		CTypePtr pt = resolve_expr_type(e->lhs, env);
		// 数组在 * 下按衰减后的指针处理（§6.5.3.2p3）：*arr 是「少一维的数组」，类型为其元素。
		if (pt && (pt->kind == TypeKind::Pointer || pt->kind == TypeKind::Array)) return pt->base;
		return nullptr;
	}
	if (e->kind == ExprKind::Unary && e->op == "&")
	{
		// &expr → Pointer{type_of_expr}
		CTypePtr vt = resolve_expr_type(e->lhs, env);
		if (vt) return make_pointer(vt);
		return make_pointer(make_type(TypeKind::Void));
	}
	if (e->kind == ExprKind::Binary && e->op == "[]")
	{
		CTypePtr at = resolve_expr_type(e->lhs, env);
		if (at && at->kind == TypeKind::Array) return at->base;
		if (at && at->kind == TypeKind::Pointer) return at->base;
		return nullptr;
	}
	if (e->kind == ExprKind::Cast)
		return e->type;
	if (e->kind == ExprKind::Unary)
	{
		// sizeof/alignof 结果 = size_t（LLP64 无符号 64）；* 与 & 已在上方提前返回
		if (e->op == "sizeof" || e->op == "alignof")
			return int_ctype_of(64, true);
		if (e->op == "!")
			return make_type(TypeKind::Int);
		if (e->op == "-" || e->op == "+" || e->op == "~")
		{
			IntPromo p = int_promo_of(resolve_expr_type(e->lhs, env));
			return int_ctype_of(p.bits, p.is_unsigned);
		}
		// ++/--（prefix/postfix）结果类型 = 操作数类型（C：后缀值为自增前的旧值，类型同操作数）。
		// 缺此项会使 *from++ 的解引用取不到指针类型而误落默认 i32（窄指针越界）。
		if (e->op == "++" || e->op == "--")
			return resolve_expr_type(e->lhs, env);
		return nullptr;
	}
	if (e->kind == ExprKind::Binary)
	{
		// 非 [] 的二元表达式结果类型（[] 已在上方处理并返回）
		const std::string &bop = e->op;
		CTypePtr lt = resolve_expr_type(e->lhs, env);
		CTypePtr rt = resolve_expr_type(e->rhs, env);
		if (bop == ",") return rt;
		// 指针参与：ptr±int / int+ptr 结果为**指针类型**（保留点指 base），其余（ptr-ptr/比较等）
		// 维持旧行为返回 nullptr 由 codegen 指针路径据 LLVM 类型处理。
		// P184：缺此项使 *(p+1) 的解引用取不到点指类型、load 宽度塌到默认 i32
		// （float/double 数组经指针算术解引用读出垃圾位模式、CTS G 族；int 恰为 i32 故幸免）。
		bool l_ptr = lt && (lt->kind == TypeKind::Pointer || lt->kind == TypeKind::Array);
		bool r_ptr = rt && (rt->kind == TypeKind::Pointer || rt->kind == TypeKind::Array);
		if (l_ptr || r_ptr) {
			if ((bop == "+" || bop == "-") && l_ptr != r_ptr) {
				const CTypePtr &pt = l_ptr ? lt : rt;         // 指针操作数
				if (pt->kind == TypeKind::Pointer) return pt;
				return make_pointer(pt->base);               // 数组名参与算术 → 衰减为元素指针
			}
			return nullptr;
		}
		if (bop == "<<" || bop == ">>")
		{
			IntPromo lp = int_promo_of(lt);
			return int_ctype_of(lp.bits, lp.is_unsigned);
		}
		if (bop == "==" || bop == "!=" || bop == "<" || bop == ">" ||
		    bop == "<=" || bop == ">=" || bop == "&&" || bop == "||")
			return make_type(TypeKind::Int);
		if (bop == "+" || bop == "-" || bop == "*" || bop == "/" || bop == "%" ||
		    bop == "&" || bop == "|" || bop == "^")
		{
			IntPromo res = int_uac(int_promo_of(lt), int_promo_of(rt));
			return int_ctype_of(res.bits, res.is_unsigned);
		}
		return nullptr;
	}
	// 赋值表达式的结果类型 = 左值类型（C §6.5.16）；复合赋值同理（ptr += 2 → 指针）。
	//   缺此项使 (obj1 = obj2).array[3]、(ptr1 = ptr2)->array[3] 取不到结构体 tag，
	//   成员访问按空 tag 造出 opaque %struct.（GEP into unsized type，CTS 0044/0054 族）。
	if (e->kind == ExprKind::Assign)
	{
		return resolve_expr_type(e->lhs, env);
	}
	// 逗号表达式结果类型 = 最后一个子表达式（§6.5.17）；空列表回退 nullptr。
	if (e->kind == ExprKind::Comma)
	{
		if (e->args.empty()) return nullptr;
		return resolve_expr_type(e->args.back(), env);
	}
	// 三元表达式（§6.5.15）：两臂同 tag 的 struct/union → 该类型；同型指针 → 该指针；
	//   其余仅在两臂类型兼容时采纳首臂，否则维持旧 nullptr 行为（最小 blast radius）。
	if (e->kind == ExprKind::Ternary)
	{
		CTypePtr a = e->args.size() > 0 ? resolve_expr_type(e->args[0], env) : nullptr;
		CTypePtr b = e->args.size() > 1 ? resolve_expr_type(e->args[1], env) : nullptr;
		if (!a) return b;
		if (!b) return a;
		if (a->kind == TypeKind::Struct || a->kind == TypeKind::Union)
			return (a->kind == b->kind && !a->tag.empty() && a->tag == b->tag) ? a : nullptr;
		if (types_compatible(a, b)) return a;
		return nullptr;
	}
	if (e->kind == ExprKind::Call)
	{
		// va_arg(ap, T)：结果类型由 parser 直接写入 e->type；成员访问/取址需据此解析
		//   （缺失时 (va_arg(ap, test_t)).i 会按空 tag 造出 opaque %struct.，CTS 0043 族）。
		if (e->name == "va_arg" && e->type) return e->type;
		// C23 stdckdint 内建返回 bool（此处以 i32 0/1 承载）：报告 Int 供转换使用
		if (e->name == "ckd_add" || e->name == "ckd_sub" || e->name == "ckd_mul")
			return make_type(TypeKind::Int);
		// C23 stdbit 内建：value-op(bit_floor/bit_ceil)→同宽无符号；其余计数/has_single_bit→unsigned int
		if (e->name.rfind("stdc_", 0) == 0)
		{
			std::string core;
			unsigned fw = stdbit_parse(e->name, core);
			unsigned W = fw;
			if (!W && !e->args.empty()) W = get_bitfield_unit_bits(resolve_expr_type(e->args[0], env));
			if (stdbit_is_value_op(core)) return stdbit_unsigned_ty(W);
			return stdbit_unsigned_ty(32);
		}
		// 间接调用：callee 是表达式（如 go()()、s->fp()、funcs[i]()）。
		//   取被调表达式类型，若为 Pointer(Function) → 返回其 Function 的返回类型；
		//   无法干净解析时维持旧 nullptr 行为（最小 blast radius，不扰其它间接调用的 codegen 指针路径）。
		if (e->name.empty() && e->lhs)
		{
			CTypePtr ct = resolve_expr_type(e->lhs, env);
			if (ct && ct->kind == TypeKind::Pointer && ct->base && ct->base->kind == TypeKind::Function)
				return ct->base->base;
			if (ct && ct->kind == TypeKind::Function)
				return ct->base;
			return nullptr;
		}
		// 函数调用: 查返回类型；未命中时把同名形参（函数类型参数，如 `int f4(union t f(void))`）
		//   按间接调用处理，取其返回类型（缺此项 f().i 的成员访问拿不到 tag，CTS 0169 族）。
		if (CTypePtr rt = env.func_return(e->name))
			return rt;
		if (CTypePtr vt = env.var_type(e->name))
		{
			if (vt->kind == TypeKind::Function) return vt->base;
			if (vt->kind == TypeKind::Pointer && vt->base && vt->base->kind == TypeKind::Function)
				return vt->base->base;
		}
		return nullptr;
	}
	if (e->kind == ExprKind::StringLiteral)
	{
		// 前缀决定字面量元素类型（C §6.4.5）：L/u→wchar_t*(i16)、U→char32_t*(i32)、其余→char*。
		// 与 codegen::gen_expr 右值三路编码发射同口径（均据 co_str_lit_width），使 wchar_t *p = L"…"
		// 与 MessageBoxW(L"…", …) 等宽字符实参在 Sema 通过。旧行为恒报 char*（忽略前缀、与右值发射不一致）。
		const int w = co_str_lit_width(e->str_val);
		if (w == 2) return make_pointer(make_type(TypeKind::WChar));
		if (w == 4) return make_pointer(make_type(TypeKind::Char32));
		return make_pointer(make_type(TypeKind::Char));
	}
	if (e->kind == ExprKind::FloatLiteral)
	{
		// P9.24 浮点后缀定型：f/F 后缀的 FloatLiteral 由 parser 携带 Float 类型，此处如实采纳
		//   （供 sizeof(2.0f)==4、auto/typeof(2.0f)==float）；无后缀/l/L 仍 double（Windows long double==double）。
		if (e->type && e->type->kind == TypeKind::Float) return e->type;
		return make_type(TypeKind::Double);
	}
	if (e->kind == ExprKind::IntLiteral || e->kind == ExprKind::CharLiteral)
	{
		// nullptr：parser 以 Void* 建模的零字面量。类型层须如实报指针（C23 nullptr_t 表示同 void*），
		//   供 sizeof(nullptr)==sizeof(void*) 及指针路径的元素类型解析取正确宽度。
		if (e->type && e->type->kind == TypeKind::Pointer) return e->type;
		// 位精确字面量（wb/ub）携带 _BitInt 类型，供 sizeof(expr) 等取真实宽度
		if (e->type && e->type->kind == TypeKind::BitInt) return e->type;
		// 字面量携带的类型（§6.4.5.2 选定，含 unsigned int/long/long long）优先采用，
		// 供常用算术转换/cast 加宽取正确宽度与符号；无符号小字面量仍为 i32。
		if (e->type && (e->type->kind == TypeKind::LongLong ||
				e->type->kind == TypeKind::Long ||
				e->type->kind == TypeKind::Int)) return e->type;
		return make_type(TypeKind::Int);
	}
	return nullptr;
}

} // namespace colang
