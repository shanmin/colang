// ============================================================
// parser.cpp — 语法分析器（P3.4）
//
// 手写递归下降 + 优先级爬升。解析范围：
//   函数声明/定义/全局变量/多声明符/数组声明符/int,char,void,const,
//   pointer/参数表/变参/复合语句/if-else/while/do-while/for/break/
//   continue/switch/case/default/goto/label/局部声明/return/表达式语句/
//   二元运算符全集/一元运算/赋值(=/op=)/数组下标/sizeof/三元(?:)/
//   逗号(,)/显式类型转换(cast)/字面量/标识符/函数调用。
// 后续 P3 扩展：struct/union/enum/typedef/++/--。
// ============================================================

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "colang/parser.h"
#include "colang/type_resolver.h"

namespace colang
{

namespace
{

// ============================================================
// 内部 Parser 结构
// ============================================================
struct Parser
{
	const std::vector<Token> &toks;
	std::size_t pos = 0;
	std::vector<Error> &errs;
	// typedef 名 -> 类型映射
	std::unordered_map<std::string, CTypePtr> typedefs_;
	// struct/union 定义存储（tag -> index in TU）
	std::unordered_map<std::string, std::size_t> struct_tags_;
	// enum 常量名 -> 值
	std::unordered_map<std::string, std::int64_t> enum_constants_;
	// 当前 TU 指针（parse_decl_specifiers 存 struct/enum 用）
	TranslationUnit *cur_tu_ = nullptr;
	bool last_auto_ = false; // parse_decl_specifiers 返回 nullptr 且因 auto
	unsigned last_alignas_ = 0; // parse_decl_specifiers 捕获的 _Alignas 强制对齐（字节；0=未指定）
	// 函数名 -> 返回类型（auto 推断用）
	std::unordered_map<std::string, CTypePtr> func_return_types_;
	// 局部变量名 -> 类型（typeof/auto 推断用，作用域简化为全局累积）
	std::unordered_map<std::string, CTypePtr> local_var_types_;

	Parser(const std::vector<Token> &tokens, std::vector<Error> &errors)
		: toks(tokens), errs(errors)
	{
	}

	// —— token 导航 ——
	const Token &peek(std::size_t off = 0) const
	{
		const std::size_t i = pos + off;
		if (i < toks.size())
		{
			return toks[i];
		}
		static const Token eof_tok = [] { Token t; t.kind = TokKind::EndOfFile; return t; }();
		return eof_tok;
	}
	const Token &advance() { return toks[pos++]; }
	bool at_end() const { return pos >= toks.size(); }

	bool check_keyword(const std::string &kw) const
	{
		const Token &t = peek();
		return (t.kind == TokKind::Keyword || t.kind == TokKind::Identifier) && t.text == kw;
	}
	bool check_punct(const std::string &p) const
	{
		const Token &t = peek();
		return t.kind == TokKind::Punct && t.text == p;
	}
	bool match_keyword(const std::string &kw)
	{
		if (check_keyword(kw)) { advance(); return true; }
		return false;
	}
	bool match_punct(const std::string &p)
	{
		if (check_punct(p)) { advance(); return true; }
		return false;
	}
	const Token &expect_punct(const std::string &p)
	{
		if (check_punct(p)) return advance();
		error(peek().loc, "expected '" + p + "'");
		static const Token dummy = [] { Token t; t.kind = TokKind::Punct; return t; }();
		return dummy;
	}
	const Token &expect_identifier()
	{
		if (peek().kind == TokKind::Identifier) return advance();
		error(peek().loc, "expected identifier");
		static const Token dummy = [] { Token t; t.kind = TokKind::Identifier; return t; }();
		return dummy;
	}

	void error(const SourceLoc &loc, const std::string &msg)
	{
		errs.push_back(Error{loc.file, Pos{loc.line, loc.column}, msg});
	}

	// —— 语法规约 ——

	TUPtr parse_translation_unit()
	{
		auto tu = std::make_shared<TranslationUnit>();
		cur_tu_ = tu.get();
		while (!at_end())
		{
			if (!parse_global_decl_or_func(*tu))
				skip_to_semi();
		}
		return tu;
	}

	// C23 [[attribute]] 跳过：识别连续的 [[...]] 并整段消费
	void skip_attributes()
	{
		while (peek().text == "[" && peek(1).text == "[")
		{
			advance(); // first [
			advance(); // second [
			// 跳过直到 ]]
			while (!at_end())
			{
				if (peek().text == "]" && peek(1).text == "]")
				{
					advance(); // first ]
					advance(); // second ]
					break;
				}
				advance();
			}
		}
	}

	// _BitInt(N) 解析（C23 任意宽度整数）
	CTypePtr parse_bitint()
	{
		auto loc = peek().loc;
		advance(); // consume '_BitInt'
		expect_punct("(");
		ExprPtr dim = parse_assign();
		expect_punct(")");
		int64_t n = eval_const_int(dim);
		if (n <= 0 || n > 65535)
			error(loc, "_BitInt width must be 1..65535");
		auto t = std::make_shared<CType>();
		t->kind = TypeKind::BitInt;
		t->bit_width = static_cast<unsigned>(n);
		return t;
	}

	// P140 M3：注入共享 resolve_expr_type 的 parser 侧符号环境。
	// 镜像 CodegenTypeEnv/SemaTypeEnv，读 parser 自有符号表（解析期可见子集）。
	struct ParserAutoTypeEnv
	{
		Parser *self;
		CTypePtr var_type(const std::string &n)
		{
			auto it = self->local_var_types_.find(n);
			if (it != self->local_var_types_.end()) return it->second;
			if (self->cur_tu_)
				for (const auto &gv : self->cur_tu_->globals)
					if (gv.name == n) return gv.type;
			return nullptr;
		}
		CTypePtr struct_member(const std::string &tag, const std::string &field)
		{
			if (!self->cur_tu_) return nullptr;
			for (const auto &s : self->cur_tu_->structs)
				if (s.tag == tag)
					for (const auto &m : s.members)
						if (m.name == field) return m.type;
			return nullptr;
		}
		CTypePtr func_return(const std::string &n)
		{
			auto it = self->func_return_types_.find(n);
			if (it != self->func_return_types_.end()) return it->second;
			return nullptr;
		}
	};

	// 编译期常量整数求值（支持 _Static_assert）
	// C23 auto 类型推断（简化版：根据初始化式字面量推断）
	CTypePtr infer_auto_type(const ExprPtr &e)
	{
		if (!e) return make_type(TypeKind::Int);
		// P140 M3 定向复用：叶/简单节点交共享 resolve_expr_type（复用字面量定型、
		//   nullptr、成员查找修复）；命中即返回，null 落回下方 parser 既有近似。
		//   刻意不委托 Binary/Unary/Call：resolve 对指针算术/va_arg/++-- abstain 或
		//   仅整型取向（int_promo_of 对 double 误得 int）——直接采用会回归 p136/p137。
		switch (e->kind)
		{
		case ExprKind::VarRef:
		case ExprKind::Cast:
		case ExprKind::StringLiteral:
		case ExprKind::FloatLiteral:
		case ExprKind::IntLiteral:
		case ExprKind::CharLiteral:
		case ExprKind::Member:
		{
			ParserAutoTypeEnv env{this};
			if (CTypePtr t = resolve_expr_type(e, env))
				return t;
			break;
		}
		default:
			break;
		}
		switch (e->kind)
		{
		case ExprKind::IntLiteral:
		case ExprKind::CharLiteral:
			// 位精确字面量（wb/ub 后缀）携带 _BitInt 类型，优先采用
			if (e->type && (e->type->kind == TypeKind::BitInt ||
				e->type->kind == TypeKind::LongLong ||
				e->type->kind == TypeKind::Long ||
				e->type->kind == TypeKind::Int)) return e->type;
			return make_type(TypeKind::Int);
		case ExprKind::FloatLiteral:
			return make_type(TypeKind::Double);
		case ExprKind::StringLiteral:
			return make_pointer(make_type(TypeKind::Char));
		case ExprKind::Unary:
			if (e->op == "&")
			{
				// &var → pointer to var's type
				CTypePtr inner = infer_auto_type(e->lhs);
				return make_pointer(inner);
			}
			if (e->op == "*")
			{
				CTypePtr pt = infer_auto_type(e->lhs);
				if (pt && pt->kind == TypeKind::Pointer) return pt->base;
				return make_type(TypeKind::Int);
			}
			// sizeof / _Alignof 结果 = size_t（LLP64 无符号 64，对齐 codegen int_ctype_of(64,true)）
			if (e->op == "sizeof" || e->op == "alignof")
			{
				auto st = make_type(TypeKind::LongLong);
				st->is_unsigned = true;
				return st;
			}
			if (e->op == "!")
				return make_type(TypeKind::Int);
			// 一元算术 / 位取反 / 自增自减：结果取操作数类型（整型/浮点/指针皆适用）
			if (e->op == "-" || e->op == "+" || e->op == "~" ||
				e->op == "++" || e->op == "--")
			{
				CTypePtr t = infer_auto_type(e->lhs);
				return t ? t : make_type(TypeKind::Int);
			}
			return make_type(TypeKind::Int);
		case ExprKind::Cast:
			return e->type ? e->type : make_type(TypeKind::Int);
		case ExprKind::Call:
			// 函数调用: 查返回类型
			{
				// va_arg(ap, type)：结果类型已在解析时写入 e->type
				if (e->name == "va_arg" && e->type) return e->type;
				auto fit = func_return_types_.find(e->name);
				if (fit != func_return_types_.end()) return fit->second;
			}
			return make_type(TypeKind::Int);
		case ExprKind::VarRef:
			// 查局部变量类型表
			{
				auto lit = local_var_types_.find(e->name);
				if (lit != local_var_types_.end()) return lit->second;
				// 查全局类型
				for (const auto &gv : cur_tu_->globals)
					if (gv.name == e->name) return gv.type;
			}
			return make_type(TypeKind::Int);
		case ExprKind::Binary:
		{
			const std::string &op = e->op;
			// 关系/相等/逻辑与或：结果恒 int
			if (op == "<" || op == ">" || op == "<=" || op == ">=" ||
				op == "==" || op == "!=" || op == "&&" || op == "||")
				return make_type(TypeKind::Int);
			// 数组下标 a[i] / i[a]：结果 = 指针或数组操作数的元素类型
			auto is_ptr_arr = [](const CTypePtr &t) {
				return t && (t->kind == TypeKind::Pointer || t->kind == TypeKind::Array);
			};
			if (op == "[]")
			{
				CTypePtr base = is_ptr_arr(infer_auto_type(e->lhs)) ? infer_auto_type(e->lhs)
				                 : (is_ptr_arr(infer_auto_type(e->rhs)) ? infer_auto_type(e->rhs) : nullptr);
				if (base && base->base) return base->base;
				return make_type(TypeKind::Int);
			}
			// 移位：结果为左操作数类型
			if (op == "<<" || op == ">>")
				return infer_auto_type(e->lhs);
			CTypePtr l = infer_auto_type(e->lhs);
			CTypePtr r = infer_auto_type(e->rhs);
			bool lP = l && l->kind == TypeKind::Pointer;
			bool rP = r && r->kind == TypeKind::Pointer;
			if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%")
			{
				if (op == "+" || op == "-")
				{
					if (lP && !rP) return l;            // ptr + int → ptr
					if (rP && !lP) return r;            // int + ptr → ptr
					if (lP && rP) return make_type(TypeKind::LongLong); // ptr - ptr → ptrdiff_t
				}
				bool anyD = (l && l->kind == TypeKind::Double) || (r && r->kind == TypeKind::Double);
				bool anyF = (l && l->kind == TypeKind::Float) || (r && r->kind == TypeKind::Float);
				if (anyD) return make_type(TypeKind::Double);
				if (anyF) return make_type(TypeKind::Float);
				// 整型：仅两侧同型时精确取之；否则沿用既有“未知→int”近似。
				// （完整整型提升/常用算术转换留待 sema 类型装饰单一真值源收敛，勿在此复制 codegen 格）
				if (l && r && l->kind == r->kind && !lP) return l;
				return make_type(TypeKind::Int);
			}
			if (op == "&" || op == "|" || op == "^")
			{
				if (l && r && l->kind == r->kind && !lP) return l;
				return make_type(TypeKind::Int);
			}
			return make_type(TypeKind::Int);
		}
		default:
			return make_type(TypeKind::Int);
		}
	}

