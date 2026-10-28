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
#include "colang/type_spell.h"

#include <cstddef>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
		// 路 B 阶段0：`__init__*__` 为模块初始化调度协议保留前缀，禁止用户代码自定义。
		//   无条件扫描（不随 --per-module 开关分岔）；`__co_main__` 等其它 dunder 不受影响。
		for (const auto &f : tu->funcs)
			if (f.name.rfind("__init__", 0) == 0)
				report(f.loc, "reserved protocol name '__init__*' cannot be defined by user code");
		// 路 B 阶段1b：per-module 转正路径先建 ModuleCatalog（逐模块 public 视图 + 跨模块归属），
		//   供后续符号表归并的「跨模块同名共存」守卫与 host 裸名歧义裁定使用。
		if (per_module_) build_catalog(tu);
		// 建全局符号表（指针指向 ast 内部，parse 后 vector 不再变动，稳定）
		for (const auto &f : tu->funcs)
		{
			auto it = funcs_.find(f.name);
			if (it == funcs_.end())
			{
				funcs_[f.name] = &f;
				continue;
			}
			// 路 B 阶段1b：跨模块同名（source_module_key 不同）在 per-module 闭包下合法共存，
			//   redefinition/conflicting 仅同模块内判；裸名归属与歧义交 host 视图裁定（h4b 根因）。
			if (per_module_ && it->second->source_module_key != f.source_module_key)
				continue;
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
		for (const auto &g : tu->globals) { globals_[g.name] = g.type; global_keys_[g.name] = g.source_module_key; if (!g.is_extern || !global_ident_locs_.count(g.name)) global_ident_locs_[g.name] = g.ident_loc; }
		// P214：[[deprecated]] 登记表（name → 消息）。与 funcs_/globals_ 的「规范条目」归并不依赖顺序：
		//   原型与定义分属两条 AST 记录，任一处标注即对整个实体生效。
		for (const auto &f : tu->funcs)
			if (f.is_deprecated) dep_funcs_[f.name] = f.deprecated_msg;
		// P249：[[nodiscard]] / [[noreturn]] 登记表（原型或定义任一处标注即对整个实体生效）
		for (const auto &f : tu->funcs)
		{
			if (f.is_nodiscard) nodiscard_funcs_[f.name] = f.nodiscard_msg;
			if (f.is_noreturn) noreturn_funcs_.insert(f.name);
		}
		for (const auto &g : tu->globals)
			if (g.is_deprecated) dep_globals_[g.name] = g.deprecated_msg;
		for (const auto &s : tu->structs)
			if (!s.tag.empty()) structs_[s.tag] = &s;
		// P217：enum 类型标签登记表（供跨模块类型可见性门禁查 is_public/source_module_key）
		for (const auto &e : tu->enums)
			if (!e.tag.empty()) enums_[e.tag] = &e;
		// P300 Co class（0.4.0）：注册类定义 + 检查保留协议名
		for (const auto &c : tu->classes)
		{
			if (!c.tag.empty()) classes_[c.tag] = &c;
			// 方法名不得为编译器保留协议名
			for (const auto &m : c.methods)
				if (m.name == "__new__" || m.name == "__retain__" ||
					m.name == "__release__" || m.name == "__delete__")
					report(m.loc, "reserved protocol name '" + m.name + "' cannot be used as method name");
			// P300 形参/成员所有权（0.4.0 收尾）：class 句柄作成员——宿主拥有（赋值 store-own、
			//   __new__ 置 null、__delete__ 级联 release）。
			// P300 struct/union 作 class 成员（值语义内嵌）：无引用计数/无所有权转移，随宿主堆块分配/释放；
			//   布局/访问/赋值复用既有聚合机器件（to_llvm_alloca_type 内联 + gen_assign memcpy + struct-in-struct GEP），
			//   本批放开。成员类型可见性仍由 enforce_type_visible 守、未定义 tag 沿用既有报错。
			// P300 形参借用（0.4.0 收尾）：class 句柄作方法形参（不 retain、出域不 release、
			//   重绑定不转移所有权，由 codegen borrowed_slots_ 承载）；普通函数 class 形参本无门禁、口径一致。
		}
		// 路 B 阶段1（ambient 切断）：登记「主源自身声明」的名字集（source_module_key 空，
		//   含主源自带 #include 带入的原型）。宿主自身可见的名字优先于被导入模块的同名项，跨模块可见性
		//   门禁据此豁免——根除「cannot import non-public symbol 'printf'」类 ambient 误报（宿主与子模块
		//   各自 #include 同名标准库符号被误判为跨模块未导入）。真正未导入的模块私有助手仍报错（宿主不自声明）。
		for (const auto &f : tu->funcs)   if (f.source_module_key.empty()) host_own_names_.insert(f.name);
		for (const auto &g : tu->globals) if (g.source_module_key.empty()) host_own_names_.insert(g.name);
		for (const auto &s : tu->structs) if (s.source_module_key.empty() && !s.tag.empty()) host_own_names_.insert(s.tag);
		for (const auto &e : tu->enums)
			if (e.source_module_key.empty())
			{
				if (!e.tag.empty()) host_own_names_.insert(e.tag);
				for (const auto &c : e.constants) host_own_names_.insert(c.name);
			}
		// P300 Co class：主源自身定义的类名加入 host_own_names_
		for (const auto &c : tu->classes) if (c.source_module_key.empty() && !c.tag.empty()) host_own_names_.insert(c.tag);
		// 函数名与文件作用域对象同名：不同种类符号的重定义（局部同名遮蔽合法，不查）
		std::unordered_set<std::string> clash_reported;
		for (const auto &f : tu->funcs)
		{
			auto git = global_keys_.find(f.name);
			if (git == global_keys_.end()) continue;
			// 路 B 阶段1b：跨模块函数名与他模块全局名同名共存合法（仅同模块内才判不同种类重定义）。
			if (per_module_ && git->second != f.source_module_key) continue;
			if (clash_reported.insert(f.name).second)
				report(f.loc, "redefinition of '" + f.name + "' as different kind of symbol");
		}

		// P214：[[lib("X")]] 语义校验（可挂对象/须纯原型/与 public 互斥/同名不同库冲突）
		check_attributes(tu);
		// P252：`_Alignas` 跨声明一致性（同一对象多条声明的对齐须相容）
		check_alignas_consistency(tu);
		// P249：[[nodiscard]] 挂在无返回值的函数上是无效标注（clang 同文告警）
		for (const auto &kv : nodiscard_funcs_)
		{
			auto it = funcs_.find(kv.first);
			if (it != funcs_.end() && it->second->return_type &&
			    it->second->return_type->kind == TypeKind::Void)
				warn(it->second->loc, "attribute 'nodiscard' cannot be applied to functions without return value");
		}
		// P217：跨模块类型可见性门禁（主源引用导入的非 public struct/enum → 拒）。
		//   ① 主源全局变量类型 ② 主源函数签名（返回 + 参数） ③ 主源 struct 成员类型。
		//   ④ 函数体内局部声明/转换由 check_stmt/check_expr 就地触发。
		for (const auto &g : tu->globals)
			if (g.source_module_key.empty())
			{
				enforce_type_visible(g.type, g.loc);
				if (!g.is_extern) enforce_complete_type(g.type, g.loc, IctxGlobal); // P255：纯声明不分配、clang 允许 extern struct I e;
				reject_class_pointer(g.type, g.loc); // P300：禁全局 `C* g;`
			}
		for (const auto &f : tu->funcs)
			if (f.source_module_key.empty())
			{
				enforce_type_visible(f.return_type, f.loc);
				for (const auto &p : f.params) enforce_type_visible(p.type, f.loc);
				reject_class_pointer(f.return_type, f.loc); // P300：禁返回/参数含 `C*`
				for (const auto &p : f.params) reject_class_pointer(p.type, f.loc);
			}
		for (const auto &s : tu->structs)
			if (s.source_module_key.empty() && s.is_definition)
				for (const auto &m : s.members)
				{
					enforce_type_visible(m.type, s.loc);
					enforce_complete_type(m.type, s.loc, IctxField); // P255：字段不完整类型
				}
		// 仅检查函数体（全局初始化器暂不查，降低误伤面）
		// P249：[[noreturn]] 直接登记为「永不返回」，先于控制流推断——使仅有声明（无 body、
		//   如头文件原型）的 noreturn 函数在调用点同样按不返回处理。
		for (const auto &n : noreturn_funcs_) dead_ends_.insert(n);
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

	// 路 B 阶段1b：从合并后主 TU 按 source_module_key 分组，构建逐模块 public 视图 + 限定名映射。
	//   ambient 切断（导出侧）：只收模块「自有」声明（source_module_key 非空）且 is_public；
	//   主源自身项（key 空）与经 #include 带入的原型均不入导出面。
	void build_catalog(const TUPtr &tu)
	{
		// 模块 key → 文件名 stem（去扩展名）：默认限定名。
		auto stem_of = [](const std::string &key) -> std::string
		{
			std::string base = key;
			auto slash = base.find_last_of("/\\");
			if (slash != std::string::npos) base = base.substr(slash + 1);
			auto dot = base.find_last_of('.');
			if (dot != std::string::npos) base = base.substr(0, dot);
			return base;
		};
		for (const auto &f : tu->funcs)
		{
			if (f.source_module_key.empty() || !f.is_public) continue;
			catalog_.by_module[f.source_module_key].funcs[f.name] = &f;
			xmod_public_owners[f.name].insert(f.source_module_key);
		}
		for (const auto &g : tu->globals)
		{
			if (g.source_module_key.empty() || !g.is_public) continue;
			catalog_.by_module[g.source_module_key].globals[g.name] = &g;
			xmod_public_owners[g.name].insert(g.source_module_key);
		}
		for (const auto &s : tu->structs)
		{
			if (s.source_module_key.empty() || !s.is_public || !s.is_definition || s.tag.empty()) continue;
			catalog_.by_module[s.source_module_key].structs[s.tag] = &s;
		}
		for (const auto &e : tu->enums)
		{
			if (e.source_module_key.empty() || !e.is_public || e.tag.empty()) continue;
			catalog_.by_module[e.source_module_key].enums[e.tag] = &e;
		}
		// P300 Co class（跨模块 import）：逐模块 public class 视图（class 恒为定义、无 is_definition 位）。
		for (const auto &c : tu->classes)
		{
			if (c.source_module_key.empty() || !c.is_public || c.tag.empty()) continue;
			catalog_.by_module[c.source_module_key].classes[c.tag] = &c;
		}
		// 限定名映射：主源本层 imports 的 `as ModAlias`（无别名取文件 stem）→ resolved_module_key。
		//   consts 表暂空（public const int 的跨模块导出面留待限定访问实现步补齐）。
		for (const auto &d : tu->imports)
		{
			if (d.resolved_module_key.empty()) continue;
			const std::string alias = d.module_alias.empty() ? stem_of(d.resolved_module_key) : d.module_alias;
			catalog_.qualified[alias] = d.resolved_module_key;
			// 确保被 import 但无 public 导出的模块也有条目（限定名可解析到空视图）。
			catalog_.by_module.emplace(d.resolved_module_key, View{});
		}
		// 路 B 阶段2-c：跨模块同名（host vs import、import vs import）共存 → 给「导入项」挂 dunder 合成符号名
		//   `__<stem>__<name>`；host 自身项保留裸名（ambient 胜出、host 定义全局唯一）。
		//   同模块内 prototype+definition（distinct key 唯一）不触发。消解 LLVM 链接期重复 external 符号误并（h4b b=2 根因）。
		std::unordered_map<std::string, std::unordered_set<std::string>> fn_keys, g_keys;
		for (const auto &f : tu->funcs) fn_keys[f.name].insert(f.source_module_key);
		for (const auto &g : tu->globals) g_keys[g.name].insert(g.source_module_key);
		for (auto &f : tu->funcs)
			if (!f.source_module_key.empty() && fn_keys[f.name].size() > 1)
				f.symbol_name = "__" + sanitize_ident(stem_of(f.source_module_key)) + "__" + f.name;
		for (auto &g : tu->globals)
			if (!g.source_module_key.empty() && g_keys[g.name].size() > 1)
				g.symbol_name = "__" + sanitize_ident(stem_of(g.source_module_key)) + "__" + g.name;
	}

	// 路 B 阶段2-c：将任意字符串洗为合法 C 标识符（非 [A-Za-z0-9_] 一律换 '_'）。
	static std::string sanitize_ident(const std::string &s)
	{
		std::string o;
		for (char c : s)
			o += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
		return o;
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
		// P229：WinMain 入口检查——与顶层裸代码互斥；[[subsystem]] 仅入口函数生效。
		//   置于链接模式闸门之前，使 -c/--emit-llvm 亦能拦下互斥（否则裸码被 codegen 静默丢弃）。
		//   P231：入口属性仅主模块生效——导入模块（source_module_key 非空）里的 WinMain
		//   不参与互斥/参数检查、[[subsystem]] 被忽略并报警告（codegen 侧同步不置位）。
		{
			const FuncDecl *winmain_fn = nullptr;
			for (const auto &f : tu->funcs)
			{
				if (!f.source_module_key.empty())
				{
					if (f.subsystem_windows)
					{
						warn(f.loc, "'subsystem' attribute in an imported module has no effect; it is only honored on the main module's entry function 'main' or 'WinMain'");
					}
					continue;
				}
				if (f.body && f.name == "WinMain") winmain_fn = &f;
				if (f.subsystem_windows && f.name != "main" && f.name != "WinMain")
					warn(f.loc, "'subsystem' attribute is only effective on the entry function 'main' or 'WinMain'");
			}
			if (winmain_fn)
			{
				// 路 B 阶段3：WinMain 入口与顶层裸指令不再互斥——codegen 合成 WinMain 载体
				//   （改名转发 __co_WinMain__ + 头部跑顶层初始化/裸句）承载，原「静默丢弃」根除。
				if (winmain_fn->params.size() != 4)
					report(winmain_fn->loc, "'WinMain' must have 4 parameters (hInstance, hPrevInstance, lpCmdLine, nCmdShow)");
			}
		}
		// 无入口警告：.co 文件，且既无裸指令又无 user main 定义，且链接为 exe 目标
		const bool is_co = [&]
		{
			const std::string &p = opts_.source_path;
			return p.size() >= 3 && (p.compare(p.size() - 3, 3, ".co") == 0 ||
									 p.compare(p.size() - 3, 3, ".CO") == 0);
		}();
		if (!is_co) return;
		if (opts_.compile_only || opts_.emit_llvm || !opts_.link_output) return;
		// 路 B 阶段3：产 DLL（-o *.dll）时入口为合成 DllMain（顶层初始化载体），非 exe main 语义——
		//   「无入口点」告警不适用（大小写无关判后缀）。
		{
			const std::string &op = opts_.output;
			auto ci_ends = [&op](const char *s)
			{
				const std::size_t n = std::string(s).size();
				if (op.size() < n) return false;
				for (std::size_t k = 0; k < n; ++k)
				{
					const char a = (char)std::tolower((unsigned char)op[op.size() - n + k]);
					const char b = (char)std::tolower((unsigned char)s[k]);
					if (a != b) return false;
				}
				return true;
			};
			if (ci_ends(".dll")) return;
		}
		bool has_bare = !tu->top_level_body.empty();
		bool has_user_main = false;
		for (const auto &f : tu->funcs)
			if (f.body && (f.name == "main" || f.name == "WinMain") && f.source_module_key.empty()) { has_user_main = true; break; } // P231：仅主模块
		if (!has_bare && !has_user_main)
			warn(SourceLoc{}, "colang: warning: no entry point (no top-level commands and no main); generating empty executable");
	}

	bool ok() const { return !has_error_; }

	// 路 B 阶段1b：per-module 转正开关（须在 run() 前设置）。
	void set_per_module(bool v) { per_module_ = v; }
	// 路 B 阶段1b：取走已构建的 ModuleCatalog（run() 后有效）。
	ModuleCatalog take_catalog() { return std::move(catalog_); }

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

	// P249：[[nodiscard]] 引用点诊断（C23 §6.7.11）。显式 `(void)f();` 属于有意丢弃返回值，
	//   不告警（与 clang 一致）；void 返回函数本无返回值，内层已另有无效标注告警。
	void note_discarded_nodiscard(const ExprPtr &e)
	{
		if (!e) return;
		// `(void)expr` 显式丢弃：C 惯例的「有意忽略」写法
		if (e->kind == ExprKind::Cast && e->type && e->type->kind == TypeKind::Void) return;
		if (e->kind != ExprKind::Call || e->name.empty()) return;
		if (!nodiscard_funcs_.count(e->name)) return;
		auto it = funcs_.find(e->name);
		if (it == funcs_.end() || !it->second->return_type) return;
		if (it->second->return_type->kind == TypeKind::Void) return;
		const std::string &why = nodiscard_funcs_[e->name];
		warn(e->loc, "ignoring return value of function declared with 'nodiscard' attribute" +
			(why.empty() ? std::string() : ": " + why));
	}

	// —— P252：`_Alignas` 跨声明一致性（§6.7.2.1p4）——
	// 两条诊断（文本与定位均与 clang 同形）：
	//   ① 「有声明带 _Alignas、定义没带」→ error '_Alignas' must be specified on definition if
	//      it is specified on any declaration（指定义的**标识符**，clang 同列）。定义含暂定定义
	//      （文件作用域无 extern 即定义），纯 extern 声明不算：clang 实测
	//      `extern _Alignas(16) int c;` + `extern int c;` 零诊断、+ `int c;` 则报错。
	//   ② 两条声明都带 _Alignas 但**结果对齐**不同 → error redeclaration has different alignment
	//      requirement (A vs B)（指新那条的 `_Alignas` 关键字，clang 同列）。_Alignas(0) 的结果对齐
	//      = 类型自然对齐（故 `_Alignas(16)` 后跟 `_Alignas(0)` 在 int 上报 (4 vs 16)，与 clang 一致）。
	// 顺序敏感：clang 按声明逐条合并，因此需要区分两种「先前记录不带标注」的形态：
	//   • 先前是**暂定定义或纯 extern 声明**（可回改）→ 后续标注合法并被记住
	//     （`int a;` + `extern _Alignas(16) int a;` clang rc=0）；
	//   • 先前是**带初始化式的真定义**（不可回改）→ 后续每条标注各报一次 ①，
	//     位置指该定义的标识符，且**不记住**其对齐（故不再补报 ②）——clang 实测
	//     `int a = 1;` + `extern _Alignas(16) int a;` + `extern _Alignas(32) int a;` 得两条 ①。
	//     即使标注值与自然对齐相等（_Alignas(4) int）也报错：判据是「有没有写标注」。
	// 只看主源记录（source_module_key 为空）：clang 只能看到当前 TU，导入项计入会凭空报错。
	void check_alignas_consistency(const TUPtr &tu)
	{
		struct Seen
		{
			bool specified;
			unsigned effective;
			bool locked_def;   // 先前是「未带标注且已定型的真定义」：对齐不可再改
			SourceLoc def_loc; // 该真定义的标识符位置（回溯报 ① 指此处）
		};
		static const std::string cls1 =
			"'_Alignas' must be specified on definition if it is specified on any declaration";
		std::unordered_map<std::string, Seen> seen;
		for (const auto &g : tu->globals)
		{
			if (!g.source_module_key.empty()) continue;
			const bool is_def = (g.init != nullptr) || !g.is_extern; // 定义（含暂定定义）
			const bool has_init = (g.init != nullptr);               // 已定型的真定义
			auto it = seen.find(g.name);
			if (it == seen.end())
			{
				seen[g.name] = Seen{ g.align_specified, g.align_effective,
					(!g.align_specified && has_init), g.ident_loc };
				continue;
			}
			Seen &s = it->second;
			if (g.align_specified)
			{
				if (s.specified)
				{
					if (g.align_effective != s.effective)
						report(g.align_spec_loc, "redeclaration has different alignment requirement (" +
							std::to_string(g.align_effective) + " vs " + std::to_string(s.effective) + ")");
				}
				else if (s.locked_def)
				{
					report(s.def_loc, cls1); // 不更新 s：使后续标注再报一次 ①（clang 同形）
				}
				else
				{
					s.specified = true;
					s.effective = g.align_effective;
				}
			}
			else
			{
				if (s.specified && is_def) report(g.ident_loc, cls1);
				if (has_init && !s.locked_def)
				{
					s.locked_def = true;
					s.def_loc = g.ident_loc;
				}
			}
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

	// 带 note 附注的错误诊断（clang 同形：“file:line:col: note: ...”）
	void report_with_note(const SourceLoc &loc, const std::string &msg,
	                      const SourceLoc &note_loc, const std::string &note_msg)
	{
		Error e;
		e.file = loc.file.empty() ? opts_.source_path : loc.file;
		e.pos.line = loc.line;
		e.pos.column = loc.column;
		e.message = msg;
		e.is_warning = false;
		Error n;
		n.file = note_loc.file.empty() ? opts_.source_path : note_loc.file;
		n.pos.line = note_loc.line;
		n.pos.column = note_loc.column;
		n.message = note_msg;
		e.notes.push_back(std::move(n));
		errors_.push_back(std::move(e));
		has_error_ = true;
	}

	// 带 note 附注的告警
	void warn_with_note(const SourceLoc &loc, const std::string &msg,
	                    const SourceLoc &note_loc, const std::string &note_msg)
	{
		Error e;
		e.file = loc.file.empty() ? opts_.source_path : loc.file;
		e.pos.line = loc.line;
		e.pos.column = loc.column;
		e.message = msg;
		e.is_warning = true;
		Error n;
		n.file = note_loc.file.empty() ? opts_.source_path : note_loc.file;
		n.pos.line = note_loc.line;
		n.pos.column = note_loc.column;
		n.message = note_msg;
		e.notes.push_back(std::move(n));
		errors_.push_back(std::move(e));
	}

	// 查找变量声明位置（内层→外层→全局）
	SourceLoc find_decl_loc(const std::string &name)
	{
		for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
		{
			auto f = it->find(name);
			if (f != it->end()) return f->second.loc;
		}
		auto g = global_ident_locs_.find(name);
		if (g != global_ident_locs_.end()) return g->second;
		return {};
	}

	// —— 类型谓词 ——
	static bool is_struct_like(const CTypePtr &t)
	{
		return t && (t->kind == TypeKind::Struct || t->kind == TypeKind::Union || t->kind == TypeKind::Class);
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
		return globals_.count(n) > 0 || funcs_.count(n) > 0 || classes_.count(n) > 0;
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
		// 路 B 阶段1 ambient 切断：宿主自身（含其 #include）声明的名字优先，不作跨模块可见性检查。
		if (host_own_names_.count(name)) return;
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

	// 路 B 阶段1b：host 裸名歧义裁定③——同名由两个以上模块同时导出且宿主不自有 → 裸名歧义禁用（限定名仍可访问）。
	//   仅主源上下文（cur_source_module_key_ 空）、仅 per-module 转正路径触发。宿主自有一名胜出（裁定②遮蔽）、跳过。
	void check_ambiguous_bare(const std::string &name, const SourceLoc &loc)
	{
		if (!per_module_) return;
		if (!cur_source_module_key_.empty()) return;
		if (host_own_names_.count(name)) return;
		auto it = xmod_public_owners.find(name);
		if (it == xmod_public_owners.end() || it->second.size() < 2) return;
		report(loc, "reference to '" + name + "' is ambiguous (imported from multiple modules)");
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
		case TypeKind::Class: // P300 Co class
		{
			if (t->tag.empty()) return;
			auto it = classes_.find(t->tag);
			if (it == classes_.end() || !it->second) return;
			const std::string &key = it->second->source_module_key;
			if (key.empty()) return;
			ensure_visible_from_main(t->tag, key, it->second->is_public, loc);
			return;
		}
		default:
			return;
		}
	}

	// P300 Co class（§4.4）：class 具引用语义，禁声明/使用「指向类的指针」（`C*`）。
	//   剥 Pointer/Array 层、遍历 Function 返回/参数，遇 `Pointer -> Class` 即报。
	//   区别于 enforce_type_visible（仅主源跨模块门禁），本检查无条件执行（语言性质、非可见性）。
	void reject_class_pointer(const CTypePtr &t, const SourceLoc &loc)
	{
		if (!t) return;
		if (t->kind == TypeKind::Pointer && t->base && t->base->kind == TypeKind::Class)
		{
			report(loc, "class has reference semantics, pointer to class is not allowed");
			return;
		}
		if (t->kind == TypeKind::Pointer || t->kind == TypeKind::Array)
			reject_class_pointer(t->base, loc);
		else if (t->kind == TypeKind::Function)
		{
			reject_class_pointer(t->base, loc);
			for (const auto &p : t->params) reject_class_pointer(p, loc);
		}
	}

	// P300 Co class（§4.4/§4.5）：类句柄的类型相容性——声明初始化与赋值均要求源与目标同类，
	//   拒绝跨类（`Box b = counter;`）、非句柄右值（`Box c = 5;`）与不匹配的 new（`Box b = new Counter();`）。
	//   否则 codegen 会按「声明类」发 retain/release，对异类对象执行错误 dtor、或对非指针值解引用。
	//   保守原则：源类型推断不出（resolve_type 返回 null）即跳过，绝不误伤合法方言（如链式赋值中间态）。
	void check_class_handle_compat(const CTypePtr &dst, const ExprPtr &src, const SourceLoc &loc)
	{
		if (!dst || dst->kind != TypeKind::Class || !src) return;
		const std::string want = dst->tag;
		if (src->kind == ExprKind::New) // new：类名须与目标同类
		{
			if (!src->name.empty() && !want.empty() && src->name != want)
				report(loc, "class handle of type '" + want + "' cannot be bound to incompatible 'new " + src->name + "'");
			return;
		}
		if (src->kind == ExprKind::Comma && !src->args.empty()) // 逗号取末元素
		{
			check_class_handle_compat(dst, src->args.back(), loc);
			return;
		}
		CTypePtr st = resolve_type(src);
		if (!st) return; // 推断不出，保守跳过
		if (st->kind != TypeKind::Class)
			report(loc, "class handle requires a handle value, got incompatible type '" + describe_type(st) + "'");
		else if (!want.empty() && !st->tag.empty() && st->tag != want)
			report(loc, "cannot bind handle of class '" + st->tag + "' to class handle '" + want + "'");
	}

	// P255：对「仅有前向声明、无 {} 定义」的 struct/union 作按值分配（局部/全局/字段/数组元素）
	//   作前置诊断，把 LLVM 校验器的 `Cannot allocate unsized type` 挡在前面、报 clang 同款主错误。
	//   剥层规则：沿 Array 下探（记 saw_array），遇 Pointer/Function 立即停（不完整类型经指针合法），
	//   落到 Struct/Union 且 tag 非空、find_struct_def_by_tag 为空 ⇒ 不完整、报错。Enum 不纳入（越界）。
	//   仅主源触发（cur_source_module_key_ 空），镜像 enforce_type_visible 的门禁以压低回归面。
	enum IncompleteCtx { IctxVar, IctxField, IctxGlobal };

	void enforce_complete_type(const CTypePtr &t, const SourceLoc &loc, IncompleteCtx ctx)
	{
		if (!cur_source_module_key_.empty()) return; // 仅主源；imported 项/函数体自然跳过
		const CTypePtr *node = &t;                    // 逐层指向当前类型（base 为其成员，AST 存活整趟 sema）
		bool saw_array = false;
		while (node && *node)
		{
			const CTypePtr &cur = *node;
			switch (cur->kind)
			{
			case TypeKind::Array:
				saw_array = true;
				node = &cur->base; // 继续下探元素类型
				continue;
			case TypeKind::Pointer:
			case TypeKind::Function:
				return; // 指向/返回不完整类型合法，止步
			case TypeKind::Struct:
			case TypeKind::Union:
			{
				if (cur->tag.empty()) return;                  // 匿名内联定义按完整处理（合成 tag 匹配风险，列为边界）
				if (find_struct_def_by_tag(cur->tag)) return;  // 有真定义 ⇒ 完整
				const std::string elem = describe_type(cur);
				if (saw_array)                          report(loc, "array has incomplete element type " + elem);
				else if (ctx == IctxField)              report(loc, "field has incomplete type " + elem);
				else if (ctx == IctxGlobal)             report(loc, "tentative definition has type " + elem + " that is never completed");
				else                                    report(loc, "variable has incomplete type " + elem);
				return;
			}
			default:
				return; // 标量/void/enum 不纳入
			}
		}
	}

	// 变量类型查找（内层→外层→全局，未找到返回 null）。P254：提为成员，供
	//   SemaTypeEnv 与只读检查（需按根变量名回查其声明类型）共用同一口径。
	CTypePtr lookup_var_type(const std::string &n)
	{
		for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
		{
			auto f = it->find(n);
			if (f != it->end()) return f->second.type;
		}
		auto g = globals_.find(n);
		if (g != globals_.end()) return g->second;
		return nullptr;
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
			CTypePtr t = self->lookup_var_type(n);
			if (t) return t;
			// P300 Co class：类名作 VarRef（用于 :: 静态访问前缀）时返回 Class 类型
			auto cit = self->classes_.find(n);
			if (cit != self->classes_.end())
			{
				auto ct = make_type(TypeKind::Class);
				ct->tag = n;
				return ct;
			}
			return nullptr;
		}
		CTypePtr struct_member(const std::string &tag, const std::string &field)
		{
			return struct_member_impl(tag, field, 0);
		}
		// 成员名提升：直查不中时递归穿过匿名 struct/union 成员（匿名 union 保留嵌套）。
		CTypePtr struct_member_impl(const std::string &tag, const std::string &field, int depth)
		{
			if (depth > 16) return nullptr;
			// P300 Co class：先查 classes_ 成员（类名与 struct 名共享命名空间，不会同时命中）
			auto cit = self->classes_.find(tag);
			if (cit != self->classes_.end())
			{
				for (const auto &m : cit->second->members)
					if (!m.name.empty() && m.name == field) return m.type;
				// 静态成员也通过此路径返回类型
				for (const auto &m : cit->second->methods)
					if (m.name == field) return m.ret_type;
				return nullptr;
			}
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
		// 目标数据模型的 long 位宽（供共享解析器整型提升取正确宽度）。
		unsigned long_bits() const { return static_cast<unsigned>(self->opts_.target.sizeof_long) * 8u; }
	};

	CTypePtr resolve_type(const ExprPtr &e)
	{
		SemaTypeEnv env{this};
		return resolve_expr_type(e, env);
	}

	// —— P254：const 只读强制（可写位置的只读判定，clang 同文诊断）——
	// clang 把「写入只读对象」归为四形（91 例 clang 探针逐字坐实，_tmp/p254_probe.ps1）：
	//   A `cannot assign to variable 'X' with const-qualified type 'T'`
	//   B `read-only variable is not assignable`
	//   C `cannot assign to non-static data member 'm' with const-qualified type 'T'`
	//   D `cannot assign to variable 'X' with const-qualified data member 'm'`
	// 自增/自减与赋值同文（clang 不说 "cannot increment"）；类型拼写走 P252/P253 的
	//   `describe_type`（typedef 别名双轨 `CI' (aka 'const int')`、数组整维 `const int[3][2]` 自然成立）。
	// clang 另附 `note: … declared const here`：A/C 形已落地（P256）；B/D 形 clang 亦不附。
	// 判据：把左值沿「根变量 → 目标」展开为**逐层对象类型**，只看「最后一次指针间接」之下的
	//   对象链（该链之上的 const 属于别的对象，本次写入并不落到那里）：
	//   全程未经间接 → const 由根变量传播至被写子对象 → 可指名 → A 形；
	//   经指针间接取得 → 无从指名 → B 形；`->` 是 clang 特例：点指为 const 且箭头直接
	//   作用于根变量时，诊断锚在**该指针变量本身**、拼写取其声明类型
	//     （`const S *sp; sp->m = 1;` → A `'sp'` + `'const S *'`；`arr[0]->m` 则不报 / B 形）。
	//   成员自身声明为 const → C 形（clang 优先按成员定性，70 例：对象也 const 仍报 C 形）。
	// 不误伤（探针实测 clang 亦放行）：显式强转去 const、`int *const q` 写 `*q`、
	//   `const int **pp` 写 `*pp`、`S *const arr[2]` 写 `arr[0]->m`、volatile 对象
	//   （is_volatile 绝不参与只读判定）。
	enum WriteStep
	{
		WS_SUB_ARRAY,   // a[i]（a 为数组）
		WS_SUB_PTR,     // p[i]（p 为指针）
		WS_DEREF_ARRAY, // *a（a 为数组，等价 a[0]）
		WS_DEREF_PTR,   // *p
		WS_DOT,         // obj.field
		WS_ARROW        // ptr->field
	};

	// 左值 → (根变量名, 自根至目标的步序列)。不可分解（Cast/Call/Ternary/Comma/字面量等）返回 false。
	bool flatten_write_path(const ExprPtr &e, std::vector<std::pair<int, std::string>> &steps,
	                        std::string &root, int depth)
	{
		if (!e || depth > 16) return false;
		if (e->kind == ExprKind::VarRef) { root = e->name; return true; }
		if (e->kind == ExprKind::Unary && e->op == "*")
		{
			CTypePtr t = resolve_type(e->lhs);
			if (!t || (t->kind != TypeKind::Array && t->kind != TypeKind::Pointer)) return false;
			if (!flatten_write_path(e->lhs, steps, root, depth + 1)) return false;
			steps.emplace_back(t->kind == TypeKind::Array ? WS_DEREF_ARRAY : WS_DEREF_PTR, std::string());
			return true;
		}
		if (e->kind == ExprKind::Binary && e->op == "[]")
		{
			CTypePtr t = resolve_type(e->lhs);
			if (!t || (t->kind != TypeKind::Array && t->kind != TypeKind::Pointer)) return false;
			if (!flatten_write_path(e->lhs, steps, root, depth + 1)) return false;
			steps.emplace_back(t->kind == TypeKind::Array ? WS_SUB_ARRAY : WS_SUB_PTR, std::string());
			return true;
		}
		if (e->kind == ExprKind::Member)
		{
			if (!flatten_write_path(e->lhs, steps, root, depth + 1)) return false;
			steps.emplace_back(e->op == "->" ? WS_ARROW : WS_DOT, e->name);
			return true;
		}
		return false;
	}

	CTypePtr member_type_of(const CTypePtr &obj, const std::string &field)
	{
		if (!obj || field.empty()) return nullptr;
		SemaTypeEnv env{this};
		return env.struct_member(obj->tag, field);
	}

	// 查找结构体成员的声明位置（C 形 note 用）
	SourceLoc find_member_loc(const CTypePtr &obj, const std::string &field)
	{
		if (!obj || obj->tag.empty() || field.empty()) return {};
		auto sit = structs_.find(obj->tag);
		if (sit == structs_.end() || !sit->second) return {};
		for (const auto &m : sit->second->members)
			if (m.name == field) return m.loc;
		return {};
	}

	// 递归查找聚合类型（含嵌套 struct/union）中 const 成员的声明位置（field-unsafe note 用）
	SourceLoc find_const_member_rec_loc(const CTypePtr &t, int depth)
	{
		if (!t || depth > 4) return {};
		if (t->kind != TypeKind::Struct && t->kind != TypeKind::Union) return {};
		auto it = structs_.find(t->tag);
		if (it == structs_.end()) return {};
		for (const auto &m : it->second->members)
		{
			if (!m.type) continue;
			if (m.type->is_const) return m.loc;
			SourceLoc r = find_const_member_rec_loc(m.type, depth + 1);
			if (r.line) return r;
		}
		return {};
	}

	// 聚合类型是否含 const 成员（D 形判据；递归进入嵌套聚合，深度上限防环）
	bool find_const_member(const CTypePtr &t, std::string &out_name, int depth)
	{
		if (!t || depth > 4) return false;
		if (t->kind != TypeKind::Struct && t->kind != TypeKind::Union) return false;
		auto it = structs_.find(t->tag);
		if (it == structs_.end()) return false;
		for (const auto &m : it->second->members)
		{
			if (!m.type) continue;
			if (m.type->is_const) { out_name = m.name; return true; }
			if (find_const_member(m.type, out_name, depth + 1)) return true;
		}
		return false;
	}

	// 对象类型是否 const 限定。clang 的 `isConstQualified()` 对**数组类型看元素链**：
	//   `const int v[3]` 整个数组即算 const 限定（探针 75 例坐实）；colang 把 const 位
	//   落在叶元素上，故成员定性须沿数组链回推。指针不参与（点指 const ≠ 指针 const）。
	static bool is_const_obj_type(const CTypePtr &t, int depth)
	{
		if (!t || depth > 8) return false;
		if (t->is_const) return true;
		if (t->kind == TypeKind::Array) return is_const_obj_type(t->base, depth + 1);
		return false;
	}

	// 可写位置（赋值左值、++/-- 操作数）的只读检查
	void check_write_target(const ExprPtr &L, const SourceLoc &loc)
	{
		if (!L) return;
		std::vector<std::pair<int, std::string>> steps;
		std::string root;
		if (!flatten_write_path(L, steps, root, 0) || root.empty()) return;
		CTypePtr rt = lookup_var_type(root);   // 根变量声明类型（A 形一律拼它——clang 用 ND->getType()）
		if (!rt) return;
		const std::string ro = "read-only variable is not assignable";

		// ① 逐层推进：levels[k] = 第 k 步之后那个对象自身的类型（levels[0] = 根变量类型），
		//    owner[k] = 该层是否由成员选择取得（C 形指名用；`->` 的点指对象层不算成员）。
		//    `->` 拆为两层（点指对象 + 其成员），使「点指 const」与「成员 const」各自可见。
		//    记下**最后一次经指针间接**所得对象的层位：该层之上的 const 属于别的对象，
		//    本次写入并不落到那里（探针 56/58/60 例：`S *const arr[2]; arr[0]->m = 1;` 合法）。
		std::vector<CTypePtr> levels;
		std::vector<std::string> owner;   // levels[i] 由哪个成员选择取得（空 = 非成员）
		std::vector<int> producer;        // 产生 levels[i] 的步型（② 的回溯域依赖它）
		levels.push_back(rt);
		owner.push_back(std::string());
		producer.push_back(-1);
		int last_ind_level = -1;     // 最后一次间接所得对象在 levels 中的下标（-1 = 全程非间接）
		bool last_ind_arrow = false; // 该间接是否为 `->`
		CTypePtr t = rt;
		for (std::size_t i = 0; i < steps.size(); ++i)
		{
			if (!t) return;
			const int step = steps[i].first;
			const std::string &field = steps[i].second;
			CTypePtr next = nullptr;
			std::string own; // 本层是否由成员选择取得
			switch (step)
			{
			case WS_SUB_ARRAY:  // 数组下标 / 数组解引用：子对象，不跨间接
			case WS_DEREF_ARRAY:
				next = t->base;
				break;
			case WS_DOT:
				next = member_type_of(t, field);
				own = field;
				break;
			case WS_SUB_PTR:    // 指针下标 / 解引用：被写对象经指针取得
			case WS_DEREF_PTR:
				last_ind_level = static_cast<int>(levels.size());
				last_ind_arrow = false;
				next = t->base;
				break;
			case WS_ARROW:
				if (t->kind != TypeKind::Pointer) return;
				last_ind_level = static_cast<int>(levels.size());
				last_ind_arrow = true;
				{
					CTypePtr pte = t->base;          // 点指对象：不是「成员」，不参与 C 形定性
					levels.push_back(pte);
					owner.push_back(std::string());
					producer.push_back(WS_DEREF_PTR);
				}
				next = member_type_of(t->base, field); // 同时选取成员
				own = field;
				break;
			}
			t = next;
			levels.push_back(t);
			owner.push_back(own);
			producer.push_back(step);
		}
		if (!t) return;   // 类型推断不全 → 保守跳过（绝不误伤）

		// ② 写入链上碰得到「const 限定的成员」→ C 形（优先于 A 形，70 例）。
		//   clang 自最内层往外走这条链，取**最先（最内）遇到的 const 限定成员层**：
		//   非 const 成员层不截断回溯（90 例 `o.in.j`：j 非 const、in 为 `const struct IN6`
		//   → 报 'in'；88 例两者皆 const → 报最内层 'j'），但**指针间接截断**（87 例
		//   `x.v[0]->m`：v 为 `S *const[2]`、m 非 const → 不报）；数组下标不截断（75 例
		//   `t.v[0]`：v 为 `const int[3]` → 报 'v'；93 例 `o.in[1].j` → 报 'in' + `const struct IN7[2]`）。
		for (std::size_t i = levels.size(); i-- > 1; )
		{
			if (!owner[i].empty())
			{
				if (is_const_obj_type(levels[i], 0))
				{
					SourceLoc mloc = (i >= 1) ? find_member_loc(levels[i - 1], owner[i]) : SourceLoc{};
					if (mloc.line)
						report_with_note(loc, "cannot assign to non-static data member '" + owner[i] +
							"' with const-qualified type " + describe_type(levels[i]),
							mloc, "non-static data member '" + owner[i] + "' declared const here");
					else
						report(loc, "cannot assign to non-static data member '" + owner[i] +
							"' with const-qualified type " + describe_type(levels[i]));
					return;
				}
				continue; // 本成员非 const → 继续往外找
			}
			if (producer[i] != WS_SUB_ARRAY && producer[i] != WS_DEREF_ARRAY) break; // 跨间接 → 停
		}

		// ③ 被写对象（及其「最后一次间接」之下的子对象链）是否为 const
		const std::size_t lo = last_ind_level < 0 ? 0 : static_cast<std::size_t>(last_ind_level);
		for (std::size_t i = lo; i < levels.size(); ++i)
		{
			const CTypePtr &lv = levels[i];
			if (!lv || !lv->is_const) continue;
			// A 形：全程未跨间接（const 由根变量传播到被写子对象），或 `->` 直接作用于根变量
			//   本身（clang 把诊断锚回该指针变量，42/59 例）；B 形：经指针间接取得、无从指名。
			if (lo == 0 || (last_ind_arrow && lo == 1))
			{
				SourceLoc dloc = find_decl_loc(root);
				if (dloc.line)
					report_with_note(loc, "cannot assign to variable '" + root +
						"' with const-qualified type " + describe_type(rt),
						dloc, "variable '" + root + "' declared const here");
				else
					report(loc, "cannot assign to variable '" + root +
						"' with const-qualified type " + describe_type(rt));
			}
			else
				report(loc, ro);
			return;
		}

		// ④ D 形：整体写入一个「含 const 成员」的聚合。clang 按**左值形状**指名：
		//   变量 → `variable 't'`；成员 → `non-static data member 'in'`；其余 → `lvalue`
		//   （探针 18/62/63/69 例）。
		if (t->kind == TypeKind::Struct || t->kind == TypeKind::Union)
		{
			std::string m;
			if (find_const_member(t, m, 0) && !m.empty())
			{
				std::string anchor;
				if (L->kind == ExprKind::VarRef) anchor = "variable '" + root + "'";
				else if (L->kind == ExprKind::Member) anchor = "non-static data member '" + L->name + "'";
				else anchor = "lvalue";
				report(loc, "cannot assign to " + anchor +
					" with const-qualified data member '" + m + "'");
			}
		}
	}

	// 路 B 阶段1b-iii：限定访问 m.x / m.f() 解析与绑定（仅 per-module 转正路径）。
	//   形如 Member{op=".", lhs=VarRef(alias), name=member}：若 alias 是模块限定名（命中
	//   catalog_.qualified）且本地未声明同名变量（裁定②遮蔽：本地胜则不接管），则按 catalog
	//   解析目标并在 Member 上挂 q_func/q_global 指针绑定，同时标记基 VarRef 为限定符以抑制
	//   undeclared 诊断。命中返回 true（已处理）；否则返回 false（交常规成员访问路径）。
	bool try_bind_qualified_member(const ExprPtr &e)
	{
		if (!per_module_) return false;
		if (e->op != ".") return false;               // 仅 `.` 形（`->` 恒为指针成员）
		const ExprPtr &base = e->lhs;
		if (!base || base->kind != ExprKind::VarRef) return false;
		const std::string &alias = base->name;
		if (alias.empty()) return false;
		if (is_declared(alias)) return false;          // 本地同名变量胜出（遮蔽），走常规成员
		auto qit = catalog_.qualified.find(alias);
		if (qit == catalog_.qualified.end()) return false; // 非模块限定名
		auto vit = catalog_.by_module.find(qit->second);
		if (vit == catalog_.by_module.end())
		{
			report(e->loc, "reference to module '" + alias + "' before import");
			base->module_qualifier = true;
			return true; // 已报，不再叠加 undeclared / 非结构体基类误报
		}
		const ModuleCatalog::View &view = vit->second;
		const std::string &mname = e->name;
		auto fit = view.funcs.find(mname);
		if (fit != view.funcs.end())
		{
			e->q_func = fit->second;
			base->module_qualifier = true;
			return true;
		}
		auto git = view.globals.find(mname);
		if (git != view.globals.end())
		{
			e->q_global = git->second;
			if (!e->type) e->type = git->second->type; // 供裁定Ⅷ const 只读检查/类型查询
			base->module_qualifier = true;
			return true;
		}
		// 裁定⑥：非 public 不入导出面 → 限定访问不可命中（与按名 import 非 public 同口径）。
		report(e->loc, "no public symbol '" + mname + "' in module '" + alias + "'");
		base->module_qualifier = true;
		return true;
	}

	// —— 表达式检查 ——
	void check_expr(const ExprPtr &e)
	{
		if (!e) return;
		switch (e->kind)
		{
		case ExprKind::VarRef:
			if (e->module_qualifier) break; // 路 B 阶段1b-iii：限定访问基（模块名）不作变量检查
			if (!is_declared(e->name))
				report(e->loc, "use of undeclared identifier '" + e->name + "'");
			else
			{
				check_ambiguous_bare(e->name, e->loc); // P路 B 阶段1b：裁定③ host 裸名歧义
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
				// 路 B 阶段1b-iii：限定访问写目标 m.x = ... —— 先绑定；const 全局报只读（裁定Ⅷ）、
				//   非 const 与本地同权（不设跨模块只读门）；命中绑定则不走常规成员 assignability。
				if (L->kind == ExprKind::Member) try_bind_qualified_member(L);
				if (L->q_global)
				{
					if (L->q_global->type && L->q_global->type->is_const)
						report(e->loc, "read-only variable is not assignable");
				}
				else
				{
				bool bad = L->kind == ExprKind::IntLiteral || L->kind == ExprKind::FloatLiteral ||
				           L->kind == ExprKind::CharLiteral || L->kind == ExprKind::StringLiteral ||
				           L->kind == ExprKind::Call || L->kind == ExprKind::Comma ||
				           L->kind == ExprKind::Ternary;   // P254：逗号/三元作左值（clang 同文）
				// P254：`(T)x = 1` —— clang `assignment to cast is illegal, lvalue casts are not supported`，
				//   定位在强转的 `(`、**仅此一例**（修前 colang 一路走到 IR verifier 报
				//   `Store operand must be a pointer`）
				const bool cast_lvalue = L->kind == ExprKind::Cast;
				if (cast_lvalue) report(L->loc, "assignment to cast is illegal, lvalue casts are not supported");
				if (!bad && L->kind == ExprKind::VarRef)
				{
					CTypePtr t = resolve_type(L);
					if (t && t->kind == TypeKind::Array) bad = true; // 整数组赋值
				}
				if (cast_lvalue) { /* 已报，不叠加「不可赋值」*/ }
				else if (bad) report(e->loc, "expression is not assignable");
				// P254：可写位置的 const 只读检查（已判「不可赋值」的不重复报）
					else check_write_target(L, e->loc);
				}
			}
			// P300 Co class（§4.5）：类句柄赋值须源/目标同类，拒绝跨类/非句柄右值。
			if (L) check_class_handle_compat(resolve_type(L), e->rhs, e->loc);
			// C23 模型 B：nullptr（nullptr_t）只能赋给指针类型；赋给类型已知的非指针左值是约束违例。
			if (L && is_nullptr(e->rhs))
			{
				CTypePtr lt = resolve_type(L);
				if (lt && lt->kind != TypeKind::Pointer)
					report(e->loc, "assigning 'nullptr' to incompatible type (not a pointer)");
			}
			break;
		}
		case ExprKind::Unary:
			// P254：++/-- 的操作数是可写位置（clang 与赋值同文，不说 "cannot increment"）
			if (e->op == "++" || e->op == "--") check_write_target(e->lhs, e->loc);
			break;
		case ExprKind::Member:
		{
			// 路 B 阶段1b-iii：限定访问 m.x / m.f() —— 命中则已绑定，不走常规成员类型检查。
			if (try_bind_qualified_member(e)) break;
			// P300 Co class：:: 静态成员/方法访问，基类名已由 var_type 解析为 Class
			if (e->op == "::")
			{
				CTypePtr base = resolve_type(e->lhs);
				if (!base || base->kind != TypeKind::Class)
					report(e->loc, "'::' requires a class name on the left side");
				break;
			}
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
				check_ambiguous_bare(e->name, e->loc); // 路 B 阶段1b：裁定③ host 裸名歧义
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
		// P300 Co class（0.4.0）
		case ExprKind::New:
		{
			auto cit = classes_.find(e->name);
			if (cit == classes_.end())
			{
				report(e->loc, "unknown class name '" + e->name + "'");
				break;
			}
			// P300 Co class（跨模块 import）：宿主 `new C()` 引用他模块 class → 复用类型可见性门禁
			//   （public 且已 import 放行，否则「cannot import non-public symbol」/「is not imported」）。
			//   仅主源上下文触发（同 enforce_type_visible），子模块内部 new 自有 class 直接放行。
			if (cur_source_module_key_.empty() && cit->second && !cit->second->source_module_key.empty())
				ensure_visible_from_main(e->name, cit->second->source_module_key, cit->second->is_public, e->loc);
			break;
		}
		case ExprKind::ThisRef:
			if (!cur_class_)
				report(e->loc, "'this' is only available inside a class method");
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
				scopes_.back()[s->var_name] = {s->var_type, s->loc};
			enforce_type_visible(s->var_type, s->loc); // P217：局部声明类型跨模块可见性
			enforce_complete_type(s->var_type, s->loc, IctxVar); // P255：不完整类型按值分配前置诊断
			reject_class_pointer(s->var_type, s->loc); // P300：禁 `C* p;` 指向类指针
			if (s->var_init) check_class_handle_compat(s->var_type, s->var_init, s->var_init->loc); // P300：类句柄初始化同类约束
			// P256：-Wdefault-const-init-var-unsafe / -Wdefault-const-init-field-unsafe
			//   仅限局部非 static 无初始化器（全局/static 有零初始化保证，clang 不发）。
			if (!s->var_init && !s->is_static && !s->is_extern && s->var_type && cur_func_)
			{
				if (is_const_obj_type(s->var_type, 0))
					warn(s->loc, "default initialization of an object of type " +
						describe_type(s->var_type) + " leaves the object uninitialized");
				else
				{
					std::string cm;
					if (find_const_member(s->var_type, cm, 0) && !cm.empty())
					{
						SourceLoc mloc = find_const_member_rec_loc(s->var_type, 0);
						if (mloc.line)
							warn_with_note(s->loc, "default initialization of an object of type " +
								describe_type(s->var_type) + " with const member leaves the object uninitialized",
								mloc, "member '" + cm + "' declared 'const' here");
						else
							warn(s->loc, "default initialization of an object of type " +
								describe_type(s->var_type) + " with const member leaves the object uninitialized");
					}
				}
			}
			break;
		}
		case StmtKind::ExprStmt:
			if (s->expr) check_expr(s->expr);
			note_discarded_nodiscard(s->expr); // P249：[[nodiscard]] 返回值被丢弃
			break;
		case StmtKind::Delete:
		{
			// P300 Co class（0.4.0）：delete 仅接具名 class 句柄变量（A21）；拒 delete this / rvalue / 成员 / 非句柄。
			if (!s->expr) { report(s->loc, "expected class handle after 'delete'"); break; }
			check_expr(s->expr); // 复用名字/可见性解析（未声明等已由 check_expr 报）
			if (s->expr->kind == ExprKind::ThisRef)
				report(s->loc, "delete this is not allowed");
			else if (s->expr->kind != ExprKind::VarRef)
				report(s->loc, "delete requires a named class handle variable");
			else
			{
				// VarRef 的 Expr::type 未必回填，走共享类型解析器取声明类型。
				CTypePtr t = resolve_type(s->expr);
				if (t && t->kind != TypeKind::Class)
					report(s->loc, "delete operand must be a class handle");
				// t==null（未声明）已由 check_expr 报，不重复。
			}
			break;
		}
		case StmtKind::Return:
		{
			// P249：[[noreturn]] 函数体内出现 return（与 clang 同文的函数名形）
			if (cur_func_ && noreturn_funcs_.count(cur_func_->name))
				warn(s->loc, "function '" + cur_func_->name + "' declared 'noreturn' should not return");
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
			// P300 Co class（§4.4）：返类函数须返无主值（new / 返句柄的调用），禁 `return c;`
			//   返局部拥有句柄（否则 callee 计数与 caller release 会双重释放）。
			if (s->expr && cur_func_ && cur_func_->return_type &&
				cur_func_->return_type->kind == TypeKind::Class)
			{
				bool fresh = s->expr->kind == ExprKind::New || s->expr->kind == ExprKind::Call;
				CTypePtr rt = resolve_type(s->expr);
				if (!fresh && rt && rt->kind == TypeKind::Class)
					report(s->loc, "cannot return owned class handle; return a 'new' expression instead");
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

	// P300 Co class（§风险项#4）：按前序线性序收集 goto/label/class 句柄声明，供 check_goto_class_crossing 判跨越。
	void collect_goto_cross(const StmtPtr &s, int &seq,
		std::unordered_map<std::string, int> &label_seq,
		std::vector<int> &decl_seqs, std::vector<std::string> &decl_names,
		std::vector<std::string> &goto_labels, std::vector<int> &goto_seqs, std::vector<SourceLoc> &goto_locs)
	{
		if (!s) return;
		const int my = seq++;
		switch (s->kind)
		{
		case StmtKind::Label:
			label_seq.emplace(s->label_name, my); // 首次出现为准（重复已由 collect_labels 报错）
			break;
		case StmtKind::Goto:
			goto_labels.push_back(s->label_name); goto_seqs.push_back(my); goto_locs.push_back(s->loc);
			break;
		case StmtKind::Decl:
			if (s->var_type && s->var_type->kind == TypeKind::Class) { decl_seqs.push_back(my); decl_names.push_back(s->var_name); }
			break;
		default:
			break;
		}
		collect_goto_cross(s->init_stmt, seq, label_seq, decl_seqs, decl_names, goto_labels, goto_seqs, goto_locs);
		collect_goto_cross(s->for_init, seq, label_seq, decl_seqs, decl_names, goto_labels, goto_seqs, goto_locs);
		if (s->kind == StmtKind::Compound)
			for (const auto &b : s->body)
				collect_goto_cross(b, seq, label_seq, decl_seqs, decl_names, goto_labels, goto_seqs, goto_locs);
		else
		{
			collect_goto_cross(s->then_branch, seq, label_seq, decl_seqs, decl_names, goto_labels, goto_seqs, goto_locs);
			collect_goto_cross(s->else_branch, seq, label_seq, decl_seqs, decl_names, goto_labels, goto_seqs, goto_locs);
		}
	}

	// P300 Co class（§风险项#4）：禁止 goto 跨越「需自动清理（RC release）的 class 句柄声明」。
	//   heap-only 句柄出域按 RC 帧 release，等价于 C++「带非平凡析构的对象」：goto 前跳越过其初始化、
	//   或后跳回到其声明之前，均使目标路径上该句柄槽未初始化/重复注册，出域 release 打在栈垃圾上崩溃
	//   （本批 codegen 对前跳仅保守不释放、无法自证安全，故在 sema 层拒非法写法）。
	//   判据保守（循 clang「jump to label crosses initialization」口径）：goto 与其目标标签之间（前序序）
	//   夹有类句柄声明即拒；无类句柄/无 goto 早退。C 路径无句柄→零影响（plct 恒）。
	void check_goto_class_crossing(const StmtPtr &body)
	{
		if (!body) return;
		std::unordered_map<std::string, int> label_seq;
		std::vector<int> decl_seqs;
		std::vector<std::string> decl_names;
		std::vector<std::string> goto_labels;
		std::vector<int> goto_seqs;
		std::vector<SourceLoc> goto_locs;
		int seq = 0;
		collect_goto_cross(body, seq, label_seq, decl_seqs, decl_names, goto_labels, goto_seqs, goto_locs);
		if (decl_seqs.empty() || goto_labels.empty()) return; // 无类句柄或无 goto → 必安全
		for (std::size_t i = 0; i < goto_labels.size(); ++i)
		{
			auto lit = label_seq.find(goto_labels[i]);
			if (lit == label_seq.end()) continue; // 未声明标签（已由其它检查报错）
			const int lo = goto_seqs[i] < lit->second ? goto_seqs[i] : lit->second;
			const int hi = goto_seqs[i] < lit->second ? lit->second : goto_seqs[i];
			for (std::size_t j = 0; j < decl_seqs.size(); ++j)
				if (decl_seqs[j] > lo && decl_seqs[j] < hi)
				{
					report(goto_locs[i], "jump to label '" + goto_labels[i] +
						"' crosses initialization of class handle '" + decl_names[j] + "'");
					break;
				}
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
			if (!p.name.empty()) scopes_.back()[p.name] = {p.type, p.loc};
		labels_.clear();
		collect_labels(f.body);
		check_goto_class_crossing(f.body); // P300 §风险项#4：禁 goto 跨越需 RC 清理的类句柄声明
		// 非 void 函数可能执行到末尾而无 return（C 为 UB，clang 默认 -Wreturn-type 告警）。
		// main 例外：C11 §5.1.2.2.3 规定 main 落到 } 等价于 return 0，clang 亦不告警。
		if (f.return_type && f.return_type->kind != TypeKind::Void &&
			f.name != "main" && falls_through(f.body))
			warn(f.loc, "non-void function does not return a value in all control paths");
		// P249：[[noreturn]] 函数体允许落到末尾（等价于“会返回”）
		if (noreturn_funcs_.count(f.name) && falls_through(f.body))
			warn(f.loc, "function declared 'noreturn' should not return");
		check_stmt(f.body, 0, 0);
		cur_func_ = nullptr;
		cur_source_module_key_.clear();
	}

	const CompileOptions &opts_;
	std::vector<Error> &errors_;
	bool has_error_ = false;

	std::unordered_map<std::string, const FuncDecl *> funcs_;
	std::unordered_map<std::string, CTypePtr> globals_;
	// 路 B 阶段1b：全局名 → 归属模块 key（供跨模块「函数 vs 全局」不同种类重定义同模块守卫）。
	std::unordered_map<std::string, std::string> global_keys_;
	// P214：[[deprecated]] 登记表（name → 消息，空串 = 无附带理由）
	std::unordered_map<std::string, std::string> dep_funcs_;
	std::unordered_map<std::string, std::string> dep_globals_;
	// P249：[[nodiscard]] / [[noreturn]] 登记表（按名字，不依赖声明/定义归属）
	//   nodiscard 带可选理由字符串（与 clang 同形的 ": reason" 尾巴）
	std::unordered_map<std::string, std::string> nodiscard_funcs_;
	std::unordered_set<std::string> noreturn_funcs_;
	std::unordered_map<std::string, const StructDef *> structs_;
	// P217：enum 类型标签→定义（供跨模块类型可见性门禁）
	std::unordered_map<std::string, const EnumDef *> enums_;
	// P300 Co class（0.4.0）：类名→定义
	std::unordered_map<std::string, const ClassDef *> classes_;
	// P300：当前正在检查的类（非空表示在类方法体内，允许 this/裸成员名访问）
	const ClassDef *cur_class_ = nullptr;
	struct ScopeEntry { CTypePtr type; SourceLoc loc; };
	std::vector<std::unordered_map<std::string, ScopeEntry>> scopes_; // 末位为最内层
	std::unordered_map<std::string, SourceLoc> global_ident_locs_; // 全局变量名→声明位置
	std::unordered_set<std::string> labels_;
	std::unordered_set<std::string> dead_ends_; // 永不返回调用者的函数
	const FuncDecl *cur_func_ = nullptr;
	// P212：当前正在走查的代码归属模块 key（空 = 主源）。影响跨模块可见性检查开关。
	std::string cur_source_module_key_;
	// P212：主源 TU 指针（仅 run() 内使用），供可见性检查访问 imports/globals/enums。
	const TranslationUnit *main_tu_ = nullptr;
	// 路 B 阶段1（ambient 切断）：主源自身声明（含其 #include）的裸名集；命中则跨模块可见性门禁豁免。
	std::unordered_set<std::string> host_own_names_;
	// 路 B 阶段1b：per-module 转正开关（true 时 run() 末建 catalog；旧单遍路径恒 false、零改动）。
	bool per_module_ = false;
	// 路 B 阶段1b：逐模块 public 视图目录（per_module_ 为 true 时填充）。
	ModuleCatalog catalog_;
	using View = ModuleCatalog::View;
	// 路 B 阶段1b：裸名 → 导出该名的不同模块 key 集（host 裸名歧义裁定③：size>1 且非 host 自有→歧义）。
	std::unordered_map<std::string, std::unordered_set<std::string>> xmod_public_owners;
};

}  // namespace

bool analyze(const CompileOptions &opts, CompileState &state, std::vector<Error> &errors)
{
	if (!state.ast) return false;
	Sema sema(opts, errors);
	sema.run(state.ast);
	return sema.ok();
}

// 路 B 阶段1b：per-module 转正入口。当前增量仅置 per_module_ 建 catalog、宿主检查逻辑沿用现有 run()
//   （行为与旧单遍恒等、三门槛不变）；run_host/run_module 闭包逻辑在后续子步接入。
bool analyze_per_module(const CompileOptions &opts, CompileState &state, std::vector<Error> &errors,
                        ModuleCatalog *out_catalog)
{
	if (!state.ast) return false;
	Sema sema(opts, errors);
	sema.set_per_module(true);
	sema.run(state.ast);
	if (out_catalog) *out_catalog = sema.take_catalog();
	return sema.ok();
}

}  // namespace colang
