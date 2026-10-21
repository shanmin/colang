// ============================================================
// sema.cpp — 语义分析（P9.35：validate-only 保守护栏诊断层）
//
// 架构：analyze() 作为 parse 与 generate 之间的**只读校验 pass**。自带轻量
// 符号表与类型推断，只做保守的硬约束检查并发 error 诊断；**不写 Expr.type、
// 不改 codegen**（codegen.cpp 的 get_expr_ctype 等保持独立，二者类型逻辑暂重复）。
//
// 首期检查（全 error，任一命中 → analyze 返回 false → driver 中断 → main 打印）：
//   1 未声明标识符  2 给不可修改值赋值  3 成员访问类别错  4 下标作用于非指针/数组
//   5 已知原型调用实参个数不符  6 break/continue 越界  7 case/default 越界
//   8 goto 目标缺失/标签重复  9 return 形态错误
// 保守原则：凡类型推断拿不准（resolve_type 返回 null）即跳过，绝不误伤合法方言。
// ============================================================

#include "colang/sema.h"

#include "colang/type_resolver.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace colang
{
namespace
{

class Sema
{
public:
	Sema(const CompileOptions &opts, std::vector<Error> &errors) : opts_(opts), errors_(errors) {}

	void run(const TUPtr &tu)
	{
		// 建全局符号表（指针指向 ast 内部，parse 后 vector 不再变动，稳定）
		for (const auto &f : tu->funcs) funcs_[f.name] = &f;
		for (const auto &g : tu->globals) globals_[g.name] = g.type;
		for (const auto &s : tu->structs)
			if (!s.tag.empty()) structs_[s.tag] = &s;

		// 仅检查函数体（全局初始化器暂不查，降低误伤面）
		for (const auto &f : tu->funcs)
		{
			if (f.body) check_func(f);
		}
	}

	bool ok() const { return !has_error_; }

private:
	// —— 诊断 ——
	void report(const SourceLoc &loc, const std::string &msg)
	{
		Error e;
		e.file = loc.file.empty() ? opts_.source_path : loc.file;
		e.pos.line = loc.line;
		e.pos.column = loc.column;
		e.message = msg;
		e.is_warning = false;
		errors_.push_back(std::move(e));
		has_error_ = true;
	}

	// —— 类型谓词 ——
	static bool is_struct_like(const CTypePtr &t)
	{
		return t && (t->kind == TypeKind::Struct || t->kind == TypeKind::Union);
	}
	static bool is_ptr_or_array(const CTypePtr &t)
	{
		return t && (t->kind == TypeKind::Pointer || t->kind == TypeKind::Array);
	}

	// nullptr：parser 以 Void* 建模的零字面量（int_val==0 且 type 为 Pointer）
	static bool is_nullptr(const ExprPtr &e)
	{
		return e && e->kind == ExprKind::IntLiteral && e->int_val == 0 &&
		       e->type && e->type->kind == TypeKind::Pointer;
	}

	// 名字是否已声明（局部/全局/函数/__func__）
	bool is_declared(const std::string &n)
	{
		if (n == "__func__") return true;
		for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
			if (it->count(n)) return true;
		return globals_.count(n) > 0 || funcs_.count(n) > 0;
	}

	// 类型解析单一真值源（P140 M2）：委托共享 resolve_expr_type，经 SemaTypeEnv 注入
	// sema 自有符号表。旧 resolve_type 的保守 null 面（算术/一元/字面量携带类型）现由
	// 共享解析器算出真类型 → 5 处检查触发面随之收紧（只多判、少漏判，绝不误伤合法程序）。
	// 注：旧 sema 对 Unary `*` 作用于 Array、以及 `i[a]` 反写下标的额外能力未保留（评估为
	//   不影响任何检查点操作数解析，回归为门验证）。
	struct SemaTypeEnv
	{
		Sema *self;
		CTypePtr var_type(const std::string &n)
		{
			for (auto it = self->scopes_.rbegin(); it != self->scopes_.rend(); ++it)
			{
				auto f = it->find(n);
				if (f != it->end()) return f->second;
			}
			auto g = self->globals_.find(n);
			if (g != self->globals_.end()) return g->second;
			return nullptr; // 内层→外层→全局，未找到返回 null
		}
		CTypePtr struct_member(const std::string &tag, const std::string &field)
		{
			return struct_member_impl(tag, field, 0);
		}
		// 成员名提升：直查不中时递归穿过匿名 struct/union 成员（匿名 union 保留嵌套）。
		CTypePtr struct_member_impl(const std::string &tag, const std::string &field, int depth)
		{
			if (depth > 16) return nullptr;
			auto it = self->structs_.find(tag);
			if (it == self->structs_.end()) return nullptr;
			for (const auto &m : it->second->members)
				if (!m.name.empty() && m.name == field) return m.type;
			for (const auto &m : it->second->members)
			{
				if (m.name.empty() && m.type
					&& (m.type->kind == TypeKind::Struct || m.type->kind == TypeKind::Union)
					&& !m.type->tag.empty())
				{
					if (CTypePtr r = struct_member_impl(m.type->tag, field, depth + 1))
						return r;
				}
			}
			return nullptr;
		}
		CTypePtr func_return(const std::string &n)
		{
			auto it = self->funcs_.find(n);
			if (it != self->funcs_.end()) return it->second->return_type;
			return nullptr;
		}
	};

	CTypePtr resolve_type(const ExprPtr &e)
	{
		SemaTypeEnv env{this};
		return resolve_expr_type(e, env);
	}

	// —— 表达式检查 ——
	void check_expr(const ExprPtr &e)
	{
		if (!e) return;
		switch (e->kind)
		{
		case ExprKind::VarRef:
			if (!is_declared(e->name))
				report(e->loc, "use of undeclared identifier '" + e->name + "'");
			break;
		case ExprKind::Assign:
		{
			const ExprPtr &L = e->lhs;
			if (L)
			{
				bool bad = L->kind == ExprKind::IntLiteral || L->kind == ExprKind::FloatLiteral ||
				           L->kind == ExprKind::CharLiteral || L->kind == ExprKind::StringLiteral ||
				           L->kind == ExprKind::Call;
				if (!bad && L->kind == ExprKind::VarRef)
				{
					CTypePtr t = resolve_type(L);
					if (t && t->kind == TypeKind::Array) bad = true; // 整数组赋值
				}
				if (bad) report(e->loc, "expression is not assignable");
			}
			// C23 模型 B：nullptr（nullptr_t）只能赋给指针类型；赋给类型已知的非指针左值是约束违例。
			if (L && is_nullptr(e->rhs))
			{
				CTypePtr lt = resolve_type(L);
				if (lt && lt->kind != TypeKind::Pointer)
					report(e->loc, "assigning 'nullptr' to incompatible type (not a pointer)");
			}
			break;
		}
		case ExprKind::Member:
		{
			CTypePtr base = resolve_type(e->lhs);
			if (base)
			{
				if (e->op == "." && !is_struct_like(base))
					report(e->loc, "member reference base type is not a structure or union");
				else if (e->op == "->" && !is_ptr_or_array(base))
					report(e->loc, "member reference base type is not a pointer to structure/union");
			}
			break;
		}
		case ExprKind::Binary:
		{
			if (e->op == "[]")
			{
				CTypePtr a = resolve_type(e->lhs);
				CTypePtr b = resolve_type(e->rhs);
				if (a && !is_ptr_or_array(a) && b && !is_ptr_or_array(b))
					report(e->loc, "subscript of expression which is not a pointer or array");
			}
			break;
		}
		case ExprKind::Call:
		{
			if (!e->lhs && !e->name.empty())
			{
				auto it = funcs_.find(e->name);
				if (it != funcs_.end())
				{
					const FuncDecl *f = it->second;
					// 仅查本 TU 内有定义（有 body）的函数：extern 头声明的签名可能带
					// codegen 注入的隐藏参数（如 setjmp.h 的 _setjmp 帧地址第二参），
					// arity 不可靠，一律跳过以免误伤。
					if (f->body && (!f->params.empty() || f->is_variadic))
					{
						const std::size_t np = f->params.size();
						const std::size_t na = e->args.size();
						if (f->is_variadic)
						{
							if (na < np)
								report(e->loc, "too few arguments to function '" + e->name + "'");
						}
						else if (na != np)
						{
							report(e->loc, std::string(na < np ? "too few arguments" : "too many arguments") +
							              " to function '" + e->name + "'");
						}
					}
				}
			}
			break;
		}
		default:
			break;
		}
		// 递归子表达式
		if (e->lhs) check_expr(e->lhs);
		if (e->rhs) check_expr(e->rhs);
		for (const auto &a : e->args)
			if (a) check_expr(a);
	}

	// —— 语句：收集标签（含重复检测）——
	void collect_labels(const StmtPtr &s)
	{
		if (!s) return;
		if (s->kind == StmtKind::Label)
		{
			if (!labels_.insert(s->label_name).second)
				report(s->loc, "duplicate case label '" + s->label_name + "'");
		}
		collect_labels(s->then_branch);
		collect_labels(s->else_branch);
		collect_labels(s->for_init);
		collect_labels(s->init_stmt);
		for (const auto &b : s->body) collect_labels(b);
	}

	// —— 语句检查 ——
	void check_stmt(const StmtPtr &s, int loop_d, int switch_d)
	{
		if (!s) return;
		switch (s->kind)
		{
		case StmtKind::Compound:
		{
			// 真块 {...}：压一层作用域，块内声明遮蔽外层、出块即失效。
			// 合成 Compound（parser 把多声明符 `int a=7,b=3;` 折成，is_block_scope=false）
			// 保持不压层、直接遍历 body，使多个声明符并入外层作用域。
			if (s->is_block_scope) scopes_.emplace_back();
			for (const auto &b : s->body) check_stmt(b, loop_d, switch_d);
			if (s->is_block_scope) scopes_.pop_back();
			break;
		}
		case StmtKind::Decl:
		{
			if (s->var_init) check_expr(s->var_init); // 初值先查（变量在自身初值内不可见）
			// C23 模型 B：nullptr 仅隐转指针类型；初始化非指针变量是约束违例。
			if (is_nullptr(s->var_init) && s->var_type && s->var_type->kind != TypeKind::Pointer)
				report(s->var_init->loc, "initializing incompatible type (not a pointer) with 'nullptr'");
			if (!scopes_.empty() && !s->var_name.empty())
				scopes_.back()[s->var_name] = s->var_type;
			break;
		}
		case StmtKind::ExprStmt:
			if (s->expr) check_expr(s->expr);
			break;
		case StmtKind::Return:
		{
			bool is_void = cur_func_ && cur_func_->return_type &&
				cur_func_->return_type->kind == TypeKind::Void;
			if (s->expr)
			{
				if (is_void)
				{
					CTypePtr t = resolve_type(s->expr);
					if (t && t->kind != TypeKind::Void)
						report(s->loc, "void function should not return a value");
				}
			}
			else if (cur_func_ && !is_void)
			{
				report(s->loc, "non-void function should return a value");
			}
			if (s->expr) check_expr(s->expr);
			break;
		}
		case StmtKind::If:
		{
			if (s->init_stmt) check_stmt(s->init_stmt, loop_d, switch_d);
			if (s->expr) check_expr(s->expr);
			check_stmt(s->then_branch, loop_d, switch_d);
			check_stmt(s->else_branch, loop_d, switch_d);
			break;
		}
		case StmtKind::While:
			if (s->expr) check_expr(s->expr);
			check_stmt(s->then_branch, loop_d + 1, switch_d);
			break;
		case StmtKind::DoWhile:
			check_stmt(s->then_branch, loop_d + 1, switch_d);
			if (s->expr) check_expr(s->expr);
			break;
		case StmtKind::For:
		{
			// 同 Compound：不压独立作用域，for_init 声明的循环变量并入函数作用域。
			if (s->for_init) check_stmt(s->for_init, loop_d, switch_d);
			if (s->expr) check_expr(s->expr);
			check_stmt(s->then_branch, loop_d + 1, switch_d);
			if (s->for_incr) check_expr(s->for_incr);
			break;
		}
		case StmtKind::Switch:
			// C23 switch init-statement：先处理 init_stmt（在其中声明的变量对选择式可见）
			if (s->init_stmt) check_stmt(s->init_stmt, loop_d, switch_d);
			if (s->expr) check_expr(s->expr);
			check_stmt(s->then_branch, loop_d, switch_d + 1);
			break;
		case StmtKind::Case:
		case StmtKind::Default:
			if (switch_d == 0) report(s->loc, "case label not within a switch statement");
			check_stmt(s->then_branch, loop_d, switch_d);
			for (const auto &b : s->body) check_stmt(b, loop_d, switch_d);
			break;
		case StmtKind::Break:
			if (loop_d == 0 && switch_d == 0)
				report(s->loc, "break statement not in loop or switch");
			break;
		case StmtKind::Continue:
			if (loop_d == 0) report(s->loc, "continue statement not in a loop");
			break;
		case StmtKind::Goto:
			if (!labels_.count(s->label_name))
				report(s->loc, "use of undeclared label '" + s->label_name + "'");
			break;
		case StmtKind::Label:
			check_stmt(s->then_branch, loop_d, switch_d);
			break;
		}
	}

	void check_func(const FuncDecl &f)
	{
		cur_func_ = &f;
		scopes_.clear();
		scopes_.emplace_back(); // 形参作用域
		for (const auto &p : f.params)
			if (!p.name.empty()) scopes_.back()[p.name] = p.type;
		labels_.clear();
		collect_labels(f.body);
		check_stmt(f.body, 0, 0);
		cur_func_ = nullptr;
	}

	const CompileOptions &opts_;
	std::vector<Error> &errors_;
	bool has_error_ = false;

	std::unordered_map<std::string, const FuncDecl *> funcs_;
	std::unordered_map<std::string, CTypePtr> globals_;
	std::unordered_map<std::string, const StructDef *> structs_;
	std::vector<std::unordered_map<std::string, CTypePtr>> scopes_; // 末位为最内层
	std::unordered_set<std::string> labels_;
	const FuncDecl *cur_func_ = nullptr;
};

}  // namespace

bool analyze(const CompileOptions &opts, CompileState &state, std::vector<Error> &errors)
{
	if (!state.ast) return false;
	Sema sema(opts, errors);
	sema.run(state.ast);
	return sema.ok();
}

}  // namespace colang