	int64_t const_sizeof_type(const CTypePtr &t)
	{
		if (!t) return 4;
		switch (t->kind)
		{
		case TypeKind::Void: case TypeKind::Bool: case TypeKind::Char: case TypeKind::Char8: return 1;
		case TypeKind::Short: case TypeKind::WChar: case TypeKind::Char16: return 2;
		case TypeKind::Int: case TypeKind::Long: case TypeKind::Float: case TypeKind::Enum: case TypeKind::Char32: return 4;
		case TypeKind::LongLong: case TypeKind::Double: return 8;
		case TypeKind::Pointer: case TypeKind::Function: return 8; // LLP64
		case TypeKind::Array:
		{
			int64_t es = const_sizeof_type(t->base);
			return es * static_cast<int64_t>(t->array_size);
		}
		case TypeKind::Struct: case TypeKind::Union:
		{
			// 对齐感知的标准 C 布局（与 codegen 的 LLVM StructType/DataLayout 一致）
			for (const auto &s : cur_tu_->structs)
			{
				if (s.tag != t->tag) continue;
				if (s.is_union)
				{
					int64_t mx = 0, al = 1;
					for (const auto &m : s.members)
					{
						int64_t ms = const_sizeof_type(m.type);
						int64_t ma = const_member_align(m);
						if (ms > mx) mx = ms;
						if (ma > al) al = ma;
					}
					if (static_cast<int64_t>(t->align) > al) al = static_cast<int64_t>(t->align);
					int64_t r = ((mx + al - 1) / al) * al;
					return r > 0 ? r : 4;
				}
				// struct：逐成员对齐填充，尾部补到最大对齐
				int64_t off = 0, al = 1;
				for (const auto &m : s.members)
				{
					int64_t ma = const_member_align(m);
					if (ma < 1) ma = 1;
					if (ma > al) al = ma;
					off = ((off + ma - 1) / ma) * ma + const_sizeof_type(m.type);
				}
				if (static_cast<int64_t>(t->align) > al) al = static_cast<int64_t>(t->align);
				int64_t r = ((off + al - 1) / al) * al;
				return r > 0 ? r : 4;
			}
			return 4; // 未定义 struct/union
		}
		case TypeKind::BitInt:
			return (static_cast<int64_t>(t->bit_width) + 7) / 8;
		default: return 4;
		}
	}
	// struct/union 成员的有效对齐：max(类型自然对齐, 成员 _Alignas)
	int64_t const_member_align(const StructMember &m)
	{
		int64_t a = const_alignof_type(m.type);
		if (static_cast<int64_t>(m.align) > a) a = static_cast<int64_t>(m.align);
		return a < 1 ? 1 : a;
	}

	// 将 _Alignas 强制对齐并入类型节点（用于 typedef/声明）。
	// 仅在确会抬高对齐时浅拷贝 overriding，避免污染共享的 typedef 节点。
	CTypePtr apply_type_align(CTypePtr t, unsigned al)
	{
		if (!t || al == 0 || al <= t->align) return t;
		auto c = std::make_shared<CType>(*t);
		c->align = al;
		return c;
	}

	int64_t const_alignof_type(const CTypePtr &t)
	{
		if (!t) return 4;
		int64_t natural = 4;
		switch (t->kind)
		{
		case TypeKind::Void: case TypeKind::Bool: case TypeKind::Char: case TypeKind::Char8: natural = 1; break;
		case TypeKind::Short: case TypeKind::WChar: case TypeKind::Char16: natural = 2; break;
		case TypeKind::Int: case TypeKind::Long: case TypeKind::Float: case TypeKind::Enum: case TypeKind::Char32: natural = 4; break;
		case TypeKind::LongLong: case TypeKind::Double: natural = 8; break;
		case TypeKind::Pointer: case TypeKind::Function: natural = 8; break;
		case TypeKind::Array: natural = const_alignof_type(t->base); break;
		case TypeKind::BitInt: natural = 1; break; // _BitInt 对齐 = 1
		case TypeKind::Struct: case TypeKind::Union:
		{
			// struct/union 对齐 = 各成员有效对齐的最大值
			int64_t al = 1;
			for (const auto &s : cur_tu_->structs)
				if (s.tag == t->tag)
				{
					for (const auto &m : s.members)
					{
						int64_t ma = const_member_align(m);
						if (ma > al) al = ma;
					}
					break;
				}
			natural = al;
			break;
		}
		default: natural = 4; break;
		}
		// 类型自身携带的 _Alignas（typedef/独立类型形式）并入：取 max
		if (static_cast<int64_t>(t->align) > natural) natural = static_cast<int64_t>(t->align);
		return natural;
	}

	int64_t eval_const_int(const ExprPtr &e)
	{
		if (!e) return 0;
		switch (e->kind)
		{
		case ExprKind::IntLiteral:
		case ExprKind::CharLiteral:
			return e->int_val;
		case ExprKind::Unary:
		{
			if (e->op == "sizeof")
			{
				// 编译期 sizeof: 简化平台大小 (LLP64 Windows)
				if (e->type) return const_sizeof_type(e->type);
				if (e->lhs && e->lhs->type && e->lhs->type->kind == TypeKind::BitInt)
					return const_sizeof_type(e->lhs->type); // sizeof(位精确字面量)
				if (e->lhs) return 4; // sizeof(expr) 默认按 int
				return 4;
			}
			if (e->op == "alignof")
			{
				if (e->type) return const_alignof_type(e->type);
				return 4;
			}
			int64_t v = eval_const_int(e->lhs);
			if (e->op == "-") return -v;
			if (e->op == "!") return !v;
			if (e->op == "~") return ~v;
			return 0;
		}
		case ExprKind::Binary:
		{
			int64_t l = eval_const_int(e->lhs);
			int64_t r = eval_const_int(e->rhs);
			const auto &op = e->op;
			if (op == "+") return l + r;
			if (op == "-") return l - r;
			if (op == "*") return l * r;
			if (op == "/") return r ? l / r : 0;
			if (op == "%") return r ? l % r : 0;
			if (op == "<<") return l << r;
			if (op == ">>") return l >> r;
			if (op == "&") return l & r;
			if (op == "|") return l | r;
			if (op == "^") return l ^ r;
			if (op == "==") return l == r;
			if (op == "!=") return l != r;
			if (op == "<") return l < r;
			if (op == "<=") return l <= r;
			if (op == ">") return l > r;
			if (op == ">=") return l >= r;
			if (op == "&&") return (l && r) ? 1 : 0;
			if (op == "||") return (l || r) ? 1 : 0;
			return 0;
		}
		default:
			return 0;
		}
	}

	bool parse_global_decl_or_func(TranslationUnit &tu)
	{
		SourceLoc loc = peek().loc;
		// C23 [[attribute]] 跳过
		skip_attributes();
		// _Static_assert(const_expr, "msg");
		if (check_keyword("_Static_assert") || check_keyword("static_assert"))
		{
			advance(); // keyword
			expect_punct("(");
			ExprPtr cond = parse_assign();
			match_punct(",");
			std::string msg;
			if (peek().kind == TokKind::String) msg = advance().text;
			expect_punct(")");
			expect_punct(";");
			// 编译期求值
			int64_t val = eval_const_int(cond);
			if (val == 0)
				error(loc, "static assertion failed: " + (msg.empty() ? "" : msg));
			return true;
		}
		// 存储类说明符
		bool is_static = match_keyword("static");
		bool is_extern = match_keyword("extern");
		// typedef 检测
		bool is_typedef = match_keyword("typedef");
		CTypePtr base_type = parse_decl_specifiers();
		unsigned ga = last_alignas_; // 本声明的 _Alignas（需在任何后续递归解析前捕获）
		if (!base_type) return false;
		// 纯类型定义（struct/enum 无变量名）: 后面跟 ';'
		if (check_punct(";")) { advance(); return true; }
		// —— typedef：各声明符经统一 Decorator（对象/数组/函数指针皆然）——
		if (is_typedef)
		{
			do
			{
				std::string nm;
				DeclaratorFn d = parse_declarator_rec(nm);
				typedefs_[nm] = apply_type_align(d(base_type), ga);
			} while (match_punct(","));
			match_punct(";");
			return true;
		}
		// —— 函数声明/定义（保留具名参数与函数体）：(*[qual] Identifier 后紧跟 '(' ——
		if (is_function_form())
		{
			CTypePtr dtype = parse_pointer_suffix(base_type);
			std::string name = expect_identifier().text;
			FuncDecl fn;
			fn.loc = loc;
			fn.name = std::move(name);
			fn.return_type = std::move(dtype);
			fn.is_static = is_static;
			expect_punct("(");
			parse_param_list(fn.params, fn.is_variadic);
			expect_punct(")");
			if (check_punct("{"))
				fn.body = parse_compound_stmt();
			else if (!check_punct(","))
				match_punct(";");
			func_return_types_[fn.name] = fn.return_type;
			tu.funcs.push_back(std::move(fn));
			// 多声明符续接（如 int f(int a), g(int a), a;）：其余声明符经 Decorator
			while (match_punct(","))
				parse_global_declarator_item(tu, base_type, is_static, is_extern, ga, loc);
			match_punct(";");
			return true;
		}
		// —— 对象/分组/函数指针对象声明符（可多个，逗号分隔）——
		do
		{
			parse_global_declarator_item(tu, base_type, is_static, is_extern, ga, loc);
		} while (match_punct(","));
		match_punct(";");
		return true;
	}

