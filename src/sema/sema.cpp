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
//   10 同一 TU 函数重复定义  11 函数名与文件作用域对象同名  12 原型/定义返回类型冲突
//   （另：非 void 函数漏 return 发 warning，不阻断编译）
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
		main_tu_ = tu.get(); // P212：可见性走查需访问 imports / imported_top_level_body_count
		// 建全局符号表（指针指向 ast 内部，parse 后 vector 不再变动，稳定）
		for (const auto &f : tu->funcs)
		{
			auto it = funcs_.find(f.name);
			if (it == funcs_.end())
			{
				funcs_[f.name] = &f;
				continue;
			}
			// 同一 TU 内重复函数定义（无论参数是否相同）：C 约束违例
			if (it->second->body && f.body)
				report(f.loc, "redefinition of '" + f.name + "'");
			// 原型与定义的返回类型冲突
			else if (!same_type(it->second->return_type, f.return_type))
				report(f.loc, "conflicting types for '" + f.name + "'");
			// 调用元数检查须看带定义的条目
			if (f.body && !it->second->body)
				funcs_[f.name] = &f;
		}
		for (const auto &g : tu->globals) globals_[g.name] = g.type;
		// P214：[[deprecated]] 登记表（name → 消息）。与 funcs_/globals_ 的「规范条目」归并不依赖顺序：
		//   原型与定义分属两条 AST 记录，任一处标注即对整个实体生效。
		for (const auto &f : tu->funcs)
			if (f.is_deprecated) dep_funcs_[f.name] = f.deprecated_msg;
		for (const auto &g : tu->globals)
			if (g.is_deprecated) dep_globals_[g.name] = g.deprecated_msg;
		for (const auto &s : tu->structs)
			if (!s.tag.empty()) structs_[s.tag] = &s;
		// P217：enum 类型标签登记表（供跨模块类型可见性门禁查 is_public/source_module_key）
		for (const auto &e : tu->enums)
			if (!e.tag.empty()) enums_[e.tag] = &e;
		// 函数名与文件作用域对象同名：不同种类符号的重定义（局部同名遮蔽合法，不查）
		std::unordered_set<std::string> clash_reported;
		for (const auto &f : tu->funcs)
			if (globals_.count(f.name) && clash_reported.insert(f.name).second)
				report(f.loc, "redefinition of '" + f.name + "' as different kind of symbol");

		// P214：[[lib("X")]] 语义校验（可挂对象/须纯原型/与 public 互斥/同名不同库冲突）
		check_attributes(tu);
		// P217：跨模块类型可见性门禁（主源引用导入的非 public struct/enum → 拒）。
		//   ① 主源全局变量类型 ② 主源函数签名（返回 + 参数） ③ 主源 struct 成员类型。
		//   ④ 函数体内局部声明/转换由 check_stmt/check_expr 就地触发。
		for (const auto &g : tu->globals)
			if (g.source_module_key.empty()) enforce_type_visible(g.type, g.loc);
		for (const auto &f : tu->funcs)
			if (f.source_module_key.empty())
			{
				enforce_type_visible(f.return_type, f.loc);
				for (const auto &p : f.params) enforce_type_visible(p.type, f.loc);
			}
		for (const auto &s : tu->structs)
			if (s.source_module_key.empty() && s.is_definition)
				for (const auto &m : s.members) enforce_type_visible(m.type, s.loc);
		// 仅检查函数体（全局初始化器暂不查，降低误伤面）
		compute_dead_ends();
		for (const auto &f : tu->funcs)
		{
			if (f.body) check_func(f);
		}
		// P211 Co 层入口模型：顶层裸语句需检查（break/continue 无围比上下文一时报错，
		//   表达式引用未声明符号同样拒收）。cur_func_=nullptr，Return 已自然旁路“void 函数不应
		//   返回值”类检查（见 check_stmt 中 Return 分支的 cur_func_ 守卫）。
		check_top_level(tu);
	}

	// P211：顶层语句序列检查 + 无入口警告（仅对 .co 后缀、且 link_output 时发警告）
	void check_top_level(const TUPtr &tu)
	{
		// 先检查顶层裸语句（循环围比传 0/0，break/continue 自然报错）
		cur_func_ = nullptr;
		scopes_.clear();
		scopes_.emplace_back(); // 文件作用域（全局变量已入 globals_，本层仅充局部声明用）
		// P212：拓序前插的 imported 项需“不查跨模块可见性”（子模块引用同模块非 public 助手合法）。
		//   以 imported_top_level_body_count 为界：前缀项标“imported”、后缀项标“主源”（空串）。
		const std::size_t prefix = tu->imported_top_level_body_count;
		std::size_t idx = 0;
		for (const auto &s : tu->top_level_body)
		{
			if (!s) { ++idx; continue; }
			cur_source_module_key_ = (idx < prefix) ? std::string("imported") : std::string();
			check_stmt(s, 0, 0);
			++idx;
		}
		cur_source_module_key_.clear();
		// 无入口警告：.co 文件，且既无裸指令又无 user main 定义，且链接为 exe 目标
		const bool is_co = [&]
		{
			const std::string &p = opts_.source_path;
			return p.size() >= 3 && (p.compare(p.size() - 3, 3, ".co") == 0 ||
									 p.compare(p.size() - 3, 3, ".CO") == 0);
		}();
		if (!is_co) return;
		if (opts_.compile_only || opts_.emit_llvm || !opts_.link_output) return;
		bool has_bare = !tu->top_level_body.empty();
		bool has_user_main = false;
		for (const auto &f : tu->funcs)
			if (f.body && f.name == "main") { has_user_main = true; break; }
		if (!has_bare && !has_user_main)
			warn(SourceLoc{}, "colang: warning: no entry point (no top-level commands and no main); generating empty executable");
	}

	bool ok() const { return !has_error_; }