	CTypePtr parse_decl_specifiers()
	{
		// 先消费前置存储类关键字（不影响类型）
		// register：C23 仍保留该关键字（存储类），但不保证真实寄存器绑定；按 clang 默认作 no-op 消费
		while (match_keyword("register") ||
			match_keyword("inline") || match_keyword("restrict") || match_keyword("_Inline") ||
			match_keyword("_Noreturn") || match_keyword("_Thread_local") ||
			match_keyword("thread_local") || match_keyword("_Atomic"))
			;
		// C23 _Alignas(...)：§6.7.2.1p6 多个取最大；类型形式取 _Alignof，常量形式取该常量。
		// 前缀在此消费，避免后续类型解析（struct/typeof 递归）覆盖本声明的对齐值。
		unsigned acc = 0;
		while (match_keyword("_Alignas") || match_keyword("alignas"))
		{
			expect_punct("(");
			int64_t a = 0;
			if (is_type_start())
			{
				// _Alignas(type-name)：取该类型的对齐（含 struct 成员对齐感知）
				CTypePtr at = parse_decl_specifiers_core();
				at = parse_pointer_suffix(at);
				if (check_punct("["))
					at = parse_array_suffix(at);
				a = const_alignof_type(at);
			}
			else
			{
				// _Alignas(constant-expression)
				ExprPtr ce = parse_assign();
				a = eval_const_int(ce);
			}
			expect_punct(")");
			if (a < 0 || (a != 0 && (a & (a - 1)) != 0))
				error(peek().loc, "_Alignas argument must be a power of two or zero");
			else if (static_cast<unsigned>(a) > acc)
				acc = static_cast<unsigned>(a);
		}
		CTypePtr r = parse_decl_specifiers_core();
		// C23 §6.7：限定符可出现在类型说明符之后（int const x / enum E const *e2 / struct S const *s2）。
		//   指针限定符（int * const p）的 const 在 * 之后、不在此路径，仍由 parse_declarator_rec/parse_pointer_suffix 处理。
		for (;;)
		{
			if (match_keyword("const")) { if (r) r->is_const = true; continue; }
			if (match_keyword("volatile") || match_keyword("restrict") || match_keyword("_Atomic")) continue;
			break;
		}
		last_alignas_ = acc; // 本声明的 _Alignas（覆盖递归期间对 last_alignas_ 的改写）
		return r;
	}

	CTypePtr parse_decl_specifiers_core()
	{
		last_auto_ = false;
		// 跳过不影响类型的存储类/属性关键字（含 register no-op，见 P141）
		while (match_keyword("register") ||
			match_keyword("inline") || match_keyword("restrict") || match_keyword("_Inline") ||
			match_keyword("_Noreturn") || match_keyword("_Thread_local") ||
			match_keyword("thread_local") || match_keyword("_Atomic"))
			;
		// C23 [[attribute]] 跳过（不影响类型解析）
		skip_attributes();
		bool cnst = false;
		while (match_keyword("const") || match_keyword("constexpr"))
			cnst = true;
		if (match_keyword("void"))
			return make_type(TypeKind::Void, cnst);
		if (match_keyword("auto"))
		{
			last_auto_ = true; // C23 auto: 类型由初始化式推断
			return nullptr;
		}
		// C23 typeof(expr) / typeof(type) / typeof_unqual(type)
		if (check_keyword("typeof") || check_keyword("typeof_unqual"))
		{
			// C23: typeof_unqual 与 typeof 同解析路径，仅额外剥离顶层限定符
			bool unqual = (peek().text == "typeof_unqual");
			advance(); // consume keyword
			expect_punct("(");
			CTypePtr result;
			// 此时当前 token 是括号内第一个: 若是类型关键字则为 typeof(type)
			if (is_type_start())
			{
				// typeof(type)
				result = parse_decl_specifiers();
				result = parse_pointer_suffix(result);
				if (check_punct("["))
					result = parse_array_suffix(result);
			}
			else
			{
				// typeof(expr) —— 解析表达式并推断类型
				ExprPtr inner = parse_assign();
				result = infer_auto_type(inner);
			}
			expect_punct(")");
			// typeof_unqual: 剥离顶层限定符（colang 类型系统当前仅建模 is_const；
			//   volatile 未建模，深层限定符待 sema 类型装饰收敛时处理）
			if (unqual && result)
				result->is_const = false;
			return result;
		}
		if (match_keyword("_Bool") || match_keyword("bool"))
			return make_type(TypeKind::Bool, cnst);
		if (match_keyword("char"))
			return make_type(TypeKind::Char, cnst);
		if (match_keyword("wchar_t"))
			return make_type(TypeKind::WChar, cnst);  // 2 字节无符号（对齐宿主 CRT）
		// charN_t：恒无符号的独立整型（char8_t=C23/1B、char16_t=C11/2B、char32_t=C11/4B）；
		//   不可与 signed/unsigned/short/long 复合，故直接 return。
		if (match_keyword("char8_t"))  { auto t = make_type(TypeKind::Char8,  cnst); t->is_unsigned = true; return t; }
		if (match_keyword("char16_t")) { auto t = make_type(TypeKind::Char16, cnst); t->is_unsigned = true; return t; }
		if (match_keyword("char32_t")) { auto t = make_type(TypeKind::Char32, cnst); t->is_unsigned = true; return t; }
		if (match_keyword("short"))
		{
			match_keyword("int");  // 可选尾随 int（short int）
			return make_type(TypeKind::Short, cnst);  // short: i16（2 字节，有符号）
		}
		if (match_keyword("int"))
			return make_type(TypeKind::Int, cnst);
		if (match_keyword("long"))
		{
			// long long [int] → LongLong（尾随 int 可选，对齐 signed/unsigned long long int 分支）
			if (match_keyword("long"))
			{
				match_keyword("int");
				return make_type(TypeKind::LongLong, cnst);
			}
			// long double → Double（本机 Windows x64 long double == double，8 字节，对齐 clang -std=c23；修 plct 00204）
			if (match_keyword("double"))
				return make_type(TypeKind::Double, cnst);
			// optional 'int' after long
			match_keyword("int");
			return make_type(TypeKind::Long, cnst);
		}
		if (match_keyword("unsigned"))
		{
			// unsigned _BitInt(N)
			if (check_keyword("_BitInt"))
			{
				auto t = parse_bitint();
				t->is_unsigned = true;
				return t;
			}
			if (match_keyword("char")) { auto t = make_type(TypeKind::Char, cnst); t->is_unsigned = true; return t; }
			if (match_keyword("short")) { match_keyword("int"); auto t = make_type(TypeKind::Short, cnst); t->is_unsigned = true; return t; }
			if (match_keyword("long"))
			{
				if (match_keyword("long")) { match_keyword("int"); auto t = make_type(TypeKind::LongLong, cnst); t->is_unsigned = true; return t; }
				match_keyword("int");
				auto t = make_type(TypeKind::Long, cnst); t->is_unsigned = true; return t;
			}
			match_keyword("int");
			{ auto t = make_type(TypeKind::Int, cnst); t->is_unsigned = true; return t; }
		}
		if (match_keyword("signed"))
		{
			// signed _BitInt(N)
			if (check_keyword("_BitInt"))
				return parse_bitint();
			if (match_keyword("char")) return make_type(TypeKind::Char, cnst);
			if (match_keyword("short")) { match_keyword("int"); return make_type(TypeKind::Short, cnst); }
			if (match_keyword("long"))
			{
				if (match_keyword("long")) { match_keyword("int"); return make_type(TypeKind::LongLong, cnst); }
				match_keyword("int");
				return make_type(TypeKind::Long, cnst);
			}
			match_keyword("int");
			return make_type(TypeKind::Int, cnst);
		}
		// _BitInt(N) 独立出现（默认 signed）
		if (check_keyword("_BitInt"))
			return parse_bitint();
		if (match_keyword("float"))
			return make_type(TypeKind::Float, cnst);
		if (match_keyword("double"))
			return make_type(TypeKind::Double, cnst);
		// struct / union
		if (check_keyword("struct") || check_keyword("union"))
		{
			bool is_union = check_keyword("union");
			advance(); // consume 'struct'/'union'
			std::string tag;
			if (peek().kind == TokKind::Identifier)
				tag = advance().text;
			if (check_punct("{"))
			{
				// 完整定义
				advance(); // consume '{'
				StructDef sd;
				sd.tag = tag;
				sd.is_union = is_union;
				sd.loc = peek().loc;
				while (!check_punct("}") && !at_end())
				{
					CTypePtr mtype = parse_decl_specifiers();
					unsigned mal = last_alignas_; // 本成员的 _Alignas（后续声明符解析前捕获）
					if (!mtype) { skip_to_semi_or_brace(); continue; }
					// 匿名 struct/union 成员: type是struct/union且紧跟';'（无成员名）
					if ((mtype->kind == TypeKind::Struct || mtype->kind == TypeKind::Union)
						&& check_punct(";"))
					{
						// 匿名 union：不展开，作为单个匿名成员嵌入（保留重叠语义、
						// 占一个位置初始化槽）。mtype 已带合成 tag `__anon_union_N`（见 L835）。
						// 已知限制：`.c` 指向提升名的指定符、alignas 手工布局叠加匿名 union、
						//   匿名 union 内位域均维持现状（罕见组合，回归门槛兜底）。
						if (mtype->kind == TypeKind::Union)
						{
							StructMember m;
							m.name = ""; // 匿名：成员名由 sema/codegen 名提升递归解析
							m.type = mtype;
							m.align = last_alignas_;
							sd.members.push_back(std::move(m));
							match_punct(";");
							continue;
						}
						// 匿名 struct：展开内部成员（布局/初始化/访问与嵌等价）
						const std::string &inner_tag = mtype->tag;
						const StructDef *inner_sd = nullptr;
						for (const auto &s : cur_tu_->structs)
						{
							if (inner_tag.empty())
							{
								// 最近定义的匿名 struct
								if (!s.tag.empty() || &s == &cur_tu_->structs.back())
									{ inner_sd = &s; break; }
							}
							else if (s.tag == inner_tag) { inner_sd = &s; break; }
						}
						// 匿名: 取最后定义的
						if (!inner_sd && !cur_tu_->structs.empty())
							inner_sd = &cur_tu_->structs.back();
						if (inner_sd)
							for (const auto &im : inner_sd->members)
								sd.members.push_back(im);
						match_punct(";");
						continue;
					}
					do {
						// 函数指针成员: int (*fp)(int, int)
						if (is_funcptr_declarator())
						{
							auto [fp_name, fp_type] = parse_funcptr_declarator(mtype);
							StructMember m;
							m.name = fp_name;
							m.type = fp_type;
							m.align = mal;
							sd.members.push_back(std::move(m));
							break; // funcptr 不支持逗号分隔多个声明符
						}
						mtype = parse_pointer_suffix(mtype);
						StructMember m;
						if (!check_punct(":")) // 匿名位域（type : width，无成员名，§6.7.2.1p5）
							m.name = expect_identifier().text;
						m.type = parse_array_suffix(mtype);
						m.align = mal;
						// 位域: name : width
						if (check_punct(":"))
						{
							advance(); // ':'
							ExprPtr w = parse_assign();
							m.bit_width = static_cast<int>(eval_const_int(w));
						}
						sd.members.push_back(std::move(m));
					} while (match_punct(","));
					match_punct(";");
				}
				expect_punct("}");
				// 匿名 struct/union: 分配合成 tag
				if (tag.empty() && !sd.members.empty())
				{
					tag = std::string("__anon_") + (is_union ? "union_" : "struct_")
						+ std::to_string(cur_tu_->structs.size());
					sd.tag = tag;
				}
				// 注册到 struct_tags_
				if (!tag.empty())
					struct_tags_[tag] = 0;
				// 存入 TU
				cur_tu_->structs.push_back(std::move(sd));
				if (!tag.empty())
					struct_tags_[tag] = cur_tu_->structs.size() - 1;
			}
			else
			{
				// 引用已有 tag（未定义也先记录）
				if (!struct_tags_.count(tag))
				{
					StructDef fwd;
					fwd.tag = tag;
					fwd.is_union = is_union;
					cur_tu_->structs.push_back(std::move(fwd));
					struct_tags_[tag] = cur_tu_->structs.size() - 1;
				}
			}
			auto st = std::make_shared<CType>();
			st->kind = is_union ? TypeKind::Union : TypeKind::Struct;
			st->tag = tag;
			st->is_const = cnst;
			return st;
		}
		// enum
		if (check_keyword("enum"))
		{
			advance();
			std::string tag;
			if (peek().kind == TokKind::Identifier)
				tag = advance().text;
			if (check_punct("{"))
			{
				advance();
				EnumDef ed;
				ed.tag = tag;
				ed.loc = peek().loc;
				std::int64_t next_val = 0;
				while (!check_punct("}") && !at_end())
				{
					EnumConstant ec;
					ec.name = expect_identifier().text;
					if (match_punct("="))
						next_val = eval_const_int(parse_assign()); // 一般常量表达式枚举值（含 0x/0b/0o/offsetof/sizeof/算术）
					ec.value = next_val++;
					enum_constants_[ec.name] = ec.value;
					ed.constants.push_back(std::move(ec));
					if (!match_punct(",")) break;
				}
				expect_punct("}");
				cur_tu_->enums.push_back(std::move(ed));
			}
			auto et = std::make_shared<CType>();
			et->kind = TypeKind::Enum;
			et->tag = tag;
			et->is_const = cnst;
			return et;
		}
		// typedef 名解析
		if (peek().kind == TokKind::Identifier)
		{
			auto it = typedefs_.find(peek().text);
			if (it != typedefs_.end())
			{
				advance();
				return it->second;
			}
		}
		error(peek().loc, "expected type specifier, got '" + peek().text + "'");
		return nullptr;
	}