private:
	// —— P214 [[lib("X")]] 校验 ——
	// 语法位与重复项由 parser 拦下（仅顶层声明位才记录、同声明多个 [[lib]] 报错）；
	// 本处只查需要完整 AST 才能判定的语义约束：
	//   ③ 须纯原型/纯声明（函数不得有 body；全局变量须 extern 且无初始化式）
	//   ④ 与 static 互斥；public 仅对「无 body 外部原型」放行（重导出，见下），
	//      对含 body 的定义因 ③ 已拦；全局数据符号 public+lib 仍禁（dllimport 数据未建模）
	//   ⑥ 同一符号名来自两个不同 lib → 报错（/DEFAULTLIB 无法区分同名归属）
	void check_attributes(const TUPtr &tu)
	{
		std::unordered_map<std::string, std::pair<std::string, SourceLoc>> seen_lib;
		// 同名实体是否在另一条记录上带函数体（`[[lib]]` 原型 + 本地定义 = 归属矛盾）
		std::unordered_set<std::string> has_body;
		for (const auto &f : tu->funcs)
			if (f.body) has_body.insert(f.name);
		auto note_lib = [&](const std::string &sym, const std::string &lib, const SourceLoc &loc)
		{
			auto it = seen_lib.find(sym);
			if (it == seen_lib.end())
			{
				seen_lib[sym] = {lib, loc};
				return;
			}
			if (it->second.first != lib)
				report(loc, "symbol '" + sym + "' is declared with conflicting 'lib' attributes ('" +
				                  it->second.first + "' and '" + lib + "')");
		};
		for (const auto &f : tu->funcs)
		{
			if (f.lib_name.empty()) continue;
			const std::string tag = "'lib' attribute on '" + f.name + "'";
			if (f.body)
				report(f.loc, tag + " must be a declaration (function has a definition)");
			// P214 放宽（winapi.co 落地）：`public` + `[[lib]]` 现允许共存于「无 body 外部原型」——
			// 语义为「本模块从外部库解析该符号、并作为接口重导出给 import 方」。colang 的 public 对无
			// body 原型并不注入 DLL /EXPORT（P213），故与 lib 归属不矛盾。含 body 的定义已被上方「must
			// be a declaration」拦下；`static`（内部链接）与重导出相冲、仍禁止。旧行为一律拒绝 public+lib。
			if (f.is_static)
				report(f.loc, tag + " cannot be combined with 'static'");
			if (has_body.count(f.name))
				report(f.loc, tag + " conflicts with a definition of '" + f.name + "'");
			note_lib(f.name, f.lib_name, f.loc);
		}
		for (const auto &g : tu->globals)
		{
			if (g.lib_name.empty()) continue;
			const std::string tag = "'lib' attribute on '" + g.name + "'";
			if (!g.is_extern || g.init)
				report(g.loc, tag + " must be an 'extern' declaration (no definition or initializer)");
			if (g.is_public)
				report(g.loc, tag + " cannot be combined with 'public'");
			if (g.is_static)
				report(g.loc, tag + " cannot be combined with 'static'");
			note_lib(g.name, g.lib_name, g.loc);
		}
	}

	// P214：[[deprecated]] 引用点告警（不改退出码、不阻断编译）
	void note_deprecated_use(const std::string &name, const std::string &msg, const SourceLoc &loc)
	{
		warn(loc, "'" + name + "' is deprecated" + (msg.empty() ? "" : (": " + msg)));
	}
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

	// 告警：不置 has_error_，编译继续（与 main.cpp 的 is_warning 口径一致）
	void warn(const SourceLoc &loc, const std::string &msg)
	{
		Error e;
		e.file = loc.file.empty() ? opts_.source_path : loc.file;
		e.pos.line = loc.line;
		e.pos.column = loc.column;
		e.message = msg;
		e.is_warning = true;
		errors_.push_back(std::move(e));
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

	// 返回类型同一性判定：忽略限定符拼写，只比结构（原型/定义冲突检查用）
	static bool same_type(const CTypePtr &a, const CTypePtr &b)
	{
		if (!a || !b) return !a && !b;
		if (a->kind != b->kind) return false;
		switch (a->kind)
		{
		case TypeKind::Pointer:
		case TypeKind::Array:
			return same_type(a->base, b->base);
		case TypeKind::Function:
			if (a->is_variadic != b->is_variadic || a->params.size() != b->params.size())
				return false;
			if (!same_type(a->base, b->base)) return false;
			for (std::size_t i = 0; i < a->params.size(); ++i)
				if (!same_type(a->params[i], b->params[i])) return false;
			return true;
		case TypeKind::Struct:
		case TypeKind::Union:
		case TypeKind::Enum:
			return a->tag == b->tag;
		case TypeKind::BitInt:
			return a->bit_width == b->bit_width && a->is_unsigned == b->is_unsigned;
		default:
			return a->is_unsigned == b->is_unsigned;
		}
	}

	// —— 控制流可达性（「非 void 函数漏 return」告警用）——
	// falls_through：语句执行完能否落到其后的语句（false = 必然 return/无限循环/不返回调用）。
	// 保守建模：拿不准一律按「可能落穿」处理，避免误伤合法程序。
	bool falls_through(const StmtPtr &s)
	{
		if (!s) return true;
		switch (s->kind)
		{
		case StmtKind::Return:
			return false;
		case StmtKind::ExprStmt:
			return !is_noreturn_call(s->expr);
		case StmtKind::Compound:
			for (const auto &b : s->body)
				if (!falls_through(b)) return false;
			return true;
		case StmtKind::If:
			if (!s->else_branch) return true;
			return falls_through(s->then_branch) || falls_through(s->else_branch);
		case StmtKind::While:
		case StmtKind::DoWhile:
		case StmtKind::For:
			// 常量真条件且循环体内无本层 break → 永不落穿
			return !const_true_cond(s->expr) || has_break(s->then_branch);
		case StmtKind::Switch:
			return switch_falls_through(s);
		default:
			return true;
		}
	}

	bool is_noreturn_call(const ExprPtr &e)
	{
		if (!e || e->kind != ExprKind::Call || e->name.empty()) return false;
		static const char *const kNoreturn[] = {
			"exit", "_Exit", "quick_exit", "abort", "longjmp", "thrd_exit"
		};
		for (const char *n : kNoreturn)
			if (e->name == n) return true;
		return dead_ends_.count(e->name) > 0;
	}

	// 本 TU 内「永不返回调用者」的函数（末尾 exit/abort/无限循环，或转调同类函数）。
	// 迭代到不动点：先认内建 noreturn，再逐轮并入「body 不返回」的函数（如 `noreturn void finish(){exit(0);}`）。
	void compute_dead_ends()
	{
		for (bool changed = true; changed; )
		{
			changed = false;
			for (const auto &kv : funcs_)
			{
				const FuncDecl *f = kv.second;
				if (!f->body || dead_ends_.count(f->name)) continue;
				if (never_returns(f->body))
				{
					dead_ends_.insert(f->name);
					changed = true;
				}
			}
		}
	}

	// 语句是否永不正常返回调用者：所有出口都是 noreturn 调用 / 无限循环。
	// 与 falls_through 互补——`return` 属「返回」，故 Return 分支为 false。
	bool never_returns(const StmtPtr &s)
	{
		if (!s) return false;
		switch (s->kind)
		{
		case StmtKind::ExprStmt:
			return is_noreturn_call(s->expr);
		case StmtKind::Compound:
			for (const auto &b : s->body)
				if (never_returns(b)) return true;
			return false;
		case StmtKind::If:
			return s->else_branch && never_returns(s->then_branch) &&
			       never_returns(s->else_branch);
		case StmtKind::While:
		case StmtKind::DoWhile:
		case StmtKind::For:
			return const_true_cond(s->expr) && !has_break(s->then_branch);
		default:
			return false; // 含 Return：正常返回到调用者
		}
	}

	static bool const_true_cond(const ExprPtr &e)
	{
		if (!e) return true; // for(;;)
		return e->kind == ExprKind::IntLiteral && e->int_val != 0;
	}

	// 本层循环体内是否有 break（嵌套循环/switch 的 break 归内层，不下探）
	static bool has_break(const StmtPtr &s)
	{
		if (!s) return false;
		switch (s->kind)
		{
		case StmtKind::Break:
			return true;
		case StmtKind::Compound:
			for (const auto &b : s->body)
				if (has_break(b)) return true;
			return false;
		case StmtKind::If:
			return has_break(s->then_branch) || has_break(s->else_branch);
		case StmtKind::Label:
			return has_break(s->then_branch);
		case StmtKind::Case:
		case StmtKind::Default:
			if (has_break(s->then_branch)) return true;
			for (const auto &b : s->body)
				if (has_break(b)) return true;
			return false;
		default:
			return false;
		}
	}

	// switch 是否可能落穿到其后：无 default 必可能；本层 break 必可能；
	// 否则由体内最后一条语句是否落穿决定（各组按源码顺序线性合并）。
	bool switch_falls_through(const StmtPtr &s)
	{
		if (!has_default_label(s->then_branch)) return true;
		if (has_break(s->then_branch)) return true;
		bool completes = true;
		std::vector<StmtPtr> seq;
		flatten_switch_body(s->then_branch, seq);
		for (const auto &b : seq)
		{
			if (b->kind == StmtKind::Case || b->kind == StmtKind::Default)
				completes = falls_through(b->then_branch);
			else
				completes = completes && falls_through(b);
		}
		return completes;
	}

	static void flatten_switch_body(const StmtPtr &s, std::vector<StmtPtr> &out)
	{
		if (!s) return;
		if (s->kind == StmtKind::Compound)
		{
			for (const auto &b : s->body) out.push_back(b);
			return;
		}
		out.push_back(s);
	}

	static bool has_default_label(const StmtPtr &s)
	{
		if (!s) return false;
		if (s->kind == StmtKind::Default) return true;
		if (s->kind == StmtKind::Switch) return false; // 内层 switch 的 default 不算
		if (has_default_label(s->then_branch)) return true;
		if (has_default_label(s->else_branch)) return true;
		if (has_default_label(s->for_init)) return true;
		if (has_default_label(s->init_stmt)) return true;
		for (const auto &b : s->body)
			if (has_default_label(b)) return true;
		return false;
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

	// 名字是否由局部作用域（块作域/形参）声明：P214 deprecated 查表需先排阴蔽
	bool is_declared_local(const std::string &n) const
	{
		for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
			if (it->count(n)) return true;
		return false;
	}

	// P212：查符号归属模块。命中 imported 顶层项 → 返回非空 source_module_key；否则主源、返回空。
	//   只处理 funcs_ + globals_；structs_/enums 本版不参与可见性。枚举常量作为常量入 globals_ 同属。
	std::string symbol_source_module(const std::string &n) const
	{
		auto fit = funcs_.find(n);
		if (fit != funcs_.end() && fit->second) return fit->second->source_module_key;
		auto sit = structs_.find(n);
		if (sit != structs_.end() && sit->second) return sit->second->source_module_key;
		// globals_ 只存 CTypePtr，不存 GlobalVar。需回 back 到 tu->globals 里线性定位。
		//   MVP 阶段顶层项量小，可接受；后续如需优化可换 name → const GlobalVar* 的辅助表。
		if (main_tu_)
		{
			for (const auto &g : main_tu_->globals)
				if (g.name == n) return g.source_module_key;
			for (const auto &e : main_tu_->enums)
				for (const auto &c : e.constants)
					if (c.name == n) return e.source_module_key;
		}
		return std::string();
	}

	// P212：目标 imported 符号是否从主源可见（主源本层 imports 里匹配）。
	//   - 命名条目（非 star）：匹配 spec.name 或 spec.alias；目标必须 is_public，否则报「不可导入非 public」。
	//   - star：目标必须 is_public。
	//   返回 true = 可访问。false = 需报错（已在内部直接 report）。调用方仅在主源上下文使用。
	bool ensure_visible_from_main(const std::string &name, const std::string &target_module,
	                              bool target_is_public, const SourceLoc &loc)
	{
		if (!main_tu_) return true;
		bool star_seen = false;
		bool named_seen = false;
		bool named_non_public = false;
		for (const auto &d : main_tu_->imports)
		{
			if (d.resolved_module_key != target_module) continue;
			for (const auto &sp : d.specs)
			{
				if (sp.is_star) { star_seen = true; continue; }
				if (sp.name == name || sp.alias == name)
				{
					named_seen = true;
					if (!target_is_public) named_non_public = true;
				}
			}
		}
		if (named_seen)
		{
			if (named_non_public && !target_is_public)
			{
				report(loc, "cannot import non-public symbol '" + name + "'");
				return false;
			}
			return target_is_public;
		}
		if (star_seen)
		{
			if (!target_is_public)
			{
				report(loc, "cannot import non-public symbol '" + name + "'");
				return false;
			}
			return true;
		}
		report(loc, "symbol '" + name + "' is not imported from its module");
		return false;
	}

	// P217：按 tag 查 struct/union 的「完整定义项」（is_definition=true）。
	//   不取 structs_（可能被主源的前置引用空壳覆盖），而直接扫 main_tu_->structs。
	const StructDef *find_struct_def_by_tag(const std::string &tag) const
	{
		if (!main_tu_) return nullptr;
		for (const auto &s : main_tu_->structs)
			if (s.tag == tag && s.is_definition) return &s;
		return nullptr;
	}

	// P212：封装——在主源上下文下检查名字引用；非主源代码直接放行。
	void enforce_import_visibility(const std::string &name, const SourceLoc &loc)
	{
		// 只在主源代码（cur_source_module_key_ 空）中才需跨模块可见性检查。
		if (!cur_source_module_key_.empty()) return;
		const std::string src = symbol_source_module(name);
		if (src.empty()) return; // 主源自身符号、不跨模块
		// 取目标 is_public 标记（funcs_/globals_ 两路）
		bool target_pub = false;
		auto fit = funcs_.find(name);
		if (fit != funcs_.end() && fit->second) target_pub = fit->second->is_public;
		else
		{
			auto sit = structs_.find(name);
			if (sit != structs_.end() && sit->second) target_pub = sit->second->is_public;
			else if (main_tu_)
			{
				for (const auto &g : main_tu_->globals)
					if (g.name == name) { target_pub = g.is_public; break; }
			}
		}
		ensure_visible_from_main(name, src, target_pub, loc);
	}

	// P217：主源将导入的非 public struct/enum 当类型用 → 拒收。仅主源（cur_source_module_key_ 空）触发。
	//   沿 Pointer/Array 的 base 逐层剥、Function 遍历返回类型与参数，得基类型后仅对 Struct/Union/Enum
	//   且非空 tag 查表；命中且 source_module_key 非空（导入类型）才调 ensure_visible_from_main。
	//   注：枚举常量名在 parse 期折成整数字面量、非类型引用，故门禁只作用于 enum 类型标签。
	void enforce_type_visible(const CTypePtr &t, const SourceLoc &loc)
	{
		if (!cur_source_module_key_.empty()) return; // 仅主源；子模块内部互引放行
		if (!t) return;
		switch (t->kind)
		{
		case TypeKind::Pointer:
		case TypeKind::Array:
			enforce_type_visible(t->base, loc);
			return;
		case TypeKind::Function:
			enforce_type_visible(t->base, loc); // 返回类型
			for (const auto &p : t->params) enforce_type_visible(p, loc);
			return;
		case TypeKind::Struct:
		case TypeKind::Union:
		{
			if (t->tag.empty()) return;
			// 主源引用 `struct X` 时 parser 会先造一条前置引用项（is_definition=false、source_module_key 空）；
			//   loader 又把子模块 struct 插到 vector 首部，故 structs_[tag] 可能指向主源的空壳。
			//   门禁须以「真正的定义项」为准：按 tag 找 is_definition 的那条（即子模块定义）判归属/public。
			const StructDef *def = find_struct_def_by_tag(t->tag);
			if (!def || def->source_module_key.empty()) return; // 无定义/主源自定义 → 跳过
			ensure_visible_from_main(t->tag, def->source_module_key, def->is_public, loc);
			return;
		}
		case TypeKind::Enum:
		{
			if (t->tag.empty()) return;
			auto it = enums_.find(t->tag);
			if (it == enums_.end() || !it->second) return;
			const std::string &key = it->second->source_module_key;
			if (key.empty()) return;
			ensure_visible_from_main(t->tag, key, it->second->is_public, loc);
			return;
		}
		default:
			return;
		}
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
			else
			{
				enforce_import_visibility(e->name, e->loc); // P212
				// P214：[[deprecated]] 引用点告警（对象名；函数名作为值使用时同路径）
				if (!is_declared_local(e->name))
				{
					auto git = dep_globals_.find(e->name);
					if (git != dep_globals_.end())
						note_deprecated_use(e->name, git->second, e->loc);
					else
					{
						auto fit = dep_funcs_.find(e->name);
						if (fit != dep_funcs_.end())
							note_deprecated_use(e->name, fit->second, e->loc);
					}
				}
			}
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
				// P212：函数调用跨模块可见性检查（与 VarRef 同机制）。
				enforce_import_visibility(e->name, e->loc);
				auto it = funcs_.find(e->name);
				if (it != funcs_.end())
				{
					const FuncDecl *f = it->second;
					// P214：[[deprecated]] 函数调用点告警
					auto dep = dep_funcs_.find(e->name);
					if (dep != dep_funcs_.end())
						note_deprecated_use(e->name, dep->second, e->loc);
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
		case ExprKind::Cast:
			enforce_type_visible(e->type, e->loc); // P217：转换/复合字面量目标类型跨模块可见性
			break;
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
			enforce_type_visible(s->var_type, s->loc); // P217：局部声明类型跨模块可见性
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
		// P212：进入函数体前，将当前归属模块设为该函数自己的 source_module_key（主源 空串）。
		//   主源代码中引用 imported 符号 → 需 import 可见；
		//   imported 代码引用同模块非 public 助手 → 合法、不报错。
		cur_source_module_key_ = f.source_module_key;
		scopes_.clear();
		scopes_.emplace_back(); // 形参作用域
		for (const auto &p : f.params)
			if (!p.name.empty()) scopes_.back()[p.name] = p.type;
		labels_.clear();
		collect_labels(f.body);
		// 非 void 函数可能执行到末尾而无 return（C 为 UB，clang 默认 -Wreturn-type 告警）。
		// main 例外：C11 §5.1.2.2.3 规定 main 落到 } 等价于 return 0，clang 亦不告警。
		if (f.return_type && f.return_type->kind != TypeKind::Void &&
			f.name != "main" && falls_through(f.body))
			warn(f.loc, "non-void function does not return a value in all control paths");
		check_stmt(f.body, 0, 0);
		cur_func_ = nullptr;
		cur_source_module_key_.clear();
	}

	const CompileOptions &opts_;
	std::vector<Error> &errors_;
	bool has_error_ = false;

	std::unordered_map<std::string, const FuncDecl *> funcs_;
	std::unordered_map<std::string, CTypePtr> globals_;
	// P214：[[deprecated]] 登记表（name → 消息，空串 = 无附带理由）
	std::unordered_map<std::string, std::string> dep_funcs_;
	std::unordered_map<std::string, std::string> dep_globals_;
	std::unordered_map<std::string, const StructDef *> structs_;
	// P217：enum 类型标签→定义（供跨模块类型可见性门禁）
	std::unordered_map<std::string, const EnumDef *> enums_;
	std::vector<std::unordered_map<std::string, CTypePtr>> scopes_; // 末位为最内层
	std::unordered_set<std::string> labels_;
	std::unordered_set<std::string> dead_ends_; // 永不返回调用者的函数
	const FuncDecl *cur_func_ = nullptr;
	// P212：当前正在走查的代码归属模块 key（空 = 主源）。影响跨模块可见性检查开关。
	std::string cur_source_module_key_;
	// P212：主源 TU 指针（仅 run() 内使用），供可见性检查访问 imports/globals/enums。
	const TranslationUnit *main_tu_ = nullptr;
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