	CTypePtr parse_pointer_suffix(CTypePtr base)
	{
		while (check_punct("*"))
		{
			advance();
			// '*' 之后的限定符属于**指针自身**（顶层限定），不是 pointee。
			//   旧写法误记到 base(pointee)->is_const，致 `int * const` 与 `const int *` 不可区分（_Generic L46 会误命中）。
			bool ptr_quals = false;
			while (match_keyword("const") || match_keyword("restrict") ||
				match_keyword("volatile") || match_keyword("_Atomic"))
				ptr_quals = true;
			// pointee const/volatile 由 parse_decl_specifiers 前置关键字置位，不在此处理。
			//   colang CType 仅有 is_const 字段（无 is_volatile），故 volatile/restrict/_Atomic 顶层限定统一并入指针节点 is_const 占位。
			auto p = make_pointer(base);
			if (ptr_quals)
				p->is_const = true;
			base = p;
		}
		return base;
	}

	void parse_param_list(std::vector<Param> &params, bool &variadic)
	{
		// (void)
		if (check_keyword("void") && peek(1).kind == TokKind::Punct && peek(1).text == ")")
		{
			advance(); // consume 'void'
			return;
		}
		if (check_punct(")"))
		{
			return; // empty
		}
		variadic = false;
		for (;;)
		{
			if (check_punct("..."))
			{
				advance();
				variadic = true;
				break;
			}
			Param p;
			CTypePtr base = parse_decl_specifiers();
			if (!base)
				base = make_type(TypeKind::Int); // fallback
			CTypePtr t;
			// 前导 '(' 消歧（§6.7.6p6）：其后为类型/typedef 名/void/.../空 → 函数指示符参数表（退化为指针）；
			//   否则（* / ( / [ / 普通标识符）→ 分组/具名声明符，委托 Decorator。
			if (check_punct("(") && starts_parameter_type_list())
			{
				advance(); // '('
				std::vector<Param> inner;
				bool ivariadic = false;
				parse_param_list(inner, ivariadic);
				expect_punct(")");
				std::vector<CTypePtr> innerTypes;
				for (auto &ip : inner) innerTypes.push_back(ip.type);
				t = make_pointer(make_func_type(base, innerTypes, ivariadic)); // 函数指示符形参 → 函数指针
			}
			else
			{
				std::string pname;
				DeclaratorFn d = parse_declarator_rec(pname); // 具名/* /数组/抽象/分组/内联函数指针皆正确处理
				t = d(base);
				p.name = std::move(pname);
			}
			// 退化调整（§6.7.6.3）：数组形参 → 元素指针；顶层函数指示符形参 → 函数指针
			while (t && t->kind == TypeKind::Array)
				t = make_pointer(t->base);
			if (t && t->kind == TypeKind::Function)
				t = make_pointer(t);
			p.type = t;
			params.push_back(std::move(p));
			if (!match_punct(","))
			{
				break;
			}
		}
	}

	StmtPtr parse_compound_stmt()
	{
		auto stmt = std::make_shared<Stmt>();
		stmt->kind = StmtKind::Compound;
		stmt->is_block_scope = true; // 真块 {...}：sema/codegen 据此压弹块级作用域
		stmt->loc = peek().loc;
		expect_punct("{");
		while (!check_punct("}") && !at_end())
		{
			auto inner = parse_stmt();
			if (inner)
			{
				stmt->body.push_back(std::move(inner));
			}
			else
			{
				skip_to_semi_or_brace();
			}
		}
		expect_punct("}");
		return stmt;
	}

	StmtPtr parse_stmt()
	{
		// C23: [[attribute]] 可出现在语句前
		skip_attributes();
		if (check_keyword("if"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::If;
			s->loc = advance().loc;
			expect_punct("(");
			// C23 init-statement: if (init; cond)
			skip_attributes();
			if (is_type_start())
			{
				s->init_stmt = parse_local_decl_no_semi();
				if (!s->init_stmt) { error(peek().loc, "expected init-declaration"); return s; }
				expect_punct(";");
				s->expr = parse_expr();
			}
			else
			{
				ExprPtr first = parse_expr();
				if (check_punct(";"))
				{
					// expression-statement as init
					auto es = std::make_shared<Stmt>();
					es->kind = StmtKind::ExprStmt;
					es->expr = first;
					s->init_stmt = es;
					advance(); // ;
					s->expr = parse_expr();
				}
				else
					s->expr = first;
			}
			expect_punct(")");
			s->then_branch = parse_stmt();
			if (match_keyword("else"))
				s->else_branch = parse_stmt();
			return s;
		}
		if (check_keyword("while"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::While;
			s->loc = advance().loc;
			expect_punct("(");
			s->expr = parse_expr();
			expect_punct(")");
			s->then_branch = parse_stmt();
			return s;
		}
		if (check_keyword("do"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::DoWhile;
			s->loc = advance().loc;
			s->then_branch = parse_stmt();
			if (!match_keyword("while")) { error(peek().loc, "expected 'while'"); return s; }
			expect_punct("(");
			s->expr = parse_expr();
			expect_punct(")");
			match_punct(";");
			return s;
		}
		if (check_keyword("return"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Return;
			s->loc = advance().loc;
			if (!check_punct(";"))
				s->expr = parse_expr();
			match_punct(";");
			return s;
		}
		if (check_keyword("for"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::For;
			s->loc = advance().loc;
			expect_punct("(");
			// init
			if (check_punct(";"))
				advance();
			else if (is_type_start())
				s->for_init = parse_local_decl();
			else
			{
				auto es = std::make_shared<Stmt>();
				es->kind = StmtKind::ExprStmt;
				es->loc = peek().loc;
				es->expr = parse_expr();
				match_punct(";");
				s->for_init = es;
			}
			// cond
			if (!check_punct(";"))
				s->expr = parse_expr();
			match_punct(";");
			// incr
			if (!check_punct(")"))
				s->for_incr = parse_expr();
			expect_punct(")");
			s->then_branch = parse_stmt();
			return s;
		}
		if (check_keyword("break"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Break;
			s->loc = advance().loc;
			match_punct(";");
			return s;
		}
		if (check_keyword("continue"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Continue;
			s->loc = advance().loc;
			match_punct(";");
			return s;
		}
		if (check_keyword("switch"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Switch;
			s->loc = advance().loc;
			expect_punct("(");
			// C23 init-statement: switch (init; cond)
			skip_attributes();
			if (is_type_start())
			{
				s->init_stmt = parse_local_decl_no_semi();
				if (!s->init_stmt) { error(peek().loc, "expected init-declaration"); return s; }
				expect_punct(";");
				s->expr = parse_expr();
			}
			else
			{
				ExprPtr first = parse_expr();
				if (check_punct(";"))
				{
					auto es = std::make_shared<Stmt>();
					es->kind = StmtKind::ExprStmt;
					es->expr = first;
					s->init_stmt = es;
					advance(); // ;
					s->expr = parse_expr();
				}
				else
					s->expr = first;
			}
			expect_punct(")");
			s->then_branch = parse_stmt(); // body (usually compound)
			return s;
		}
		if (check_keyword("case"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Case;
			s->loc = advance().loc;
			s->case_val = eval_const_int(parse_assign()); // 一般常量表达式 case 标签（含 0x/0b/0o/字符/offsetof/sizeof/枚举/算术）
			expect_punct(":");
			s->then_branch = parse_stmt();
			return s;
		}
		if (check_keyword("default"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Default;
			s->loc = advance().loc;
			expect_punct(":");
			s->then_branch = parse_stmt();
			return s;
		}
		if (check_keyword("goto"))
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Goto;
			s->loc = advance().loc;
			s->label_name = expect_identifier().text;
			match_punct(";");
			return s;
		}
		// label: identifier + ':' (不跟另一个 ':')
		if (peek().kind == TokKind::Identifier &&
			peek(1).kind == TokKind::Punct && peek(1).text == ":")
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Label;
			s->loc = peek().loc;
			s->label_name = advance().text;
			advance(); // consume ':'
			s->then_branch = parse_stmt();
			return s;
		}
		// 局部声明（类型关键字开头）
		if (is_type_start())
			return parse_local_decl();
		// 复合语句
		if (check_punct("{"))
			return parse_compound_stmt();
		// expression-statement
		auto s = std::make_shared<Stmt>();
		s->kind = StmtKind::ExprStmt;
		s->loc = peek().loc;
		if (!check_punct(";"))
		{
			s->expr = parse_expr();
			if (!s->expr)
				return nullptr;
		}
		match_punct(";");
		return s;
	}

	StmtPtr parse_local_decl()
	{
		// C23 [[attribute]] 跳过
		skip_attributes();
		// 存储类说明符
		bool is_static = match_keyword("static");
		// 块级 typedef
		bool is_td = match_keyword("typedef");
		CTypePtr base_type = parse_decl_specifiers();
		unsigned al = last_alignas_; // 本声明的 _Alignas（在后续递归解析前捕获）
		if (!base_type && !last_auto_) return nullptr;
		// C23 auto 类型推断: 解析 name + init，然后推断类型
		if (last_auto_)
		{
			last_auto_ = false;
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Decl;
			s->loc = peek().loc;
			s->var_name = expect_identifier().text;
			s->is_static = is_static;
			s->var_align = al;
			if (!match_punct("="))
			{
				error(peek().loc, "'auto' variable requires an initializer");
				return nullptr;
			}
			s->var_init = check_punct("{") ? parse_init_list() : parse_assign();
			// 推断类型
			s->var_type = infer_auto_type(s->var_init);
			local_var_types_[s->var_name] = s->var_type;
			match_punct(";");
			return s;
		}
		// 纯类型定义（无变量名）: struct/enum 后紧跟 ';'
		if (!is_td && check_punct(";"))
		{
			advance();
			auto nop = std::make_shared<Stmt>();
			nop->kind = StmtKind::ExprStmt;
			nop->loc = peek().loc;
			return nop;
		}
		if (is_td)
		{
			// typedef name; 注册别名（对象/数组/函数指针皆经 Decorator）
			do {
				std::string nm;
				DeclaratorFn d = parse_declarator_rec(nm);
				typedefs_[nm] = apply_type_align(d(base_type), al);
			} while (match_punct(","));
			match_punct(";");
			// 返回空 ExprStmt 作占位
			auto nop = std::make_shared<Stmt>();
			nop->kind = StmtKind::ExprStmt;
			nop->loc = peek().loc;
			return nop;
		}
		// 局部声明符（含分组 (*p)[4]、函数指针对象 (*fp)(int)、数组等）：Decorator
		auto compound = std::make_shared<Stmt>();
		compound->kind = StmtKind::Compound;
		compound->loc = peek().loc;
		do
		{
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Decl;
			s->loc = peek().loc;
			std::string nm;
			DeclaratorFn d = parse_declarator_rec(nm);
			s->var_type = d(base_type);
			s->var_name = nm;
			s->is_static = is_static;
			s->var_align = al;
			if (match_punct("="))
				s->var_init = check_punct("{") ? parse_init_list() : parse_assign();
			deduce_array_size(s->var_type, s->var_init);
			local_var_types_[s->var_name] = s->var_type;
			compound->body.push_back(std::move(s));
		} while (match_punct(","));
		match_punct(";");
		if (compound->body.size() == 1)
			return compound->body[0];
		return compound;
	}
	
	// C23 if/switch init-declaration: 解析类似 parse_local_decl 但不消费 ';'
	StmtPtr parse_local_decl_no_semi()
	{
		skip_attributes();
		CTypePtr base_type = parse_decl_specifiers();
		if (!base_type && !last_auto_) return nullptr;
		if (last_auto_)
		{
			last_auto_ = false;
			auto s = std::make_shared<Stmt>();
			s->kind = StmtKind::Decl;
			s->loc = peek().loc;
			s->var_name = expect_identifier().text;
			if (!match_punct("=")) { error(peek().loc, "auto requires initializer"); return nullptr; }
			s->var_init = parse_assign();
			s->var_type = infer_auto_type(s->var_init);
			local_var_types_[s->var_name] = s->var_type;
			return s;
		}
		// 简单声明: type name = expr
		auto s = std::make_shared<Stmt>();
		s->kind = StmtKind::Decl;
		s->loc = peek().loc;
		s->var_type = parse_pointer_suffix(base_type);
		s->var_name = expect_identifier().text;
		s->var_type = parse_array_suffix(s->var_type);
		if (match_punct("="))
			s->var_init = check_punct("{") ? parse_init_list() : parse_assign();
		deduce_array_size(s->var_type, s->var_init);
		local_var_types_[s->var_name] = s->var_type;
		return s;
	}
	
	// 复合初始化器 {e1, e2, ...} 或嵌套 {...}/{...}
	ExprPtr parse_init_list()
	{
		auto e = std::make_shared<Expr>();
		e->kind = ExprKind::InitList;
		e->loc = expect_punct("{").loc;
		while (!check_punct("}") && !at_end())
		{
			// 指定初始化器: [expr]=val 或 .name=val
			int di = -1;
			std::string df;
			if (check_punct("["))
			{
				advance(); // [
				// 护栏：数组下标指定符须为整型常量表达式；解析失败(如不支持的 GCC 范围指定符
				//   [a ... b]，parse_assign 见 '...' 返回 null)时不可直接 ->int_val，否则空指针 AV。
				//   parse_primary 已记录诊断，di 保持 -1（无效下标，后续按畸形初始化器处理）。
				auto idx = parse_assign();
				di = idx ? static_cast<int>(idx->int_val) : -1;
				expect_punct("]");
				expect_punct("=");
			}
			else if (check_punct(".") && peek(1).kind == TokKind::Identifier)
			{
				advance(); // .
				df = advance().text;
				expect_punct("=");
			}
			// 值表达式
			ExprPtr val;
			if (check_punct("{"))
				val = parse_init_list();
			else
				val = parse_assign();
			// 护栏：值表达式解析失败时(如不支持的 GCC 范围指定符 [a ... b]、畸形初始化器)
			//   parse_assign/parse_init_list 返回 null，parse_primary 已记录诊断；此处终止本
			//   初始化列表，避免对空 ExprPtr 解引用而崩溃（00216 编译期 AV 根因）。
			if (!val) break;
			// 设置 designator
			val->desig_index = di;
			val->desig_field = df;
			e->args.push_back(val);
			if (!match_punct(",")) break;
		}
		expect_punct("}");
		return e;
	}

	// 检测 (*name) 函数指针声明符模式
	bool is_funcptr_declarator() const
	{
		return check_punct("(") && peek(1).kind == TokKind::Punct && peek(1).text == "*";
	}

	// 解析 (*name)(params) → Pointer{Function{...}}
	std::pair<std::string, CTypePtr> parse_funcptr_declarator(CTypePtr ret_type)
	{
		advance(); // (
		advance(); // *
		std::string name = expect_identifier().text;
		expect_punct(")");
		if (check_punct("("))
		{
			advance(); // (
			std::vector<Param> ps;
			bool variadic = false;
			parse_param_list(ps, variadic);
			expect_punct(")");
			std::vector<CTypePtr> ptypes;
			for (auto &p : ps) ptypes.push_back(p.type);
			auto fn_ty = make_func_type(ret_type, std::move(ptypes), variadic);
			return {name, make_pointer(fn_ty)};
		}
		if (check_punct("["))
		{
			// (*name)[N] —— pointer to array[N] of ret_type
			auto arr_ty = parse_array_suffix(ret_type);
			return {name, make_pointer(arr_ty)};
		}
		// 无参数列表/无数组后缀：只是指针
		return {name, make_pointer(ret_type)};
	}

	// 解析连续的数组后缀 [ ... ]，返回各维大小（<=0 表示未定/柔性）。消费所有前导 [..]。
	// C99：[ static qual-list? N ]、VLA [ * ] / [ qual * ]；qualifier 列表（const/volatile/restrict/_Atomic）跳过。
	std::vector<std::size_t> parse_bracket_dims()
	{
		std::vector<std::size_t> dims;
		while (check_punct("["))
		{
			advance();
			match_keyword("static");
			while (match_keyword("const") || match_keyword("volatile") ||
				match_keyword("restrict") || match_keyword("_Atomic"))
			{
			}
			std::size_t sz = 0;
			if (!check_punct("]"))
			{
				if (check_punct("*"))
				{
					advance(); // VLA 未定大小 [*]：按柔性/未定处理（sz=0）
				}
				else
				{
					// 维度为一般常量表达式：字面量(含 0x/0b/0o/八/十)、sizeof/offsetof/枚举/算术等
					ExprPtr dim = parse_assign();
					int64_t v = dim ? eval_const_int(dim) : -1;
					sz = (v > 0) ? static_cast<std::size_t>(v) : 0; // <=0 视为柔性/未定大小
				}
			}
			expect_punct("]");
			dims.push_back(sz);
		}
		return dims;
	}

	// 数组后缀薄封装: [N] [N][M] ... → Array(N, Array(M, elem))（反向构建，最内层维先）
	CTypePtr parse_array_suffix(CTypePtr elem_type)
	{
		std::vector<std::size_t> dims = parse_bracket_dims();
		for (int i = static_cast<int>(dims.size()) - 1; i >= 0; --i)
		{
			auto arr = std::make_shared<CType>();
			arr->kind = TypeKind::Array;
			arr->base = elem_type;
			arr->array_size = dims[i];
			elem_type = arr;
		}
		return elem_type;
	}

	// —— 统一 C 声明符（Decorator 递归下降，Phase A）——
	// 声明符解析为「类型装饰函数」D: CTypePtr->CTypePtr，并回填最内层 name；最终类型 = D(base)。
	// 组合律（正确区分 *p[4]=Array(Ptr) 与 (*p)[4]=Ptr(Array)）：
	//   前缀 *   : 结果(X) = rest(Pointer(X))
	//   后缀 [N] : 结果(X) = prev(Array(X,N))
	//   后缀 ()  : 结果(X) = prev(Function(X,params,variadic))
	//   分组 ()  : prev = 递归解析括号内声明符
	using DeclaratorFn = std::function<CTypePtr(CTypePtr)>;

	DeclaratorFn parse_declarator_rec(std::string &name, std::vector<Param>* own = nullptr)
	{
		if (check_punct("*"))
		{
			advance();
			bool cst = false;
			while (match_keyword("const") || match_keyword("volatile") ||
				match_keyword("restrict") || match_keyword("_Atomic"))
				cst = true;
			DeclaratorFn rest = parse_declarator_rec(name, own);
			return [rest, cst](CTypePtr X) -> CTypePtr
			{
				CTypePtr p = make_pointer(X);
				if (cst) p->is_const = true;
				return rest(p);
			};
		}
		return parse_direct_rec(name, own);
	}

	DeclaratorFn parse_direct_rec(std::string &name, std::vector<Param>* own = nullptr)
	{
		DeclaratorFn cur;
		bool ident_here = false; // 本帧是否消费了标识符（其紧邻参数表=被定义函数自身参数）
		if (check_punct("("))
		{
			advance();
			cur = parse_declarator_rec(name, own); // 括号内是一个完整声明符
			expect_punct(")");
		}
		else if (peek().kind == TokKind::Identifier)
		{
			name = advance().text;
			ident_here = true;
			cur = [](CTypePtr X) -> CTypePtr { return X; };
		}
		else
		{
			// 抽象声明符（无名）
			cur = [](CTypePtr X) -> CTypePtr { return X; };
		}
		for (;;)
		{
			if (check_punct("["))
			{
				std::vector<std::size_t> dims = parse_bracket_dims();
				DeclaratorFn prev = cur;
				cur = [prev, dims](CTypePtr X) -> CTypePtr
				{
					CTypePtr t = X;
					for (int i = static_cast<int>(dims.size()) - 1; i >= 0; --i)
					{
						auto arr = std::make_shared<CType>();
						arr->kind = TypeKind::Array;
						arr->base = t;
						arr->array_size = dims[i];
						t = arr;
					}
					return prev(t);
				};
			}
			else if (check_punct("("))
			{
				advance(); // '('
				std::vector<Param> ps;
				bool variadic = false;
				parse_param_list(ps, variadic);
				expect_punct(")");
				// 紧邻标识符的参数表 = 被定义函数自身参数（带名），回填供函数定义绑定体作用域。
				//   仅记首个（分组外层 (c,d) 因 ident_here=false 不入选）。
				if (ident_here && own && own->empty()) *own = ps;
				std::vector<CTypePtr> ptypes;
				for (auto &p : ps) ptypes.push_back(p.type);
				DeclaratorFn prev = cur;
				cur = [prev, ptypes, variadic](CTypePtr X) -> CTypePtr
				{
					return prev(make_func_type(X, ptypes, variadic));
				};
			}
			else break;
		}
		return cur;
	}

	// 前瞻：声明符是否为「(*[qual]... Identifier」且该标识符后紧跟 '('（函数声明/定义形式）。
	// 命中走保留具名参数与函数体的专用路径；其余（分组/指针到数组/函数指针对象等）走 Decorator。
	bool is_function_form() const
	{
		std::size_t k = 0;
		for (;;)
		{
			const Token &t = peek(k);
			if (t.kind == TokKind::Punct && t.text == "*") { ++k; continue; }
			if (t.kind == TokKind::Keyword &&
				(t.text == "const" || t.text == "volatile" || t.text == "restrict" || t.text == "_Atomic"))
			{ ++k; continue; }
			break;
		}
		if (peek(k).kind != TokKind::Identifier) return false;
		const Token &nx = peek(k + 1);
		return nx.kind == TokKind::Punct && nx.text == "(";
	}

	// 解析一个全局声明符项（对象 / 数组 / 函数指针对象 / 函数原型），登记入 tu。
	// 不消费分隔 ',' 或结尾 ';'（由调用方循环处理）。
	void parse_global_declarator_item(TranslationUnit &tu, CTypePtr base_type,
		bool is_static, bool is_extern, unsigned ga, const SourceLoc &loc)
	{
		std::string nm;
		std::vector<Param> own; // 紧邻标识符的具名参数（分组/返回函数指针的函数定义用）
		DeclaratorFn d = parse_declarator_rec(nm, &own);
		CTypePtr type = d(base_type);
		if (type->kind == TypeKind::Function)
		{
			// 尾缀构成函数 → 函数原型或定义（分组形 int (*f1(int a,int b))(int c,int d){...}）
			FuncDecl fn;
			fn.loc = loc;
			fn.name = std::move(nm);
			fn.return_type = type->base;
			if (!own.empty() && own.size() == type->params.size())
				fn.params = own; // 带名参数（供函数体绑定 a/b）
			else
				for (auto &pt : type->params)
				{
					Param p;
					p.type = pt;
					fn.params.push_back(std::move(p));
				}
			fn.is_variadic = type->is_variadic;
			fn.is_static = is_static;
			func_return_types_[fn.name] = fn.return_type;
			if (check_punct("{")) // 函数定义：消费函数体（原型/声明以 ; 结尾，不进此分支）
				fn.body = parse_compound_stmt();
			tu.funcs.push_back(std::move(fn));
			return;
		}
		GlobalVar gv;
		gv.loc = loc;
		gv.name = std::move(nm);
		gv.type = type;
		gv.align = ga;
		gv.is_static = is_static;
		gv.is_extern = is_extern;
		if (match_punct("="))
			gv.init = check_punct("{") ? parse_init_list() : parse_assign();
		deduce_array_size(gv.type, gv.init);
		tu.globals.push_back(std::move(gv));
	}

	// 按 tag 查 StructDef（brace elision 维度推导用）。
	const StructDef *find_structdef(const std::string &tag)
	{
		if (tag.empty()) return nullptr;
		for (const auto &s : cur_tu_->structs)
			if (s.tag == tag) return &s;
		return nullptr;
	}

	// brace elision 用：一个该类型元素在「无外层花括号」扁平初始化中消耗的标量叶子数。
	// 标量/指针/函数/枚举/位域=1；数组=array_size×leaves(base)；struct=Σ成员；union=首成员。
	std::size_t leaves_per_type(const CTypePtr &t, int depth = 0)
	{
		if (!t || depth > 32) return 1;
		switch (t->kind)
		{
		case TypeKind::Array:
		{
			std::size_t n = t->array_size ? t->array_size : 1; // 内层未定维(elision 罕见)按 1
			return n * leaves_per_type(t->base, depth + 1);
		}
		case TypeKind::Struct:
		case TypeKind::Union:
		{
			const StructDef *sd = find_structdef(t->tag);
			if (!sd || sd->members.empty()) return 1;
			if (sd->is_union) return leaves_per_type(sd->members[0].type, depth + 1);
			std::size_t sum = 0;
			for (const auto &m : sd->members) sum += leaves_per_type(m.type, depth + 1);
			return sum;
		}
		default:
			return 1;
		}
	}

	// 省略大小的数组 T name[] = {…} / char name[] = "str"：从初始化式推导大小。
	// 仅处理最外层维且当前大小为 0（显式给定大小不覆盖；柔性数组成员无初始化式不受影响）。
	void deduce_array_size(CTypePtr &ty, const ExprPtr &init)
	{
		if (!ty || ty->kind != TypeKind::Array) return;
		if (ty->array_size != 0) return;
		if (!init) return;
		if (init->kind == ExprKind::InitList)
		{
			// 指定式护栏：任一 desig_index/desig_field → 维持既有「max(下标)+1」逻辑（P169 等逐字不变）。
			bool has_desig = false;
			for (const auto &ae : init->args)
				if (ae && (ae->desig_index >= 0 || !ae->desig_field.empty())) { has_desig = true; break; }
			if (!has_desig)
			{
				// brace elision 计数（P173）：元素为聚合且首项非花括号 → 扁平标量按每元素 L 个叶子分组。
				std::size_t L = leaves_per_type(ty->base);
				if (L > 1 && !init->args.empty() && init->args[0]->kind != ExprKind::InitList)
					ty->array_size = (init->args.size() + L - 1) / L;
				else
					ty->array_size = init->args.size(); // 标量元素或逐元素带括号 → 每项一元素
				return;
			}
			// §6.7.9: 省略大小时，数组大 = (最大下标)+1；需考虑 `[N]` 指定符。
			//   位置式初始化按序占位，指定式 `[N]=` 跳转至 N且下一位置从 N+1 起。
			//   不能用 args.size()（如 {5,[2]=2,3} 为 3 个元素但最大下标 3 → 应 4）。
			std::size_t next_pos = 0, maxv = 0;
			for (const auto &ae : init->args)
			{
				std::size_t idx = (ae && ae->desig_index >= 0)
					? static_cast<std::size_t>(ae->desig_index) : next_pos;
				next_pos = idx + 1;
				if (next_pos > maxv) maxv = next_pos;
			}
			ty->array_size = static_cast<long long>(maxv);
			return;
		}
		// §6.7.9p14：字符数组由字符串字面量初始化时，大小为内容元素数 + 1（含 '\0'）。
		// 按数组元素宽度分派编码（前缀无关，沿用 P177「按元素宽度驱动」）：
		//   1 字节（char/char8_t）→ UTF-8 字节数；2 字节（wchar_t/char16_t）→ UTF-16 码元数；
		//   4 字节（char32_t）→ UTF-32 码点数（修 plct 00220 同域、补全 charN_t）。
		if (init->kind == ExprKind::StringLiteral)
		{
			const int64_t ew = ty->base ? const_sizeof_type(ty->base) : 1;
			if (ew >= 4)      ty->array_size = static_cast<int64_t>(co_decode_utf32(init->str_val).size() + 1);
			else if (ew >= 2) ty->array_size = static_cast<int64_t>(co_decode_utf16(init->str_val).size() + 1);
			else              ty->array_size = static_cast<int64_t>(co_decode_string(init->str_val).size() + 1);
			return;
		}
	}

	bool is_type_start() const
	{
		const Token &t = peek();
		if (t.kind != TokKind::Keyword && t.kind != TokKind::Identifier)
			return false;
		if (t.text == "void" || t.text == "char" || t.text == "wchar_t" ||
			t.text == "char8_t" || t.text == "char16_t" || t.text == "char32_t" ||
			t.text == "short" ||
			t.text == "int" || t.text == "long" || t.text == "float" ||
			t.text == "double" || t.text == "signed" || t.text == "unsigned" ||
			t.text == "const" || t.text == "struct" || t.text == "union" ||
			t.text == "enum" || t.text == "typedef" ||
			t.text == "static" || t.text == "extern" || t.text == "register" ||
			t.text == "_Bool" || t.text == "inline" || t.text == "restrict" ||
			t.text == "_Inline" || t.text == "_Noreturn" ||
			t.text == "_Alignas" || t.text == "alignas" ||
			t.text == "_Thread_local" || t.text == "thread_local" ||
			t.text == "_Static_assert" || t.text == "_Atomic" ||
			t.text == "auto" || t.text == "bool" || t.text == "constexpr" ||
			t.text == "typeof" || t.text == "typeof_unqual" || t.text == "_BitInt" ||
			t.text == "thread_local")
			return true;
		// typedef 名
		return typedefs_.count(t.text) > 0;
	}

	// 判断当前 '('_后_一个_token_是否是类型关键字或 typedef 名（用于 cast 消歧）
	bool is_type_after_paren() const
	{
		const Token &t = peek(1);
		if (t.kind != TokKind::Keyword && t.kind != TokKind::Identifier)
			return false;
		if (t.text == "void" || t.text == "char" || t.text == "wchar_t" ||
			t.text == "char8_t" || t.text == "char16_t" || t.text == "char32_t" ||
			t.text == "short" ||
			t.text == "int" || t.text == "long" || t.text == "float" ||
			t.text == "double" || t.text == "signed" || t.text == "unsigned" ||
			t.text == "const" || t.text == "struct" || t.text == "union" ||
			t.text == "enum" || t.text == "_Bool" || t.text == "bool" ||
			t.text == "_BitInt" || t.text == "typeof" || t.text == "typeof_unqual" ||
			t.text == "constexpr" || t.text == "_Atomic")
			return true;
		return typedefs_.count(t.text) > 0;
	}

	// 站在声明符起始 '('：判断其后是否为函数指示符参数表（vs 分组声明符）。§6.7.6p6
	bool starts_parameter_type_list() const
	{
		const Token &n = peek(1);
		if (n.kind == TokKind::Punct && (n.text == ")" || n.text == "..."))
			return true; // 空参数表 / 仅省略号 → 函数型
		return is_type_after_paren(); // 类型关键字 / typedef 名 / const/_Atomic 等
	}

	// ============================================================
	// 表达式解析（逗号 → 赋值 → 三元 → 二元优先级爬升 → 一元 → 后缀 → 基元）
	// ============================================================
	ExprPtr parse_expr() { return parse_comma_expr(); }

	// 逗号表达式（仅用于顶层 expression-statement 和 return）
	ExprPtr parse_comma_expr()
	{
		auto first = parse_assign();
		if (!first) return nullptr;
		if (!check_punct(","))
			return first;
		auto e = std::make_shared<Expr>();
		e->kind = ExprKind::Comma;
		e->loc = first->loc;
		e->args.push_back(std::move(first));
		while (match_punct(","))
		{
			auto next = parse_assign();
			if (next) e->args.push_back(std::move(next));
		}
		return e;
	}

	ExprPtr parse_assign()
	{
		auto lhs = parse_conditional();
		if (!lhs) return nullptr;
		if (peek().kind == TokKind::Punct)
		{
			const std::string &op = peek().text;
			if (op == "=" || op == "+=" || op == "-=" || op == "*=" ||
				op == "/=" || op == "%=" || op == "<<=" || op == ">>=" ||
				op == "&=" || op == "|=" || op == "^=")
			{
				auto e = std::make_shared<Expr>();
				e->kind = ExprKind::Assign;
				e->loc = advance().loc;
				e->op = op;
				e->lhs = std::move(lhs);
				e->rhs = parse_assign(); // right-assoc
				return e;
			}
		}
		return lhs;
	}

	// 三元运算符: cond ? then_expr : else_expr
	ExprPtr parse_conditional()
	{
		auto lhs = parse_binary(1);
		if (!lhs) return nullptr;
		if (!check_punct("?"))
			return lhs;
		auto e = std::make_shared<Expr>();
		e->kind = ExprKind::Ternary;
		e->loc = lhs->loc;
		e->lhs = std::move(lhs);
		advance(); // consume '?'
		e->args.push_back(parse_expr()); // then 允许逗号
		expect_punct(":");
		e->args.push_back(parse_conditional()); // else 递归三元（无逗号）
		return e;
	}

	ExprPtr parse_binary(int min_prec)
	{
		auto lhs = parse_unary();
		while (lhs && peek().kind == TokKind::Punct)
		{
			const int prec = binop_prec(peek().text);
			if (prec == 0 || prec < min_prec) break;
			std::string op = advance().text;
			auto rhs = parse_binary(prec + 1);
			auto e = std::make_shared<Expr>();
			e->kind = ExprKind::Binary;
			e->loc = peek().loc;
			e->op = op;
			e->lhs = std::move(lhs);
			e->rhs = std::move(rhs);
			lhs = std::move(e);
		}
		return lhs;
	}

	static int binop_prec(const std::string &op)
	{
		if (op == "*" || op == "/" || op == "%") return 13;
		if (op == "+" || op == "-") return 12;
		if (op == "<<" || op == ">>") return 11;
		if (op == "<" || op == ">" || op == "<=" || op == ">=") return 10;
		if (op == "==" || op == "!=") return 9;
		if (op == "&") return 8;
		if (op == "^") return 7;
		if (op == "|") return 6;
		if (op == "&&") return 5;
		if (op == "||") return 4;
		return 0;
	}

	ExprPtr parse_unary()
	{
		// 显式类型转换: (type-specifier) unary-expr
		if (check_punct("(") && is_type_after_paren())
		{
			auto e = std::make_shared<Expr>();
			e->kind = ExprKind::Cast;
			e->loc = peek().loc;
			advance(); // consume '('
			e->type = parse_decl_specifiers();
			// 抽象声明符（Decorator）：支持 void (*)(void) 等函数指针/数组 cast 类型
			{
				std::string dummy_nm;
				DeclaratorFn d = parse_declarator_rec(dummy_nm);
				e->type = d(e->type);
			}
			expect_punct(")");
			// 复合字面量: (type){init-list}
			if (check_punct("{"))
			{
				// 复合字面量: (type){init-list}
				auto lit = parse_init_list(); // parse_init_list 已处理 '{...}'
				lit->type = e->type;
				lit->loc = e->loc;
				return lit;
			}
			e->lhs = parse_unary();
			return e;
		}
		if (peek().kind == TokKind::Punct)
		{
			const std::string &op = peek().text;
			if (op == "-" || op == "+" || op == "!" || op == "~" || op == "*" || op == "&")
			{
				auto e = std::make_shared<Expr>();
				e->kind = ExprKind::Unary;
				e->loc = advance().loc;
				e->op = op;
				e->lhs = parse_unary();
				return e;
			}
			if (op == "++" || op == "--")
			{
				auto e = std::make_shared<Expr>();
				e->kind = ExprKind::Unary;
				e->loc = advance().loc;
				e->op = op;
				e->postfix = false;
				e->lhs = parse_unary();
				return e;
			}
		}
		// _Alignof(type) / alignof(type)
		if (check_keyword("_Alignof") || check_keyword("alignof"))
		{
			auto e = std::make_shared<Expr>();
			e->kind = ExprKind::Unary;
			e->loc = advance().loc;
			e->op = "alignof";
			expect_punct("(");
			e->type = parse_decl_specifiers();
			e->type = parse_pointer_suffix(e->type);
			if (check_punct("["))
				e->type = parse_array_suffix(e->type);
			expect_punct(")");
			return e;
		}
		if (check_keyword("sizeof"))
		{
			auto e = std::make_shared<Expr>();
			e->kind = ExprKind::Unary;
			e->loc = advance().loc;
			e->op = "sizeof";
			if (check_punct("("))
			{
				// 尝试 sizeof(type): ( 后跟类型名则为类型
				if (is_type_after_paren())
				{
					advance(); // consume '('
					e->type = parse_decl_specifiers();
					e->type = parse_pointer_suffix(e->type);
					expect_punct(")");
				}
				else
				{
					advance();
					e->lhs = parse_expr();
					expect_punct(")");
				}
			}
			else
				e->lhs = parse_unary();
			return e;
		}
		// _Generic(select, type1: expr1, type2: expr2, ..., default: exprN)
		if (check_keyword("_Generic"))
		{
			auto e = std::make_shared<Expr>();
			e->kind = ExprKind::Generic;
			e->loc = advance().loc; // consume '_Generic'
			expect_punct("(");
			e->lhs = parse_assign(); // controlling expression
			while (match_punct(","))
			{
				auto assoc = std::make_shared<Expr>();
				if (check_keyword("default"))
				{
					advance(); // 'default'
					assoc->type = nullptr; // null = default
				}
				else
				{
					assoc->type = parse_decl_specifiers();
					assoc->type = parse_pointer_suffix(assoc->type);
					if (check_punct("["))
						assoc->type = parse_array_suffix(assoc->type);
				}
				expect_punct(":");
				assoc->rhs = parse_assign();
				e->args.push_back(std::move(assoc));
			}
			expect_punct(")");
			return apply_postfix(e); // _Generic 结果纳入 postfix：支持 _Generic(...)() 调用
		}
		return parse_postfix();
	}

	// 对已解析的前缀表达式 e 依次施加后缀运算符（调用/下标/成员/自增）。
	//   抽自 parse_postfix，使 _Generic 结果也能参与 postfix（如 _Generic(...)()）。
	ExprPtr apply_postfix(ExprPtr e)
	{
		for (;;)
		{
			if (check_punct("("))
			{
				auto call = std::make_shared<Expr>();
				call->kind = ExprKind::Call;
				call->loc = e->loc;
				if (e->kind == ExprKind::VarRef)
					call->name = e->name;
				else
					call->lhs = e; // 间接被调表达式（如 (*fp)(...) ）
				advance();
				// 变参内建 va_arg(ap, type)：第二实参是类型，按 sizeof(type) 同样方式解析
				if (call->name == "va_arg" && !call->lhs)
				{
					auto ap = parse_assign();
					if (ap) call->args.push_back(std::move(ap));
					if (!match_punct(","))
					{
						error(peek().loc, "va_arg expects (ap, type)");
						expect_punct(")");
						e = std::move(call);
						continue;
					}
					CTypePtr t = parse_decl_specifiers();
					t = parse_pointer_suffix(t);
					if (check_punct("["))
						t = parse_array_suffix(t);
					call->type = t;
					expect_punct(")");
					e = std::move(call);
					continue;
				}
				// 内建 __builtin_offsetof(type, designator)：编译期求字节偏移（供数组长度/case 等常量上下文）。
				// 直接展开为携带 size_t 类型的整型字面量常量，零 codegen 依赖。
				if (call->name == "__builtin_offsetof" && !call->lhs)
				{
					CTypePtr cur = parse_decl_specifiers();
					cur = parse_pointer_suffix(cur);
					if (check_punct("["))
						cur = parse_array_suffix(cur);
					if (!match_punct(","))
					{
						error(peek().loc, "__builtin_offsetof expects (type, member)");
						expect_punct(")");
						e = std::move(call);
						continue;
					}
					int64_t off = 0;
					bool bad = false;
					// 成员访问：在当前 struct/union 上累加对齐填充后的偏移，并切换到成员类型
					auto access_member = [&](const std::string &mname) -> bool {
						if (!cur || (cur->kind != TypeKind::Struct && cur->kind != TypeKind::Union)) return false;
						const StructDef *sd = nullptr;
						for (const auto &s : cur_tu_->structs)
							if (s.tag == cur->tag) { sd = &s; break; }
						if (!sd) return false;
						if (sd->is_union)
						{
							for (const auto &m : sd->members)
								if (m.name == mname) { cur = m.type; return true; }
							return false;
						}
						int64_t moff = 0; CTypePtr mt; bool found = false;
						for (const auto &m : sd->members)
						{
							int64_t ma = const_member_align(m);
							if (ma < 1) ma = 1;
							moff = ((moff + ma - 1) / ma) * ma;
							if (m.name == mname) { mt = m.type; found = true; break; }
							moff += const_sizeof_type(m.type);
						}
						if (!found) return false;
						off += moff; cur = mt; return true;
					};
					// 数组下标：按元素大小前移，并切换到元素类型
					auto access_index = [&](int64_t idx) -> bool {
						if (!cur || (cur->kind != TypeKind::Array && cur->kind != TypeKind::Pointer)) return false;
						off += idx * const_sizeof_type(cur->base);
						cur = cur->base; return true;
					};
					{
						auto id = expect_identifier();
						if (!access_member(id.text)) bad = true;
					}
					while (!check_punct(")"))
					{
						if (match_punct("."))
						{
							auto id = expect_identifier();
							if (!access_member(id.text)) bad = true;
						}
						else if (match_punct("["))
						{
							int64_t iv = eval_const_int(parse_assign());
							expect_punct("]");
							if (!access_index(iv)) bad = true;
						}
						else { bad = true; break; }
					}
					expect_punct(")");
					if (bad)
					{
						error(call->loc, "__builtin_offsetof: invalid type or member designator");
						off = 0;
					}
					auto lit = std::make_shared<Expr>();
					lit->kind = ExprKind::IntLiteral;
					lit->loc = call->loc;
					lit->int_val = off;
					auto st = std::make_shared<CType>();
					st->kind = TypeKind::LongLong; // size_t（LLP64 无符号 64 位）
					st->is_unsigned = true;
					lit->type = st;
					e = std::move(lit);
					continue;
				}
				if (!check_punct(")"))
				{
					for (;;)
					{
						auto arg = parse_assign(); // 实参不含逗号运算符
						if (arg) call->args.push_back(std::move(arg));
						if (!match_punct(",")) break;
					}
				}
				expect_punct(")");
				e = std::move(call);
				continue;
			}
			if (check_punct("["))
			{
				auto idx = std::make_shared<Expr>();
				idx->kind = ExprKind::Binary;
				idx->loc = e->loc;
				idx->op = "[]";
				idx->lhs = std::move(e);
				advance();
				idx->rhs = parse_expr();
				expect_punct("]");
				e = std::move(idx);
				continue;
			}
			if (check_punct("++") || check_punct("--"))
			{
				auto inc = std::make_shared<Expr>();
				inc->kind = ExprKind::Unary;
				inc->loc = e->loc;
				inc->op = advance().text;
				inc->postfix = true;
				inc->lhs = std::move(e);
				e = std::move(inc);
				continue;
			}
			if (check_punct(".") || check_punct("->"))
			{
				auto mem = std::make_shared<Expr>();
				mem->kind = ExprKind::Member;
				mem->loc = e->loc;
				mem->op = advance().text; // "." or "->"
				mem->lhs = std::move(e);
				mem->name = expect_identifier().text;
				// type 字段存结构体类型（codegen 用）
				mem->type = nullptr; // 后续由 sema 或 codegen 推断
				e = std::move(mem);
				continue;
			}
			break;
		}
		return e;
	}

	ExprPtr parse_postfix()
	{
		auto e = parse_primary();
		if (!e) return nullptr;
		return apply_postfix(std::move(e));
	}

	// 解析十六进制浮点字面量 "0x1.8p3" → double
	static double parse_hex_float(const std::string &s)
	{
		// 去掉 0x/0X 前缀
		std::size_t pos = 2;
		double mantissa = 0.0;
		// 解析整数部分
		while (pos < s.size())
		{
			char c = s[pos];
			int d;
			if (c >= '0' && c <= '9') d = c - '0';
			else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
			else break;
			mantissa = mantissa * 16.0 + d;
			++pos;
		}
		// 小数部分
		if (pos < s.size() && s[pos] == '.')
		{
			++pos;
			double frac = 1.0 / 16.0;
			while (pos < s.size())
			{
				char c = s[pos];
				int d;
				if (c >= '0' && c <= '9') d = c - '0';
				else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
				else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
				else break;
				mantissa += d * frac;
				frac /= 16.0;
				++pos;
			}
		}
		// 指数部分 p/P[+/-]digits
		if (pos < s.size() && (s[pos] == 'p' || s[pos] == 'P'))
		{
			++pos;
			int exp_sign = 1;
			if (pos < s.size() && (s[pos] == '+' || s[pos] == '-'))
			{
				if (s[pos] == '-') exp_sign = -1;
				++pos;
			}
			int expo = 0;
			while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
			{
				expo = expo * 10 + (s[pos] - '0');
				++pos;
			}
			mantissa *= std::ldexp(1.0, exp_sign * expo);
		}
		return mantissa;
	}

	ExprPtr parse_primary()
	{
		const Token &t = peek();
		if (t.kind == TokKind::Number)
		{
			advance();
			auto e = std::make_shared<Expr>();
			e->loc = t.loc;
			bool is_hex = (t.text.size() > 2 && t.text[0] == '0' &&
				(t.text[1] == 'x' || t.text[1] == 'X'));
			// 检测十六进制浮点: 0x...p/P...
			bool is_hexfloat = is_hex &&
				(t.text.find('p') != std::string::npos || t.text.find('P') != std::string::npos);
			// 检测普通浮点: 含 '.' 或 e/E指数(非 hex)
			bool is_float = is_hexfloat ||
				(t.text.find('.') != std::string::npos && !is_hex) ||
				(!is_hex && (t.text.find('e') != std::string::npos ||
					t.text.find('E') != std::string::npos));
			if (is_hexfloat || is_float)
			{
				e->kind = ExprKind::FloatLiteral;
				e->float_val = is_hexfloat ? parse_hex_float(t.text) : std::stod(t.text);
				// C §6.4.5 浮点后缀定型（P9.24）：f/F → float；无后缀 / l/L → double。
				//   （本机 Windows long double==double，故 L 亦落 double，与 clang -std=c23 一致。
				//    std::stod / parse_hex_float 均自然忽略尾随 f/L，值解析无须剥后缀。）
				const char lastc = t.text.empty() ? '\0' : t.text.back();
				if (lastc == 'f' || lastc == 'F')
					e->type = make_type(TypeKind::Float);
			}
			else
			{
				e->kind = ExprKind::IntLiteral;
				// C23 位精确整数字面量后缀：wb/WB → signed _BitInt(N)，ub/UB → unsigned _BitInt(N)。
				// N 为表示该值所需的最小位宽（signed 额外含符号位）。
				const std::string &txt = t.text;
				const std::size_t tl = txt.size();
				bool bi = false, bi_unsigned = false;
				std::string digits = txt;
				if (tl >= 2)
				{
					const char a = txt[tl - 2], b = txt[tl - 1];
					if ((b == 'b' || b == 'B') && (a == 'w' || a == 'W'))
					{
						bi = true; bi_unsigned = false; digits = txt.substr(0, tl - 2);
					}
					else if ((b == 'b' || b == 'B') && (a == 'u' || a == 'U'))
					{
						bi = true; bi_unsigned = true; digits = txt.substr(0, tl - 2);
					}
				}
				unsigned long long mag = 0ULL;
				{
					// 规范化 C23 前缀 0b/0B(二进制)、0o/0O(八进制)——std::stoull base0 不识别这两者；
					// 0x / 前导 0 八进制 / 十进制仍交给 base0 自动判定。
					std::string num = digits;
					int rbase = 0;
					if (num.size() >= 2 && num[0] == '0')
					{
						const char c1 = num[1];
						if (c1 == 'b' || c1 == 'B') { rbase = 2; num = num.substr(2); }
						else if (c1 == 'o' || c1 == 'O') { rbase = 8; num = num.substr(2); }
					}
					if (!num.empty()) mag = std::stoull(num, nullptr, rbase);
				}
				e->int_val = static_cast<long long>(mag);
				if (bi)
				{
					// bitlen(mag)：表示 mag 所需的二进制位数（mag==0 计 1）
					unsigned n = 1;
					for (unsigned long long x = mag; x >= 2; x >>= 1) ++n;
					// signed 额外含符号位（mag==0 保留 _BitInt(1)）
					if (!bi_unsigned && mag != 0) ++n;
					auto bt = std::make_shared<CType>();
					bt->kind = TypeKind::BitInt;
					bt->bit_width = n;
					bt->is_unsigned = bi_unsigned;
					e->type = bt;
				}
				else
				{
					// C §6.4.5.2：按后缀(u/U, l/L, ll/LL)与进制(十进制 vs 八/十六进制)，
					// 从候选类型序列中选出能容纳该值的最小类型（LLP64：int=32, long=32, long long=64）。
					// 十进制候选恒不含无符号型；八/十六进制可选到 unsigned int/long/long long。
					std::size_t sb = tl;
					while (sb > 0)
					{
						char c = txt[sb - 1];
						if (c == 'u' || c == 'U' || c == 'l' || c == 'L') --sb;
						else break;
					}
					bool uns = false; unsigned lcnt = 0;
					if (sb < tl)
					{
						std::string sfx = txt.substr(sb);
						for (char c : sfx) { if (c == 'u' || c == 'U') uns = true; else ++lcnt; }
					}
					// 十进制字面量不以 '0' 开头（单独 "0" 视作十进制）
					bool decimal = !(txt.size() > 1 && txt[0] == '0');
					struct Cand { TypeKind tk; bool uns; unsigned bits; };
					std::vector<Cand> list;
					if (uns && lcnt == 0)
						list = { {TypeKind::Int,true,32},{TypeKind::Long,true,32},{TypeKind::LongLong,true,64} };
					else if (uns && lcnt >= 2) // ull / ULL 等（两个 l + u）：§6.4.5.2 候选仅 unsigned long long
						list = { {TypeKind::LongLong,true,64} };
					else if (uns) // ul / uL（一个 l + u）
						list = { {TypeKind::Long,true,32},{TypeKind::LongLong,true,64} };
					else if (lcnt >= 2) // ll / LL
						list = { {TypeKind::LongLong,false,64} };
					else if (lcnt == 1) // l / L
						list = { {TypeKind::Long,false,32},{TypeKind::LongLong,false,64} };
					else if (decimal) // 无后缀十进制
						list = { {TypeKind::Int,false,32},{TypeKind::Long,false,32},{TypeKind::LongLong,false,64} };
					else // 无后缀八/十六进制
						list = { {TypeKind::Int,false,32},{TypeKind::Int,true,32},{TypeKind::Long,false,32},
						         {TypeKind::Long,true,32},{TypeKind::LongLong,false,64},{TypeKind::LongLong,true,64} };
					Cand chosen = list.back();
					for (const Cand &cd : list)
					{
						bool fits;
						if (cd.bits == 64)
							fits = cd.uns ? true : (mag <= 0x7FFFFFFFFFFFFFFFULL);
						else
							fits = cd.uns ? (mag <= 0xFFFFFFFFULL) : (mag <= 0x7FFFFFFFULL);
						if (fits) { chosen = cd; break; }
					}
					auto lt = std::make_shared<CType>();
					lt->kind = chosen.tk;
					lt->is_unsigned = chosen.uns;
					e->type = lt;
				}
			}
			return e;
		}
		if (t.kind == TokKind::String)
		{
			advance();
			auto e = std::make_shared<Expr>();
			e->kind = ExprKind::StringLiteral;
			e->loc = t.loc;
			e->str_val = t.text;
			return e;
		}
		// C23: nullptr / true / false
		if (t.kind == TokKind::Keyword)
		{
			if (t.text == "nullptr")
			{
				advance();
				auto e = std::make_shared<Expr>();
				e->kind = ExprKind::IntLiteral;
				e->loc = t.loc;
				e->int_val = 0;
				e->type = make_pointer(make_type(TypeKind::Void));
				return e;
			}
			if (t.text == "true")
			{
				advance();
				auto e = std::make_shared<Expr>();
				e->kind = ExprKind::IntLiteral;
				e->loc = t.loc;
				e->int_val = 1;
				return e;
			}
			if (t.text == "false")
			{
				advance();
				auto e = std::make_shared<Expr>();
				e->kind = ExprKind::IntLiteral;
				e->loc = t.loc;
				e->int_val = 0;
				return e;
			}
		}
		if (t.kind == TokKind::Identifier)
		{
			// enum 常量名优先匹配
			auto ec = enum_constants_.find(t.text);
			if (ec != enum_constants_.end())
			{
				advance();
				auto e = std::make_shared<Expr>();
				e->kind = ExprKind::IntLiteral;
				e->loc = t.loc;
				e->int_val = ec->second;
				return e;
			}
			advance();
			auto e = std::make_shared<Expr>();
			e->kind = ExprKind::VarRef;
			e->loc = t.loc;
			e->name = t.text;
			return e;
		}
		if (t.kind == TokKind::Char)
		{
			advance();
			auto e = std::make_shared<Expr>();
			e->kind = ExprKind::CharLiteral;
			e->loc = t.loc;
			e->str_val = t.text;
			e->int_val = co_decode_char(t.text); // 解码真实码值：供 eval_const_int/静态初始化/codegen 统一取用
			return e;
		}
		if (check_punct("("))
		{
			advance();
			auto e = parse_expr();
			expect_punct(")");
			return e;
		}
		error(t.loc, "unexpected token '" + t.text + "' in expression");
		return nullptr;
	}

	// —— 错误恢复 ——
	void skip_to_semi()
	{
		while (!at_end() && !check_punct(";"))
		{
			advance();
		}
		match_punct(";");
	}
	void skip_to_semi_or_brace()
	{
		while (!at_end() && !check_punct(";") && !check_punct("}"))
		{
			advance();
		}
		match_punct(";");
	}
};

}  // namespace

// ============================================================
// 对外接口
// ============================================================
bool parse(const CompileOptions & /*opts*/, CompileState &state, std::vector<Error> &errors)
{
	Parser p(state.tokens, errors);
	state.ast = p.parse_translation_unit();
	return errors.empty();
}

}  // namespace colang
