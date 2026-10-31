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

#include <algorithm>
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
		{
			if (s.tag.empty()) continue;
			structs_[s.tag] = &s;
			// P305：单建一份「真定义」索引（前置引用空壳不得盖住定义，成员名校核依赖它）。
			if (s.is_definition) real_struct_defs_[s.tag] = &s;
			// P344：裸名标签按模块分组（限定名/块内改名带 '.' 的键不进，仍走 structs_ 精确查表）。
			//   merge 把各模块 struct 并进同一 vector 且宿主的恒排最后，只靠 last-wins 会让
			//   模块内引用自身类型被宿主/他模块的同名项顶替——分组后由 record_def_of 按上下文挑。
			if (s.tag.find('.') == std::string::npos) struct_defs_by_tag_[s.tag].push_back(&s);
		}
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
		// P304：文件作用域 typedef 的导出面登记（全量，含非 public）。typedef 在 AST 里不留别名（parse
		//   期已展开），这两张表只用于两处诊断口径：写错模块别名、与非 public 的限定访问拒收。
		for (const auto &td : tu->typedefs)
		{
			if (td.name.empty()) continue;
			typedef_names_.insert(td.name);
			if (!td.source_module_key.empty()) typedef_owner_[td.name].insert(td.source_module_key);
		}
		// P302：限定类型名 `mod.Tag` 的别名键（依赖 build_catalog 已建的逐模块 public 视图）。
		if (per_module_) register_qualified_type_aliases(tu);
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
		// 0.4.1：全局/文件作用域初始化式的类型相容（原「全局初始化器暂不查」的保守口径收窄为
		//   「只查可判定的相容性」：聚合初始化、数组目标、未知类型与另族形态仍交既有机器件）
		for (const auto &g : tu->globals)
		{
			if (!g.init || g.is_extern || is_nullptr(g.init) || g.init->kind == ExprKind::InitList) continue;
			if (!g.type || g.type->kind == TypeKind::Array) continue;
			if (is_ternary_src(g.init) && !ternary_has_void_arm(g.init)) continue; // 条件运算符来源不判（P335：void 臂例外）
			const CTypePtr rt = resolve_type(g.init);
			const Compat cc = assign_compat(rt, g.type, g.init);
			if (cc != Compat::Ok && cc != Compat::Skip)
				report(cc == Compat::VoidValue && !g.ident_loc.file.empty() ? g.ident_loc : g.init->loc,
				       compat_init_msg(g.type, rt, cc));
		}
		// P324：文件作用域聚合初始化器元素位类型相容（上面对 InitList 跳过，此处补上）
		for (const auto &g : tu->globals)
			if (g.init && !g.is_extern && g.init->kind == ExprKind::InitList)
				check_aggregate_init(g.type, g.init);
		// P334：文件作用域初始化式里的二元操作数结构合法性（优先于 P332/P317；全局初值不走 check_expr）
		for (const auto &g : tu->globals)
			if (g.init && !g.is_extern) check_binary_operands_chain(g.init, 0);
		// P317：文件作用域初始化式里的指针差相容（不受上段 assign 口径限制，见 check_ptr_diff_chain）
		for (const auto &g : tu->globals)
			if (g.init && !g.is_extern) check_ptr_diff_chain(g.init, 0);
		// P323：文件作用域初始化式里的下标基型（同上：全局初值不走 check_expr）
		for (const auto &g : tu->globals)
			if (g.init && !g.is_extern) check_subscript_chain(g.init, 0);
		// P333：文件作用域初始化式里的 sizeof/_Alignof 操作数完整性（同上：全局初值不走 check_expr）
		for (const auto &g : tu->globals)
			if (g.init && !g.is_extern) check_sizeof_chain(g.init, 0);
		// P332：文件作用域初始化式里的不完整记录指针算术（同上：全局初值不走 check_expr）
		for (const auto &g : tu->globals)
			if (g.init && !g.is_extern) check_ptr_arith_chain(g.init, 0);
		// 仅检查函数体（全局初始化器的类型相容已在上段单独查）
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
			xmod_type_owners_[s.tag].insert(s.source_module_key); // P302：标签归属（歧义裁定用）
		}
		for (const auto &e : tu->enums)
		{
			if (e.source_module_key.empty() || !e.is_public || e.tag.empty()) continue;
			catalog_.by_module[e.source_module_key].enums[e.tag] = &e;
			xmod_type_owners_[e.tag].insert(e.source_module_key); // P302
		}
		// P300 Co class（跨模块 import）：逐模块 public class 视图（class 恒为定义、无 is_definition 位）。
		for (const auto &c : tu->classes)
		{
			if (c.source_module_key.empty() || !c.is_public || c.tag.empty()) continue;
			catalog_.by_module[c.source_module_key].classes[c.tag] = &c;
			xmod_type_owners_[c.tag].insert(c.source_module_key); // P302：类名同属类型命名空间
		}
		// P302：同名 public 标签被多个模块导出——定义侧无条件告警（与用户口径一致：同名即提）。
		//   宿主裸名引用将被拒（see enforce_type_visible），须改用限定名 `mod.Tag`。
		//   报告点取首个公开定义的位置；候选按「stem.Tag」列出，顺序稳定便于回归。
		for (const auto &kv : xmod_type_owners_)
		{
			if (kv.second.size() < 2) continue;
			std::vector<std::string> names;
			for (const auto &k : kv.second) names.push_back(alias_for_key(tu.get(), k));
			std::sort(names.begin(), names.end());
			std::string list;
			for (const auto &n : names) list += (list.empty() ? "" : ", ") + n + "." + kv.first;
			SourceLoc at{};
			bool have_loc = false;
			for (const auto &s : tu->structs)
				if (s.tag == kv.first && s.is_definition && !s.source_module_key.empty())
				{ at = s.loc; have_loc = true; break; }
			if (!have_loc)
				for (const auto &e : tu->enums)
					if (e.tag == kv.first && !e.source_module_key.empty())
					{ at = e.loc; have_loc = true; break; }
			if (!have_loc)
				for (const auto &c : tu->classes)
					if (c.tag == kv.first && !c.source_module_key.empty())
					{ at = c.loc; have_loc = true; break; }
			if (have_loc)
				warn(at, "tag '" + kv.first + "' is exported by multiple imported modules (" + list +
					"); unqualified references are an error, use a qualified name");
		}
		// 限定名映射：主源本层 imports 的 `as ModAlias`（无别名取文件 stem）→ resolved_module_key。
		//   consts 表暂空（public const int 的跨模块导出面留待限定访问实现步补齐）。
		for (const auto &d : tu->imports)
		{
			if (d.resolved_module_key.empty()) continue;
			const std::string alias = d.module_alias.empty() ? stem_of_key(d.resolved_module_key) : d.module_alias;
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
				f.symbol_name = "__" + sanitize_ident(stem_of_key(f.source_module_key)) + "__" + f.name;
		for (auto &g : tu->globals)
			if (!g.source_module_key.empty() && g_keys[g.name].size() > 1)
				g.symbol_name = "__" + sanitize_ident(stem_of_key(g.source_module_key)) + "__" + g.name;
	}

	// 模块 key → 文件名 stem（去扩展名）——默认限定名（与 alias 同名）。
	static std::string stem_of_key(const std::string &key)
	{
		std::string base = key;
		auto slash = base.find_last_of("/\\");
		if (slash != std::string::npos) base = base.substr(slash + 1);
		auto dot = base.find_last_of('.');
		if (dot != std::string::npos) base = base.substr(0, dot);
		return base;
	}

	// P302：模块 key 在宿主侧的**可书写限定名**：`as ModAlias` 优先，否则文件 stem。
	//   诊断候选必须用此口径——若一律报 stem，写了 `as a` 的用户照抄 `leaf_a.Leaf`
	//   会被 `unknown module alias` 拒收，把用户指向一个写不出来的名字。
	static std::string alias_for_key(const TranslationUnit *tu, const std::string &key)
	{
		if (tu)
			for (const auto &d : tu->imports)
				if (d.resolved_module_key == key && !d.module_alias.empty()) return d.module_alias;
		return stem_of_key(key);
	}

	// P302：把各导入模块的 public 类型以别名键 `alias.Tag` 注入本地类型表。
	//   宿主书写 `a.Leaf` 后，structs_/enums_/classes_ 的既有按 tag 查找路径（成员表、
	//   完整性检查、大小/对齐）原样命中该模块的那份定义，无需逐点改签名。
	void register_qualified_type_aliases(const TUPtr &tu)
	{
		for (const auto &d : tu->imports)
		{
			if (d.resolved_module_key.empty()) continue;
			const std::string alias = d.module_alias.empty() ? stem_of_key(d.resolved_module_key) : d.module_alias;
			auto vit = catalog_.by_module.find(d.resolved_module_key);
			if (vit == catalog_.by_module.end()) continue;
			for (const auto &kv : vit->second.structs) structs_[alias + "." + kv.first] = kv.second;
			for (const auto &kv : vit->second.enums) enums_[alias + "." + kv.first] = kv.second;
			for (const auto &kv : vit->second.classes) classes_[alias + "." + kv.first] = kv.second;
		}
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
		// P344：前缀项还须带上**自己的模块 key**——模块内裸名引用（含与宿主同名的非 public 记录）
		//   恒绑自身定义；只标哨兵 "imported" 时 record_def_of 认不出归属，会落到宿主同名项上误判成员。
		//   逐模块分段视图（loader 双写、拓序最深依赖在前）与摊平 body 的顺序相反，故按段逆序铺 key。
		const std::size_t prefix = tu->imported_top_level_body_count;
		std::vector<std::string> prefix_keys;
		prefix_keys.reserve(prefix);
		for (auto sit = tu->top_level_segments.rbegin(); sit != tu->top_level_segments.rend(); ++sit)
			prefix_keys.insert(prefix_keys.end(), sit->second.size(), sit->first);
		std::size_t idx = 0;
		for (const auto &s : tu->top_level_body)
		{
			if (!s) { ++idx; continue; }
			if (idx < prefix)
				cur_source_module_key_ = idx < prefix_keys.size() ? prefix_keys[idx] : std::string("imported");
			else
				cur_source_module_key_.clear();
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

	// P323：下标基型判据（C 6.5.2.1 只许数组或指向**对象**的指针——指向函数的指针不可下标）。
	//   修前直接复用 is_ptr_or_array，把 `Pointer{Function}` 与数组一并放行 → `fp[1]`/`tbl[0][0]`
	//   零诊断（运行期按函数体字节取址，实测 AV 与错值）。
	static bool is_subscriptable(const CTypePtr &t)
	{
		if (!t) return false;
		if (t->kind == TypeKind::Array) return true;
		if (t->kind == TypeKind::Pointer) return !t->base || t->base->kind != TypeKind::Function;
		return false;
	}

	// 下标基里的函数型：裸函数指示符（未 decay）或指向函数的指针 → 返回该**函数型本身**。
	//   clang 文案印未 decay 的原型（`subscript of pointer to function type 'int (void)'`），
	//   故此处只剥掉指针层，拼写交给 describe_type。
	static CTypePtr subscript_fn_type(const CTypePtr &t)
	{
		if (!t) return nullptr;
		if (t->kind == TypeKind::Function) return t;
		if (t->kind == TypeKind::Pointer && t->base && t->base->kind == TypeKind::Function) return t->base;
		return nullptr;
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

	// 严格同一性：限定符（const/volatile/restrict/_Atomic）与数组维度全同才算同一（`_Generic` 重复关联用）。
	//   不能直接用 `types_compatible`：它有意忽略 volatile/restrict/_Atomic 与数组维度，
	//   而 clang 实测接受 `_Generic(x, int: 1, volatile int: 2)`（CTS 0005_0005 误报后改走本谓词）。
	static bool same_type_strict(const CTypePtr &a, const CTypePtr &b)
	{
		if (!a || !b) return !a && !b;
		if (a->kind != b->kind) return false;
		if (a->is_const != b->is_const || a->is_volatile != b->is_volatile ||
		    a->is_restrict != b->is_restrict || a->is_atomic != b->is_atomic)
			return false;
		switch (a->kind)
		{
		case TypeKind::Pointer:
			return same_type_strict(a->base, b->base);
		case TypeKind::Array:
			return a->array_size == b->array_size && same_type_strict(a->base, b->base);
		case TypeKind::Function:
		{
			if (a->is_variadic != b->is_variadic || a->params.size() != b->params.size())
				return false;
			if (!same_type_strict(a->base, b->base)) return false;
			for (std::size_t i = 0; i < a->params.size(); ++i)
				if (!same_type_strict(a->params[i], b->params[i])) return false;
			return true;
		}
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

	// —— 0.4.1 类型相容性判定（赋值 / 初始化 / 实参三处共用）——
	// 与 `same_type`（原型冲突、忽略限定符）和 `types_compatible`（_Generic、逐层比限定符）语义均不同：
	//   这里回答「一个值能否赋入该对象」（C 6.5.16.1），且**只区分能对齐 clang 文本**的几类。
	//   刻意不判（Skip）而一律放行：整数↔指针（clang 走 -Wint-conversion 且带 `; take the address with &`
	//   fix-it）、目标丢限定符（clang 仅 warning）、Void、任一侧类型推断不出——防误伤。
	// ground-truth（clang 23.1.0 `-std=c23`，_tmp/p_compat_*.c 探针，2026-10-28）：
	//   AggMismatch  assigning to 'X' from incompatible type 'Y'
	//   PtrMismatch  incompatible pointer types assigning to 'X' from 'Y'
	//   FnPtrMismatch incompatible function pointer types …（源侧拼裸函数类型，不带 `*`）
	//   初始化/传参另有各自句式，见 compat_init_msg / compat_arg_msg；行尾 [-W…] 分组标记
	//   按本仓既有诊断惯例（全部不带）省略。
	// P322：指针↔整型的**隐式**转换另立两向（C23 起 clang 一律 error，属 -Wint-conversion 族）。
	//   不借道 PtrMismatch——那会把三句式印成 "incompatible pointer types"，与 clang 不同文。
	// P331：`void` 不是对象类型。作**源**时 clang 三句式一律印 "incompatible type 'void'"（**不是** "of type"），
	//   正好落 compat_{init,assign,arg}_msg 的**默认分支**；作**目标**（`vp[1] = …`）另立专属句。
	enum class Compat { Ok, AggMismatch, PtrMismatch, FnPtrMismatch, PtrToInt, IntToPtr, VoidValue, Skip };

	static bool is_arithmetic_kind(TypeKind k)
	{
		switch (k)
		{
		case TypeKind::Bool: case TypeKind::Char: case TypeKind::WChar:
		case TypeKind::Char8: case TypeKind::Char16: case TypeKind::Char32:
		case TypeKind::Short: case TypeKind::Int: case TypeKind::Long:
		case TypeKind::LongLong: case TypeKind::Float: case TypeKind::Double:
		case TypeKind::BitInt: case TypeKind::Enum:
			return true;
		default:
			return false;
		}
	}

	// P322：整型族（**不含** Float/Double，也不含 Bool）。Bool 由调用处单列放行——clang 对
	//   `bool b = 指针;` 只发 -Wpointer-bool-conversion（warning，cc=0，探针 u10g2:10），
	//   而 `enum E e = 指针;` 是 error（u10g2:11）→ 二者必须分开。
	static bool is_integer_kind(TypeKind k)
	{
		switch (k)
		{
		case TypeKind::Char: case TypeKind::WChar:
		case TypeKind::Char8: case TypeKind::Char16: case TypeKind::Char32:
		case TypeKind::Short: case TypeKind::Int: case TypeKind::Long:
		case TypeKind::LongLong: case TypeKind::BitInt: case TypeKind::Enum:
			return true;
		default:
			return false;
		}
	}

	// 判型侧衰减（C 6.3.2.1）：数组→元素指针、函数→函数指针
	static CTypePtr compat_decay(const CTypePtr &t)
	{
		if (!t) return nullptr;
		if (t->kind == TypeKind::Array) return make_pointer(t->base);
		if (t->kind == TypeKind::Function) return make_pointer(t);
		return t;
	}

	// P322：只判「整型常量表达式」的字面值，**算不出即 Unknown**（调用方据此放行）。
	//   C 6.3.2.3p3 把「值为 0 的整型常量表达式」定义为空指针常量；clang 对 `int *p = 1 - 1;`
	//   只发 -Wnon-literal-null-conversion（warning、cc=0，探针 u10h 十一形）⇒ 本仓必须放行。
	//   AST 上只能确定字面量/一元/二元三类；`sizeof`、Cast、变量与调用一律 Unknown。
	static bool try_eval_int(const ExprPtr &e, std::intmax_t &out)
	{
		if (!e) return false;
		if (e->kind == ExprKind::IntLiteral || e->kind == ExprKind::CharLiteral)
		{
			out = e->int_val;
			return true;
		}
		if (e->kind == ExprKind::Unary)
		{
			std::intmax_t v = 0;
			if (!try_eval_int(e->lhs, v)) return false;
			if (e->op == "+") { out = v; return true; }
			if (e->op == "-") { out = -v; return true; }
			if (e->op == "~") { out = ~v; return true; }
			if (e->op == "!") { out = v ? 0 : 1; return true; }
			return false;
		}
		if (e->kind == ExprKind::Binary)
		{
			std::intmax_t a = 0, b = 0;
			if (!try_eval_int(e->lhs, a) || !try_eval_int(e->rhs, b)) return false;
			const std::string &o = e->op;
			if (o == "+") { out = a + b; return true; }
			if (o == "-") { out = a - b; return true; }
			if (o == "*") { out = a * b; return true; }
			if (o == "/") { if (!b) return false; out = a / b; return true; }
			if (o == "%") { if (!b) return false; out = a % b; return true; }
			if (o == "&") { out = a & b; return true; }
			if (o == "|") { out = a | b; return true; }
			if (o == "^") { out = a ^ b; return true; }
			if (o == "<<") { if (b < 0 || b > 62) return false; out = a << b; return true; }
			if (o == ">>") { if (b < 0 || b > 62) return false; out = a >> b; return true; }
			return false;
		}
		return false;
	}

	// 三态：只有能算出**非零**才报；值为 0（空指针常量）与算不出的形（变量/sizeof/Cast）
	//   一律放行——`int *p = sizeof(int) - 4;` 在 parser 里未必折成字面量，若按「算不出即非零」
	//   处理就会误拒合法 C（首次落码实测 u10h 报 8:26/21:5 两行即是此错）。
	enum class IntEval { Unknown, Zero, NonZero };

	static IntEval eval_int_state(const ExprPtr &e)
	{
		std::intmax_t v = 0;
		if (!try_eval_int(e, v)) return IntEval::Unknown;
		return v == 0 ? IntEval::Zero : IntEval::NonZero;
	}

	// 「整型 - 指针」是非法二元式（C 6.5.6p2：减法的合法组合只有「指针-整型」与
	//   「指针-指针」）——clang 报 `invalid operands to binary expression ('long' and 'SH *')`
	//   并把整个式子定型为 poison，因此外层赋值**不再**报 int-conversion
	//   （ground-truth：探针 u10j:5 `1 - ip` / u10j:9 `(arr - ip) - ip` 均无该条；
	//    对照 w9:13 `long c4 = sa - ip - sp;` clang 只报 13:14 的差面）。
	// colang 未建模 poison 与 invalid-operands 族；若沿用 rhs 的指针型去判 PtrToInt，
	//   就会多报一条 clang 没有的诊断（P322 落码实测 w9:13:23 即此错）→ 该形状一律放行。
	// 需 resolve_type（非静态成员）→ 本函数与 assign_compat 均不带 static。
	bool is_int_minus_ptr(const ExprPtr &e)
	{
		if (!e || e->kind != ExprKind::Binary || e->op != "-") return false;
		const CTypePtr r = resolve_type(e->rhs);
		if (!r || r->kind != TypeKind::Pointer) return false;
		const CTypePtr l = compat_decay(resolve_type(e->lhs));
		return !l || l->kind != TypeKind::Pointer;
	}

	// 双侧指针：比较 pointee（多级指针逐层下钻）
	static Compat pointee_compat(const CTypePtr &f, const CTypePtr &t)
	{
		const CTypePtr fb = f->base;
		const CTypePtr tb = t->base;
		if (!fb || !tb) return Compat::Skip;
		// enum 的底层整型无法从 CType 还原：clang 对 enum* ↔ 整型* 走 `-Wpointer-sign`
		//   （「converts between pointers to integer types with different sign」）另族句式，
		//   不属本批四式 → 放行。
		if (fb->kind == TypeKind::Enum || tb->kind == TypeKind::Enum) return Compat::Skip;
		const bool ff = fb->kind == TypeKind::Function;
		const bool tf = tb->kind == TypeKind::Function;
		if (ff || tf)
		{
			// clang 对 `void *` ↔ 函数指针静默接受（对象/函数指针互转是扩展）→ 放行；
			//   「函数指针 ↔ 对象指针」非 void 时才是 incompatible pointer types。
			if (fb->kind == TypeKind::Void || tb->kind == TypeKind::Void) return Compat::Skip;
			if (!ff || !tf) return Compat::PtrMismatch;
			return same_type(fb, tb) ? Compat::Ok : Compat::FnPtrMismatch;
		}
		// 任一侧 void* ⇒ 相容（void* 与对象指针可隐式互转）
		if (fb->kind == TypeKind::Void || tb->kind == TypeKind::Void) return Compat::Ok;
		// 目标丢限定符（const int* → int*）：clang 只是 warning，本批不判
		if (!tb->is_const && fb->is_const) return Compat::Skip;
		// 指向数组：`int (*p)[4] = 多维数组名` 合法（元素同型即容，维度不符才错）；
		//   数组名本身已由 compat_decay 剥去最外层，这层比的是「元素类型」一侧的数组。
		if (fb->kind == TypeKind::Array || tb->kind == TypeKind::Array)
		{
			if (fb->kind != tb->kind) return Compat::PtrMismatch;
			if (fb->array_size && tb->array_size && fb->array_size != tb->array_size)
				return Compat::PtrMismatch;
			return same_type(fb->base, tb->base) ? Compat::Ok : Compat::PtrMismatch;
		}
		if (fb->kind == tb->kind)
		{
			if (fb->kind == TypeKind::Struct || fb->kind == TypeKind::Union || fb->kind == TypeKind::Class)
				return fb->tag == tb->tag ? Compat::Ok : Compat::PtrMismatch;
			if (fb->kind == TypeKind::Pointer) return pointee_compat(fb, tb);
			if (is_arithmetic_kind(fb->kind)) return Compat::Ok;
		}
		return Compat::PtrMismatch;
	}

	// `src` 仅 P322 的「整型→指针」向需要：判 `src` 是否空指针常量（算不出则放行）。
	// `compound` 标记复合赋值（`+=`/`-=`…）：其语义是 `p = p op n`，右侧整型只与**左值自身**
	//   参与算术，不与目标指针直接比对——`p += 2` 是合法指针算术（实测 p83_edge.co:19 误拒）。
	Compat assign_compat(const CTypePtr &from, const CTypePtr &to, const ExprPtr &src = nullptr,
	                     bool compound = false)
	{
		if (!from || !to) return Compat::Skip;
		// P331：`void` 不是对象类型。作源时三句式都印 "incompatible type 'void'"，正好落默认分支；
		//   作目标（`vp[1] = …`）由 Assign 分支的 `incomplete type 'void' is not assignable` 专管，
		//   这里放行以免双报。
		if (to->kind == TypeKind::Void) return Compat::Skip;
		if (from->kind == TypeKind::Void) return Compat::VoidValue;
		const CTypePtr f = compat_decay(from);
		const CTypePtr t = compat_decay(to);
		if (!f || !t) return Compat::Skip;
		const bool fp = f->kind == TypeKind::Pointer;
		const bool tp = t->kind == TypeKind::Pointer;
		if (fp != tp)
		{
			// P322：指针 ↔ 整型（C23 起 clang 一律 error）。两向分开报、句式与既有三式不同。
			//   · 指针→整型：目标为整型族即报（`bool` 不在 is_integer_kind 内 → 放行）；
			//   · 整型→指针：仅能算出**非零**的整型常量才报；值为 0 的整型常量表达式是空指针
			//     常量（u10h 十一形 clang cc=0），算不出的形（变量/sizeof/Cast）亦放行（宁漏不错）。
			if (fp && is_integer_kind(t->kind))
			{
				// 「整型 - 指针」非法式的 poison 结果不判（见 is_int_minus_ptr）
				if (is_int_minus_ptr(src)) return Compat::Skip;
				return Compat::PtrToInt;
			}
			if (tp && !compound && is_integer_kind(f->kind) && eval_int_state(src) == IntEval::NonZero)
				return Compat::IntToPtr;
			// P326：指针 → 非整型/非 bool/非 void 目标（浮点、聚合）。clang 一律 error 且用
			//   通用 "incompatible type" 句式（落 compat_{init,assign,arg}_msg 的默认分支，零新句式）。
			//   · bool 目标仅 -Wpointer-bool-conversion（warning、cc=0，探针 p326_init:19/assign:11/arg:15）
			//     → 必须放行，不入本判据；
			//   · void 目标已由函数首部 to->kind==Void 早退（避免与 "not assignable" 专管双报）；
			//   · 「整型 - 指针」poison 式同 PtrToInt 侧，一并放行（见 is_int_minus_ptr）；
			//   · 反向浮点→指针（int *p = 1.5）走 tp 侧、本批不收（真值 clang 报同句，拆登后续批次）。
			if (fp && t->kind != TypeKind::Bool && !is_integer_kind(t->kind) &&
				!is_int_minus_ptr(src))
				return Compat::AggMismatch;
			return Compat::Skip; // 整数↔浮点等仍另族（本就不判）
		}
		if (fp) return pointee_compat(f, t);
		if (is_arithmetic_kind(f->kind) && is_arithmetic_kind(t->kind)) return Compat::Ok;
		// class 句柄同类约束交 check_class_handle_compat（避免双报）
		if (f->kind == TypeKind::Class && t->kind == TypeKind::Class) return Compat::Ok;
		if ((f->kind == TypeKind::Struct || f->kind == TypeKind::Union) &&
			f->kind == t->kind && f->tag == t->tag)
			return Compat::Ok;
		return Compat::AggMismatch;
	}

	// `char` 与 `signed char` 在 CType 里同为 Char 且 is_unsigned=false（不可区分），
	//   而 clang 视二者为不同类型 → 涉 Char 叶子的「相容」判定一律放行，宁可漏报不误报。
	static bool has_char_leaf(const CTypePtr &t)
	{
		if (!t) return false;
		if (t->kind == TypeKind::Char) return true;
		if (t->kind == TypeKind::Pointer || t->kind == TypeKind::Array || t->kind == TypeKind::Function)
			return has_char_leaf(t->base);
		return false;
	}

	// —— P317：指针差的 pointee 相容（C 6.5.9p4：两个操作数须指向相容类型）——
	// **不得复用 `pointee_compat`**：它与本判据在两条规则上正好相反。
	//   · `void *` ↔ 对象指针：赋值相容（可隐式互转，那里判 Ok），指针差 clang **拒**
	//     （探针 x5.c：`7:13 'void *' and 'int *' are not pointers to compatible types`）；
	//     两侧都是 void 才接受（GNU 扩展按字节差，探针 x1.c/y2.c：cc=0/run=0）。
	//   · 限定符差异（`int *` ↔ `const int *`）：指针差 clang **接受**（探针 w4f.c）
	//     → 这里不比 is_const/is_volatile/restrict。
	// ground-truth（clang 23.1.0 `-std=c23`，探针 w4b/w4c/w4e/w4f/w4g/w4j）：
	//   拒 short*↔int*、char*↔int*、long*↔int*、unsigned int*↔int*、void*↔对象指针、
	//     struct A*↔struct B*、int**↔int*、int(*)[3]↔int(*)[4]、enum E1*↔enum E2*、
	//     异原型函数指针、函数指针↔对象指针、char*↔unsigned char*；
	//   接受 int*↔const int*、void*↔void*、void*↔const void*、enum*↔int*、同型异对象、
	//     数组名 decay 后同型（int[3] ↔ int[4] 的 decay 结果均为 int*）。
	// 一律放行（宁漏不错、绝不新增误拒）：任一侧类型推断不出、超过深度护栏、
	//   `char` ↔ `signed char`（CType 里同为 Char 且 is_unsigned=false，不可区分）、
	//   `enum *` ↔ 其它整型指针（底层型无法从 CType 还原）。
	static bool pd_pointee(const CTypePtr &a, const CTypePtr &b, int depth)
	{
		if (!a || !b || depth > 8) return true;
		if (a->kind == TypeKind::Void || b->kind == TypeKind::Void)
			return a->kind == b->kind;            // void↔void 相容；void↔其余不相容
		if (a->kind == TypeKind::Enum || b->kind == TypeKind::Enum)
		{
			if (a->kind != b->kind) return is_arithmetic_kind(a->kind) && is_arithmetic_kind(b->kind);
			return a->tag == b->tag;              // enum E1* ↔ enum E2*：clang 拒
		}
		if (a->kind != b->kind) return false;      // short*↔int*、char*↔int*、数组↔标量…
		// Char：`char` 与 `signed char` 在 CType 里同为 is_unsigned=false（不可区分）
		//   → 该对一律判相容（clang 拒、colang 漏报，属知情缺口）；而 `unsigned char`
		//   是 Char + is_unsigned=true，与二者都不同形 → 按符号位判（clang 同拒，探针 w5.c:22）。
		if (a->kind == TypeKind::Char) return a->is_unsigned == b->is_unsigned;
		if (is_arithmetic_kind(a->kind))
		{
			if (a->is_unsigned != b->is_unsigned) return false; // int*↔unsigned int*：clang 拒
			if (a->kind == TypeKind::BitInt && a->bit_width != b->bit_width) return false;
			return true;
		}
		if (a->kind == TypeKind::Pointer) return pd_pointee(a->base, b->base, depth + 1);
		if (a->kind == TypeKind::Array)
		{
			// `int (*)[3]` ↔ `int (*)[4]`：clang 拒（探针 w4e.c）；一维未定（size==0）不比
			if (a->array_size && b->array_size && a->array_size != b->array_size) return false;
			return pd_pointee(a->base, b->base, depth + 1);
		}
		if (a->kind == TypeKind::Function) return same_type(a, b); // 异原型不相容
		if (a->kind == TypeKind::Struct || a->kind == TypeKind::Union || a->kind == TypeKind::Class)
			return a->tag == b->tag;
		return true;                                // 其余形不建模：一律放行
	}

	// 条件运算符来源不判：colang 未建模 `?:` 的复合类型（分支不相容时 clang 取 `void *`，
	//   诊断落在 `-Wpointer-type-mismatch` 另族「pointer type mismatch ('int **' and 'const int **')」，
	//   且转出的赋值本身被接受，见 CTS 0202_0097/0100/0103/0106）→ 三处接入一律放行，宁可漏报。
	static bool is_ternary_src(const ExprPtr &e)
	{
		return e && e->kind == ExprKind::Ternary;
	}

	// P335：三元式含 Void 臂时不再「整体不判」（clang C 6.5.15p5 结果即 void，
	//   相容检查自然命中 VoidValue，无 pointer-type-mismatch 误拒风险）。
	bool ternary_has_void_arm(const ExprPtr &e)
	{
		if (!e || e->kind != ExprKind::Ternary || e->args.size() < 2) return false;
		CTypePtr a = resolve_type(e->args[0]);
		CTypePtr b = resolve_type(e->args[1]);
		return (a && a->kind == TypeKind::Void) || (b && b->kind == TypeKind::Void);
	}

	// 赋值形态
	static std::string compat_assign_msg(const CTypePtr &to, const CTypePtr &from, Compat c)
	{
		const std::string t = describe_type(to);
		const std::string f = describe_type(from);
		if (c == Compat::PtrToInt)
			return "incompatible pointer to integer conversion assigning to " + t + " from " + f;
		if (c == Compat::IntToPtr)
			return "incompatible integer to pointer conversion assigning to " + t + " from " + f;
		if (c == Compat::PtrMismatch)
			return "incompatible pointer types assigning to " + t + " from " + f;
		if (c == Compat::FnPtrMismatch)
			return "incompatible function pointer types assigning to " + t + " from " + f;
		return "assigning to " + t + " from incompatible type " + f;
	}

	// 初始化形态（clang 对指针用 "with an expression of type"，非指针用 "of incompatible type"）
	static std::string compat_init_msg(const CTypePtr &to, const CTypePtr &from, Compat c)
	{
		const std::string t = describe_type(to);
		const std::string f = describe_type(from);
		if (c == Compat::PtrToInt)
			return "incompatible pointer to integer conversion initializing " + t + " with an expression of type " + f;
		if (c == Compat::IntToPtr)
			return "incompatible integer to pointer conversion initializing " + t + " with an expression of type " + f;
		if (c == Compat::PtrMismatch)
			return "incompatible pointer types initializing " + t + " with an expression of type " + f;
		if (c == Compat::FnPtrMismatch)
			return "incompatible function pointer types initializing " + t + " with an expression of type " + f;
		return "initializing " + t + " with an expression of incompatible type " + f;
	}

	// 实参形态（源侧在前、目标形参在后）
	static std::string compat_arg_msg(const CTypePtr &to, const CTypePtr &from, Compat c)
	{
		const std::string t = describe_type(to);
		const std::string f = describe_type(from);
		if (c == Compat::PtrToInt)
			return "incompatible pointer to integer conversion passing " + f + " to parameter of type " + t;
		if (c == Compat::IntToPtr)
			return "incompatible integer to pointer conversion passing " + f + " to parameter of type " + t;
		if (c == Compat::PtrMismatch)
			return "incompatible pointer types passing " + f + " to parameter of type " + t;
		if (c == Compat::FnPtrMismatch)
			return "incompatible function pointer types passing " + f + " to parameter of type " + t;
		return "passing " + f + " to parameter of incompatible type " + t;
	}

	// ============================================================
	// P324：聚合初始化器 `{...}` 元素位类型相容（C 6.7.9）
	//   每个元素位按其目标子类型做一次 assign_compat（等价标量初始化/赋值口径），
	//   不合法即报 compat_init_msg 文案、定位在该元素表达式自身 loc。
	//   游标镜像 codegen::elide_fold（brace elision），但工作在 CType 层（非 llvm::Type）：
	//     · Array 槽型 = to->base；Struct 逐 C 成员 members[i].type（经 find_struct_def_by_tag 回查）；
	//       Union 裸初值 → 首成员，`.m=` → 指定成员。
	//     · 元素是子 InitList → 对该槽目标型整体递归；标量元素落聚合槽 → brace elision 逐叶子消费。
	//   一律放行（宁漏不错、绝不新增误拒）：型解不出、struct/union 定义不完整、三元无 void 臂源、
	//     链式设计符（desig_chain≥2，交 codegen 物理路径）、字符串进字符数组（不触及元素位）。
	// ============================================================
	static bool agg_slot(const CTypePtr &t)
	{
		return t && (t->kind == TypeKind::Array || t->kind == TypeKind::Struct || t->kind == TypeKind::Union);
	}

	// P349：字符串字面量可整体初始化的元素型（§6.7.9p14「array of character type」）。
	//   `unsigned char` 在 CType 里是 Char + is_unsigned=true（与 char 同 kind）→ 一并覆盖。
	static bool is_char_elem(const CTypePtr &t)
	{
		if (!t) return false;
		return t->kind == TypeKind::Char || t->kind == TypeKind::Char8 || t->kind == TypeKind::WChar ||
		       t->kind == TypeKind::Char16 || t->kind == TypeKind::Char32;
	}

	// 标量槽位：对表达式做一次 assign_compat（复用三元/常量性既有放行护栏）
	void check_scalar_slot(const CTypePtr &to, const ExprPtr &it)
	{
		if (!to || !it) return;
		if (is_ternary_src(it) && !ternary_has_void_arm(it)) return; // 条件运算符来源不判（防误拒）
		const CTypePtr ft = resolve_type(it);
		const Compat cc = assign_compat(ft, to, it);
		if (cc != Compat::Ok && cc != Compat::Skip)
			report(it->loc, compat_init_msg(to, ft, cc));
	}

	// 消费聚合对象 `to` 的一个子对象槽 slot：当前项带括号 → 作为 slot 自己的 brace 组递归；
	//   当前项扁平且 slot 是聚合 → brace elision（共享游标继续逐叶子）；否则标量 → assign_compat
	void agg_consume(const CTypePtr &slot, const std::vector<ExprPtr> &args, std::size_t &pos, int depth)
	{
		if (!slot || depth > 16 || pos >= args.size()) return;
		if (args[pos] && args[pos]->kind == ExprKind::InitList) // 带括号子初始化器 → 它是 slot 的花括号内容
		{
			ExprPtr sub = args[pos++];
			agg_walk_init(slot, sub, depth + 1);
			return;
		}
		if (agg_slot(slot))
		{
			// 非括号项若是**与槽同型的聚合表达式**（struct/union）→ 整体赋值（clang 接受），不 elide；
			//   否则为 brace elision（异型聚合/标量扁平填入子对象），共享游标逐叶子。
			const CTypePtr nt = args[pos] ? resolve_type(args[pos]) : nullptr;
			if (nt && (nt->kind == TypeKind::Struct || nt->kind == TypeKind::Union) &&
			    assign_compat(nt, slot, args[pos]) == Compat::Ok)
			{
				ExprPtr whole = args[pos++];
				check_scalar_slot(slot, whole); // 同型聚合 → Ok，静默放行
				return;
			}
			agg_walk_pos(slot, args, pos, depth + 1);
			return;
		}
		ExprPtr it = args[pos++];
		if (it && it->kind == ExprKind::InitList && it->args.size() == 1 && it->args[0]) it = it->args[0]; // {x}≡x
		if (it) check_scalar_slot(slot, it);
	}

	// 位置式游标（镜像 elide_fold）：`args` 是聚合对象 `to` 的花括号内容，逐子对象槽消费
	void agg_walk_pos(const CTypePtr &to, const std::vector<ExprPtr> &args, std::size_t &pos, int depth)
	{
		if (!to || depth > 16) return;
		if (!agg_slot(to)) // 防御：标量对象直接到达（正常经 agg_consume 已拆包）
		{
			if (pos >= args.size()) return;
			ExprPtr it = args[pos++];
			if (it && it->kind == ExprKind::InitList && it->args.size() == 1 && it->args[0]) it = it->args[0];
			if (it) check_scalar_slot(to, it);
			return;
		}
		if (to->kind == TypeKind::Array)
		{
			// 字符串字面量整体初始化字符数组（§6.7.9p14）：不逐元素（不触及元素位）
			if (args.size() == 1 && args[0] && args[0]->kind == ExprKind::StringLiteral) return;
			// P349：brace elision 形态（`struct T {u8 s[4]; u8 a;} t = {"abc", 1};`）——此时 args 是
			//   外层花括号内容、字符串只是其中一项，上一条判据不成立（旧行为把游标降到元素 u8，
			//   报「指针转整数」。clang 接受，属误拒）。改按「当前项是字符串 + 元素是字符型」
			//   消费一整格，与 codegen elide_fold/elide_store 同口径；其余元素由零初始化补齐。
			if (pos < args.size() && args[pos] && args[pos]->kind == ExprKind::StringLiteral && is_char_elem(to->base))
			{
				pos++;
				return;
			}
			for (std::size_t k = 0; k < to->array_size && pos < args.size(); ++k)
				agg_consume(to->base, args, pos, depth);
			return;
		}
		if (to->kind == TypeKind::Union)
		{
			if (pos >= args.size()) return;
			if (to->tag.empty()) { pos++; return; }
			const StructDef *sd = find_struct_def_by_tag(to->tag);
			if (!sd || !sd->is_definition || sd->members.empty()) { pos++; return; } // 不完整 → 放行并消费一项防错位
			agg_consume(sd->members[0].type, args, pos, depth); // union 仅填首成员（§6.7.9p19）
			return;
		}
		// struct：逐 C 成员位共享游标
		if (to->tag.empty()) return;
		const StructDef *sd = find_struct_def_by_tag(to->tag);
		if (!sd || !sd->is_definition) return; // 不完整/未知 → 放行
		for (const auto &m : sd->members)
		{
			if (pos >= args.size()) break;
			agg_consume(m.type, args, pos, depth);
		}
	}

	// 把单项元素应用到槽型 slot（子 InitList→递归；同型聚合整值→放行；标量落聚合槽→brace elision；否则标量检查）
	void agg_apply_to_slot(const CTypePtr &slot, const ExprPtr &elem, int depth)
	{
		if (!slot || !elem || depth > 16) return;
		if (elem->kind == ExprKind::InitList) { agg_walk_init(slot, elem, depth + 1); return; }
		if (agg_slot(slot))
		{
			// P349：右值是**与槽同型的聚合表达式**（`.m = s`、`.m = p->m`）→ 整值初始化，不 elide。
			//   旧版直接落 brace elision，把 struct 值当首成员链的叶子消费，报「用 struct X 初始化 u8」
			//   （clang 接受，属误拒）。与上方 agg_consume 的同型整值判据同口径。
			const CTypePtr nt = resolve_type(elem);
			if (nt && (nt->kind == TypeKind::Struct || nt->kind == TypeKind::Union) &&
			    assign_compat(nt, slot, elem) == Compat::Ok)
			{
				check_scalar_slot(slot, elem);
				return;
			}
			std::vector<ExprPtr> one{elem};
			std::size_t pos = 0;
			agg_walk_pos(slot, one, pos, depth + 1);
			return;
		}
		check_scalar_slot(slot, elem);
	}

	// 指定式：命中一级 `[idx]`/`.field` → 定位槽后应用；链式（desig_chain≥2）跳过
	void agg_walk_desig(const CTypePtr &to, const std::vector<ExprPtr> &args, int depth)
	{
		if (!to || depth > 16) return;
		const StructDef *sd = nullptr;
		if (to->kind == TypeKind::Struct || to->kind == TypeKind::Union)
		{
			if (to->tag.empty()) return;
			sd = find_struct_def_by_tag(to->tag);
			if (!sd || !sd->is_definition || sd->members.empty()) return; // 不完整/未知 → 放行
		}
		for (const auto &ae : args)
		{
			if (!ae) continue;
			if (ae->desig_chain.size() >= 2) continue; // 链式交 codegen 物理路径（本批不建模，知情边界）
			CTypePtr slot;
			if (to->kind == TypeKind::Array)
			{
				if (ae->desig_index >= 0) slot = to->base;
			}
			else if (sd)
			{
				if (!ae->desig_field.empty())
					for (const auto &m : sd->members) if (m.name == ae->desig_field) { slot = m.type; break; }
				else if (ae->desig_index >= 0 && static_cast<std::size_t>(ae->desig_index) < sd->members.size())
					slot = sd->members[static_cast<std::size_t>(ae->desig_index)].type; // GNU `[idx]` 指成员
			}
			if (!slot) continue; // 定位不到槽 → 放行（宁漏不错）
			agg_apply_to_slot(slot, ae, depth);
		}
	}

	// 对一个 InitList 分派：含设计符 → 指定式；否则 → 位置式游标
	void agg_walk_init(const CTypePtr &to, const ExprPtr &init, int depth)
	{
		if (!to || !init || init->kind != ExprKind::InitList || depth > 16) return;
		bool has_desig = false;
		for (const auto &ae : init->args)
			if (ae && (ae->desig_index >= 0 || !ae->desig_field.empty() || !ae->desig_chain.empty())) { has_desig = true; break; }
		if (has_desig) agg_walk_desig(to, init->args, depth);
		else { std::size_t pos = 0; agg_walk_pos(to, init->args, pos, depth); }
	}

	// 对外入口：仅对聚合目标（Array/Struct/Union）展开；标量目标写 `{}` 非本批形态 → 放行
	void check_aggregate_init(const CTypePtr &to, const ExprPtr &init)
	{
		if (!to || !init || init->kind != ExprKind::InitList) return;
		if (!agg_slot(to)) return;
		agg_walk_init(to, init, 0);
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
		// P302：限定名 `alias.Tag` —— 按「别名 → 模块 key → 该模块的定义」精确定位，
		//   不再回落首匹配（否则宿主写 a.Leaf 可能拿到 b 的布局）。
		//   注意：点号不是限定名的专利——parser 把块内定义的标签改名为 `X.__blkN`，
		//   故首段不属于导入别名时必走下方裸 tag 精确匹配。
		const auto dot = tag.find('.');
		if (dot != std::string::npos)
		{
			const std::string alias = tag.substr(0, dot);
			const std::string nm = tag.substr(dot + 1);
			auto qit = catalog_.qualified.find(alias);
			if (qit != catalog_.qualified.end())
			{
				for (const auto &s : main_tu_->structs)
					if (s.tag == nm && s.is_definition && s.source_module_key == qit->second) return &s;
				return nullptr;
			}
		}
		for (const auto &s : main_tu_->structs)
			if (s.tag == tag && s.is_definition) return &s;
		return nullptr;
	}

	// —— P305：成员名校核（`.` / `->` 指向记录类型时，成员名不存在不得静默编过）——
	// 修前：`struct Point { int x; int y; }; p.z = 1;` 无任何诊断，codegen 走
	//   `resolve_member_info` 未命中分支的默认 0 号槽，按基址写内存 → 越界/静默错写。
	// 诊断口径逐字对 clang（探针 _tmp\memb\g1~g8 实测）：
	//   命名记录 → `no member named 'z' in 'struct Point'`（经 typedef 访问亦拼 tag，不用别名糖）；
	//   `typedef struct {…} A;` 匿名记录 → `no member named 'z' in 'A'`；
	//   只有前置声明 → `incomplete definition of type 'struct I'` + 前置声明 note；
	//   `->` 的解引用基不是记录（如 `void *`/`int *`）→ `member reference base type 'void' is not a structure or union`。
	const StructDef *record_def_of(const std::string &tag) const
	{
		if (tag.empty()) return nullptr;
		// P344：裸名 tag 可能同时存在多份（宿主一份、各被导入模块各一份，merge 只把它们并进同一
		//   vector 且宿主排末位）。旧实现按 structs_ 的 last-wins 取，模块内引用自身记录会拿到宿主
		//   或他模块的同名定义 → 成员名校核误报 `no member named`。改按「当前代码归属」挑：
		//   ① 归属模块自己的定义 ② 宿主自有定义 ③ 其余定义 ④ 只剩前置引用空壳。
		//   带 '.' 的键（限定名 `alias.Tag` / 块内改名 `X.__blkN`）本就是精确键，走 structs_。
		if (tag.find('.') == std::string::npos)
		{
			auto git = struct_defs_by_tag_.find(tag);
			if (git != struct_defs_by_tag_.end())
			{
				for (const StructDef *s : git->second)
					if (s && s->is_definition && !s->source_module_key.empty() &&
					    s->source_module_key == cur_source_module_key_)
						return s;
				for (auto rit = git->second.rbegin(); rit != git->second.rend(); ++rit)
					if (*rit && (*rit)->is_definition && (*rit)->source_module_key.empty())
						return *rit;
				for (auto rit = git->second.rbegin(); rit != git->second.rend(); ++rit)
					if (*rit && (*rit)->is_definition) return *rit;
				return git->second.back(); // 只有前置声明的空壳
			}
		}
		auto it = structs_.find(tag);
		if (it != structs_.end() && it->second && it->second->is_definition) return it->second;
		auto rd = real_struct_defs_.find(tag);
		if (rd != real_struct_defs_.end()) return rd->second;
		if (it != structs_.end()) return it->second; // 只有前置声明的空壳
		return nullptr;
	}

	// 记录名拼写（带引号）：命名记录一律 `struct/union Tag`，匿名记录回退到 typedef 别名，
	//   合成 tag（`__anon_struct_N`）与块内改名后缀（`X.__blkN`）绝不外泄到诊断。
	std::string spell_record(const CTypePtr &rec) const
	{
		if (!rec) return "<type>";
		if (rec->kind == TypeKind::Class) return "'" + spell_tag(rec->tag) + "'";
		if (rec->tag.rfind("__anon_", 0) == 0)
			return rec->alias.empty() ? std::string("<anonymous record>") : "'" + rec->alias + "'";
		return std::string("'") + (rec->kind == TypeKind::Union ? "union " : "struct ") + spell_tag(rec->tag) + "'";
	}

	// 成员名是否存在：含穿过匿名 struct/union 的名提升（与 struct_member_impl 同口径）。
	bool struct_has_member(const StructDef *sd, const std::string &field, int depth) const
	{
		if (!sd || !sd->is_definition || depth > 16) return false;
		for (const auto &m : sd->members)
			if (!m.name.empty() && m.name == field) return true;
		for (const auto &m : sd->members)
		{
			if (m.name.empty() && m.type &&
				(m.type->kind == TypeKind::Struct || m.type->kind == TypeKind::Union) &&
				!m.type->tag.empty())
			{
				if (struct_has_member(record_def_of(m.type->tag), field, depth + 1)) return true;
			}
		}
		return false;
	}

	// 基类型已确认为记录类型后的成员名校核（`.` 直接用、`->` 由调用方先解引用）。
	void check_member_exists(const CTypePtr &rec, const std::string &field, const SourceLoc &loc)
	{
		if (!rec || field.empty() || rec->tag.empty()) return; // 无 tag 的匿名内联记录：无可校核
		if (rec->kind == TypeKind::Class)
		{
			auto cit = classes_.find(rec->tag);
			if (cit == classes_.end() || !cit->second) return; // 类定义缺失：另有路径报
			for (const auto &m : cit->second->members)
				if (!m.name.empty() && m.name == field) return; // 含 static 成员
			for (const auto &m : cit->second->methods)
				if (m.name == field) return;                    // 方法访问（obj.method / obj.method()）
			report(loc, "no member named '" + field + "' in " + spell_record(rec));
			return;
		}
		const StructDef *sd = record_def_of(rec->tag);
		if (!sd || !sd->is_definition)
		{
			// 只有前置声明、无 {} 定义：clang 同文诊断 + 前置声明 note。
			const std::string sp = spell_record(rec);
			if (sd) report_with_note(loc, "incomplete definition of type " + sp, sd->loc, "forward declaration of " + sp);
			return;
		}
		if (struct_has_member(sd, field, 0)) return;
		report(loc, "no member named '" + field + "' in " + spell_record(rec));
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

	// P304：该 typedef 名是否属于指定模块（不看 public）——用于把「模块里确实有这个类型、
	//   但没导出」与「根本没这个名字」分成两条诊断。
	bool typedef_owned_by(const std::string &name, const std::string &module_key) const
	{
		auto it = typedef_owner_.find(name);
		return it != typedef_owner_.end() && it->second.count(module_key) > 0;
	}

	// P217：主源将导入的非 public struct/enum 当类型用 → 拒收。仅主源（cur_source_module_key_ 空）触发。
	// P302：`alias.Tag` 的模块归属校验与类别校正。
	//   非限定名返回 false（交常规路径）；限定名一律返回 true（已处理，不再叠裸名判定）。
	bool resolve_qualified_type(const CTypePtr &t, const SourceLoc &loc)
	{
		const auto dot = t->tag.find('.');
		if (dot == std::string::npos) return false;
		const std::string alias = t->tag.substr(0, dot);
		const std::string nm = t->tag.substr(dot + 1);
		auto qit = catalog_.qualified.find(alias);
		if (qit == catalog_.qualified.end())
		{
			// 非导入别名：点号只是 parser 的块内改名（`X.__blkN`）——不当限定名处理，
			//   交回常规 tag 路径；仅当第二段确实是已登记标签/typedef 时（如写错模块名 `zz.Leaf`）
			//   才按「未知模块别名」拒收，避免拼写错静默成未知类型。
			if (structs_.count(nm) || enums_.count(nm) || classes_.count(nm) || typedef_names_.count(nm))
			{
				report(loc, "unknown module alias '" + alias + "' in type name '" + t->tag + "'");
				return true;
			}
			return false;
		}
		auto vit = catalog_.by_module.find(qit->second);
		if (vit == catalog_.by_module.end()) return true;
		const ModuleCatalog::View &v = vit->second;
		auto sit = v.structs.find(nm);
		if (sit != v.structs.end())
		{
			t->kind = (sit->second->is_union ? TypeKind::Union : TypeKind::Struct);
			return true;
		}
		if (v.enums.count(nm)) { t->kind = TypeKind::Enum; return true; }
		if (v.classes.count(nm)) { t->kind = TypeKind::Class; return true; }
		// 裁定⑥口径：非 public / 不存在均不入导出面 → 限定名不可命中
		report(loc, "no public type '" + nm + "' in module '" + alias + "'");
		return true;
	}

	// P302：宿主本 TU 是否自有该标签的**定义**（导入项与前置引用空壳都不算）。
	bool host_defines_type(const std::string &tag) const
	{
		if (!main_tu_) return false;
		for (const auto &s : main_tu_->structs)
			if (s.tag == tag && s.source_module_key.empty() && s.is_definition) return true;
		for (const auto &e : main_tu_->enums)
			if (e.tag == tag && e.source_module_key.empty()) return true;
		for (const auto &c : main_tu_->classes)
			if (c.tag == tag && c.source_module_key.empty()) return true;
		return false;
	}

	// P302：裸名引用「被多个模块导出的同名标签」→ 报错并要求限定名。
	//   宿主自定义同名类型时按宿主胜出豁免（与函数/全局的裁定②同口径）。
	//   注意：豁免须看「宿主的真定义」——parser 对主源引用 `Leaf x;` 会先造一条前置引用项
	//   （is_definition=false、key 空），若用 host_own_names_ 判定会把这类空壳当自有点而漏报。
	//   返回 true 表示已报错（调用方跳过后续可见性检查，避免叠加误报）。
	bool reject_ambiguous_bare_type(const std::string &tag, const SourceLoc &loc)
	{
		if (!per_module_) return false;
		if (!cur_source_module_key_.empty()) return false; // 仅宿主裸名；子模块内部自有绑定
		if (host_defines_type(tag)) return false;
		auto own = xmod_type_owners_.find(tag);
		if (own == xmod_type_owners_.end() || own->second.size() < 2) return false;
		std::vector<std::string> names;
		for (const auto &k : own->second) names.push_back(alias_for_key(main_tu_, k) + "." + tag);
		std::sort(names.begin(), names.end());
		std::string list;
		for (const auto &n : names) list += (list.empty() ? "" : ", ") + n;
		report(loc, "reference to '" + tag + "' is ambiguous (defined in multiple imported modules: " +
			list + "); use a qualified name");
		return true;
	}

	//   沿 Pointer/Array 的 base 逐层剥、Function 遍历返回类型与参数，得基类型后仅对 Struct/Union/Enum
	//   且非空 tag 查表；命中且 source_module_key 非空（导入类型）才调 ensure_visible_from_main。
	//   注：枚举常量名在 parse 期折成整数字面量、非类型引用，故门禁只作用于 enum 类型标签。
	void enforce_type_visible(const CTypePtr &t, const SourceLoc &loc)
	{
		if (!cur_source_module_key_.empty()) return; // 仅主源；子模块内部互引放行
		if (!t) return;
		// P302：标签位的跨模块归属裁定（限定名校验/校正；裸名多模块同名则拒）。
		//   沿指针/数组递归时由 base 再次进入，故此处只需处理自带标签的层级。
		if (!t->tag.empty())
		{
			if (resolve_qualified_type(t, loc)) return;
			if (reject_ambiguous_bare_type(t->tag, loc)) return;
		}
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
			// P344：但宿主自有同名定义时，该引用恒绑宿主自身（裁定②遮蔽），不存在跨模块可见性问题；
			//   修前首匹配会取到模块的同名定义，把宿主自引用误报成「不可导入非 public / 未从模块导入」。
			if (host_defines_type(t->tag)) return;
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
			report(loc, "class handle requires a handle value, got incompatible type " + describe_type(st));
		else if (!want.empty() && !st->tag.empty() && st->tag != want)
			report(loc, "cannot bind handle of class '" + st->tag + "' to class handle '" + want + "'");
	}

	// P255：对「仅有前向声明、无 {} 定义」的 struct/union 作按值分配（局部/全局/字段/数组元素）
	//   作前置诊断，把 LLVM 校验器的 `Cannot allocate unsized type` 挡在前面、报 clang 同款主错误。
	//   剥层规则：沿 Array 下探（记 saw_array），遇 Pointer/Function 立即停（不完整类型经指针合法），
	//   落到 Struct/Union 且 tag 非空、find_struct_def_by_tag 为空 ⇒ 不完整、报错。Enum 不纳入（越界）。
	//   P301：主源与子模块同口径。此前仅主源触发（cur_source_module_key_ 空），子模块里按值用到
	//   不完整类型时会一路漏给 codegen，opaque %struct. 作 alloca 使 LLVM 直接崩溃（ADR 见 PROJECT.md）。
	enum IncompleteCtx { IctxVar, IctxField, IctxGlobal };

	void enforce_complete_type(const CTypePtr &t, const SourceLoc &loc, IncompleteCtx ctx)
	{
		const CTypePtr *node = &t; // 逐层指向当前类型（base 为其成员，AST 存活整趟 sema）
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

	// P329：下标基指向**不完整记录**（C 6.5.2.1 要求指向完整对象类型；clang 同文拒收
	//   `subscript of pointer to incomplete type 'struct I'`）。判据与 P255 的
	//   enforce_complete_type **同源**（find_struct_def_by_tag 落空即不完整）：
	//   · 只收 Struct/Union——clang 接受 `enum E; enum E *ep; ep[1];`（仅 MS 扩展 warning）；
	//   · 匿名内联记录必是完整定义，tag 为空者按完整处理；
	//   · `void *` / 标量指针不入判（前者 P330 已定口径：按 1 字节步进放行）。
	//   返回该记录型本身，供 describe_type 拼 `'struct I'`（typedef 形自带 aka 双轨）。
	CTypePtr incomplete_subscript_base(const CTypePtr &t) const
	{
		if (!t || t->kind != TypeKind::Pointer || !t->base) return nullptr;
		const CTypePtr &b = t->base;
		if (b->kind != TypeKind::Struct && b->kind != TypeKind::Union) return nullptr;
		if (b->tag.empty()) return nullptr;
		if (find_struct_def_by_tag(b->tag)) return nullptr; // 有 {} 定义 ⇒ 完整
		return b;
	}

	// P333：`sizeof` / `_Alignof` 的操作数是**不完整记录**（C 6.5.3.4 要求操作数类型完整；clang 同文
	//   拒收 `invalid application of 'sizeof' to an incomplete type 'struct I'`）。判据与 P255/P329
	//   **同源**（find_struct_def_by_tag 落空即不完整）：只收 Struct/Union——Enum 前置声明必放行
	//   （`enum E; enum E *ep; sizeof(*ep)` clang cc=0，仅 MS 扩展 warning）；匿名内联记录按完整。
	CTypePtr incomplete_record_type(const CTypePtr &t) const
	{
		if (!t) return nullptr;
		if (t->kind != TypeKind::Struct && t->kind != TypeKind::Union) return nullptr;
		if (t->tag.empty()) return nullptr;
		if (find_struct_def_by_tag(t->tag)) return nullptr; // 有 {} 定义 ⇒ 完整
		return t;
	}

	// P333：定位两轨（探针 p333a~ag 逐形实测的 clang 口径）——类型形在 `sizeof` 关键字列（`loc`），
	//   表达式形在**关键字之后的第一个 token**（`op_loc`，parser 的 sizeof 分支填写：括号形即 `(`、
	//   无括号形即操作数首 token）。已被更早诊断命中者其操作数型已毒化/不可解析，故不叠报。
	void check_sizeof(const ExprPtr &e)
	{
		if (e->op != "sizeof" && e->op != "alignof") return;
		const CTypePtr t = e->type ? e->type : resolve_type(e->lhs);
		const CTypePtr inc = incomplete_record_type(t);
		if (!inc) return;
		report(e->type ? e->loc : e->op_loc,
		       "invalid application of '" + e->op + "' to an incomplete type " + describe_type(inc));
	}

	// 文件作用域初始化式专用的**只查 sizeof/_Alignof 操作数完整性**递归（理由同 check_ptr_diff_chain：
	//   全局初值不走 check_expr，而该形可藏在 Cast 壳、聚合元素位与嵌套表达式里）。
	void check_sizeof_chain(const ExprPtr &e, int depth)
	{
		if (!e || depth > 16) return;
		if (e->kind == ExprKind::Unary) check_sizeof(e);
		check_sizeof_chain(e->lhs, depth + 1);
		check_sizeof_chain(e->rhs, depth + 1);
		for (const auto &x : e->args) check_sizeof_chain(x, depth + 1);
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
			// P344：按当前代码归属取记录定义（模块内自引用不得被宿主同名项顶替）。
			const StructDef *sd = self->record_def_of(tag);
			if (!sd) return nullptr;
			for (const auto &m : sd->members)
				if (!m.name.empty() && m.name == field) return m.type;
			for (const auto &m : sd->members)
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
		// P320：函数名 → 完整 Function 型（供共享解析器的 `&函数名` 取真实原型，
		//   不再退化成 `void *`）。funcs_ 已按「定义优先」归并同名的原型/定义两条记录。
		CTypePtr func_type(const std::string &n)
		{
			auto it = self->funcs_.find(n);
			if (it == self->funcs_.end() || !it->second) return nullptr;
			std::vector<CTypePtr> ps;
			for (const auto &p : it->second->params) ps.push_back(p.type);
			return make_func_type(it->second->return_type, std::move(ps), it->second->is_variadic);
		}
		// 目标数据模型的 long 位宽（供共享解析器整型提升取正确宽度）。
		unsigned long_bits() const { return static_cast<unsigned>(self->opts_.target.sizeof_long) * 8u; }
	};

	CTypePtr resolve_type(const ExprPtr &e)
	{
		// P329：表达式里嵌着「指向不完整记录的指针下标」时，其结果型按「未知」返回——
		//   check_subscript 已按 clang 报出，外层壳（取址 `&`、运算、实参、成员、逗号…）
		//   不得再叠一句异族诊断（口径同 P323 对 `Pointer{Function}` 的 poison，但那里的
		//   判据是纯结构性的、能放在共享模板里；本判据依赖符号表，故只留在 sema 包装层）。
		if (has_incomplete_poison(e, 0)) return nullptr;
		SemaTypeEnv env{this};
		return resolve_expr_type(e, env);
	}

	// 上述 poison 的判据：递归找本层已报过的两类不完整记录面——
	//   ① P329：`[]` 且任一侧是指向不完整记录的指针；
	//   ② P332：`+` 任一侧 / `-` 左侧 / `++`/`--` / `+=`/`-=` 的操作数。
	//   只下探能把子式类型**原样（或包一层）传出去**的壳；标量/字面量/成员名等一律不探。
	//   命中即意味着程序已拒收，不再区分外层用在哪里（全式定型当作不可知）。
	bool has_incomplete_poison(const ExprPtr &e, int depth)
	{
		if (!e || depth > 8) return false;
		if (e->kind == ExprKind::Binary)
		{
			const bool lhs_bad = incomplete_subscript_base(resolve_type(e->lhs)) != nullptr;
			if ((e->op == "[]" || e->op == "+") &&
				(lhs_bad || incomplete_subscript_base(resolve_type(e->rhs))))
				return true;
			if (e->op == "-" && lhs_bad) return true;
		}
		if ((e->kind == ExprKind::Unary && (e->op == "++" || e->op == "--")) ||
			(e->kind == ExprKind::Assign && (e->op == "+=" || e->op == "-=")))
		{
			if (incomplete_subscript_base(resolve_type(e->lhs))) return true;
		}
		switch (e->kind)
		{
		case ExprKind::Unary:
		case ExprKind::Member:
		case ExprKind::Assign:
		case ExprKind::Cast:
			return has_incomplete_poison(e->lhs, depth + 1);
		case ExprKind::Binary:
		case ExprKind::Comma:
			return has_incomplete_poison(e->lhs, depth + 1) ||
			       has_incomplete_poison(e->rhs, depth + 1);
		case ExprKind::Call:
			if (e->lhs && has_incomplete_poison(e->lhs, depth + 1)) return true;
			for (const auto &x : e->args)
				if (has_incomplete_poison(x, depth + 1)) return true;
			return false;
		case ExprKind::Ternary:
			for (const auto &x : e->args)
				if (has_incomplete_poison(x, depth + 1)) return true;
			return false;
		default:
			return false;
		}
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
		const StructDef *sd = record_def_of(obj->tag); // P344：按归属取定义
		if (!sd) return {};
		for (const auto &m : sd->members)
			if (m.name == field) return m.loc;
		return {};
	}

	// 递归查找聚合类型（含嵌套 struct/union）中 const 成员的声明位置（field-unsafe note 用）
	SourceLoc find_const_member_rec_loc(const CTypePtr &t, int depth)
	{
		if (!t || depth > 4) return {};
		if (t->kind != TypeKind::Struct && t->kind != TypeKind::Union) return {};
		const StructDef *sd = record_def_of(t->tag); // P344：按归属取定义
		if (!sd) return {};
		for (const auto &m : sd->members)
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
		const StructDef *sd = record_def_of(t->tag); // P344：按归属取定义
		if (!sd) return false;
		for (const auto &m : sd->members)
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
		// P303：枚举常量 `alias.RED`——枚举常量在本模块内是整型常量（parser 即折成字面量），
		//   跨模块限定访问按同值就地折成 IntLiteral，与裸 `RED` 同语义；不折则落到下面的
		//   "no public symbol" 拒收（枚举常量不属于 globals/funcs 两类导出面）。
		for (const auto &ekv : view.enums)
		{
			const EnumDef *ed = ekv.second;
			if (!ed) continue;
			for (const auto &ec : ed->constants)
			{
				if (ec.name != mname) continue;
				e->kind = ExprKind::IntLiteral;
				e->int_val = ec.value;
				e->type = nullptr;
				base->module_qualifier = true;
				return true;
			}
		}
		// 裁定⑥：非 public 不入导出面 → 限定访问不可命中（与按名 import 非 public 同口径）。
		// P302：若该名在被导入模块里是类型，改用类型口径的诊断（否则 type-only 名字会报成 'symbol'）。
		{
			auto sit = structs_.find(mname);
			auto eit = enums_.find(mname);
			auto cit = classes_.find(mname);
			const bool same_mod = (sit != structs_.end() && sit->second && sit->second->source_module_key == qit->second) ||
			                      (eit != enums_.end() && eit->second && eit->second->source_module_key == qit->second) ||
			                      (cit != classes_.end() && cit->second && cit->second->source_module_key == qit->second) ||
			                      typedef_owned_by(mname, qit->second);
			if (same_mod)
			{
				report(e->loc, "no public type '" + mname + "' in module '" + alias + "'");
				base->module_qualifier = true;
				return true;
			}
		}
		report(e->loc, "no public symbol '" + mname + "' in module '" + alias + "'");
		base->module_qualifier = true;
		return true;
	}

	// P303：`Class::name` 的名字校核——缺此判定时未命中一律静默漏给 codegen
	//   （class_static_ref 未命中返回 nullptr，会落到成员 GEP 的兜底路径，产出坏 IR）。
	//   放行：static 数据成员、同名方法（静态/实例）。拒收：非 static 数据成员、无此名。
	//   限定类名 `alias.Tag` 在 classes_ 中已有别名键，故此处按 base->tag 直查即可。
	void check_class_static_scope(const std::string &tag, const std::string &mname, const SourceLoc &loc)
	{
		auto cit = classes_.find(tag);
		if (cit == classes_.end() || !cit->second) return; // 别名键/定义缺失已由其他路径报
		bool has_data = false, has_static = false, has_method = false;
		for (const auto &m : cit->second->members)
		{
			if (m.name.empty() || m.name != mname) continue;
			has_data = true;
			if (m.is_static) has_static = true;
		}
		if (!has_data)
			for (const auto &m : cit->second->methods)
				if (m.name == mname) { has_method = true; break; }
		if (has_static || has_method) return;
		if (has_data) report(loc, "invalid use of non-static data member '" + mname + "'");
		else report(loc, "no member named '" + mname + "' in '" + tag + "'");
	}

	// P303：`alias.Tag::member` 中 `alias.Tag` 解析不出类时的**类型口径**诊断。
	//   返回 true 表示已按限定名处理（已报错或已确认模块归属），调用方不再叠通用诊断；
	//   false 交回通用诊断（非限定名/别名未知）。成功后标记 module_qualifier，使递归子
	//   表达式的 undeclared 检查不叠加第二条错。
	bool report_bad_qualified_class(const ExprPtr &ref)
	{
		if (!per_module_ || !ref) return false;
		const auto dot = ref->name.find('.');
		if (dot == std::string::npos) return false;
		const std::string alias = ref->name.substr(0, dot);
		const std::string nm = ref->name.substr(dot + 1);
		auto qit = catalog_.qualified.find(alias);
		if (qit == catalog_.qualified.end()) return false;
		ref->module_qualifier = true;
		auto vit = catalog_.by_module.find(qit->second);
		if (vit == catalog_.by_module.end()) return true;
		const ModuleCatalog::View &v = vit->second;
		if (v.classes.count(nm)) return true; // 已导出为类（解析失败属上层问题，静默）
		if (v.structs.count(nm) || v.enums.count(nm))
			report(ref->loc, "no public class '" + nm + "' in module '" + alias + "'");
		else
			report(ref->loc, "no public type '" + nm + "' in module '" + alias + "'");
		return true;
	}

	// —— P317：指针差的类型相容（C 6.5.9p4）——
	// 两侧先按 C 6.3.2.1 衰减（数组名→元素指针、函数名→函数指针），均为指针才入判据；
	//   文案两侧取**衰减后**的指针拼写（`sa - arr` → `'short *' and 'int *'`），
	//   typedef 侧由 describe_type 自带 aka 双轨（探针 w9.c：`'SH *' (aka 'short *') and 'int *'`）。
	// 定在**运算符** loc（`e->op_loc`）：P316 的常量性文案定在表达式起点，两面各归其位。
	// 只收 `-`：`sa + arr` clang 走 `invalid operands to binary expression` 另族（且打印**未衰减**
	//   的数组型）；`sa < arr` 等比较 clang **仅 warning**（cc=0，探针 w4i.c）→ 收了即比 clang 严。
	// 返回 true 表示已报（P332 的算术句据此让位：clang 对两个指针的 `-` 只报相容性句）。
	bool check_ptr_diff(const ExprPtr &e)
	{
		const CTypePtr a = compat_decay(resolve_type(e->lhs));
		const CTypePtr b = compat_decay(resolve_type(e->rhs));
		if (!a || !b) return false;
		if (a->kind != TypeKind::Pointer || b->kind != TypeKind::Pointer) return false;
		if (pd_pointee(a->base, b->base, 0)) return false;
		report(e->op_loc, describe_type(a) + " and " + describe_type(b) +
			" are not pointers to compatible types");
		return true;
	}

	// —— P332：不完整记录指针的算术 ——
	// C 6.5.6p2/p3 要求算术所用指针指向**完整对象类型**；clang 同文拒收
	//   `arithmetic on a pointer to an incomplete type 'struct I'`。判据与 P255/P329/P333
	//   **同源**（复用 incomplete_subscript_base：Struct/Union + tag 非空 + 查不到定义；
	//   Enum 必放行、`void *` 按 P330 口径放行、匿名内联记录按完整）。形状覆盖（p332a~ar 实测）：
	//   `+` **任一侧**（`1 + ip` 同报）、`-` **只判左侧**（`1 - ip` 走 invalid-operands 另族，
	//   本批不建模）、`++`/`--`、`+=`/`-=`。定位：Binary 用 `op_loc`（运算符列），
	//   Unary/Assign 用 `loc`（parser 在这两处存的正是运算符起点，探针 d/l/m/e/n 逐列相符）。
	void check_ptr_arith(const ExprPtr &e)
	{
		CTypePtr inc;
		SourceLoc loc;
		if (e->kind == ExprKind::Binary)
		{
			if (e->op == "+")
			{
				inc = incomplete_subscript_base(resolve_type(e->lhs));
				if (!inc) inc = incomplete_subscript_base(resolve_type(e->rhs));
			}
			else if (e->op == "-")
				inc = incomplete_subscript_base(resolve_type(e->lhs));
			else
				return;
			loc = e->op_loc;
		}
		else if (e->kind == ExprKind::Unary)
		{
			if (e->op != "++" && e->op != "--") return;
			inc = incomplete_subscript_base(resolve_type(e->lhs));
			loc = e->loc;
		}
		else if (e->kind == ExprKind::Assign)
		{
			if (e->op != "+=" && e->op != "-=") return;
			inc = incomplete_subscript_base(resolve_type(e->lhs));
			loc = e->loc;
		}
		else
			return;
		if (!inc) return;
		report(loc, "arithmetic on a pointer to an incomplete type " + describe_type(inc));
	}

	// 文件作用域初始化式专用的**只查指针算术**递归（理由同 check_ptr_diff_chain：全局初值不走
	//   check_expr，而该形可藏在 Cast 壳、聚合元素位与嵌套表达式里）。
	void check_ptr_arith_chain(const ExprPtr &e, int depth)
	{
		if (!e || depth > 16) return;
		if (e->kind == ExprKind::Binary || e->kind == ExprKind::Unary || e->kind == ExprKind::Assign)
			check_ptr_arith(e);
		check_ptr_arith_chain(e->lhs, depth + 1);
		check_ptr_arith_chain(e->rhs, depth + 1);
		for (const auto &x : e->args) check_ptr_arith_chain(x, depth + 1);
	}

	// 文件作用域初始化式专用的**只查指针差**递归。不复用 `check_expr`：它带整套表达式诊断，
	//   接入全局初值面会一次性改动词表（与本批「单面收敛」纪律不符，且极易新增误拒）。
	//   上段的 assign 循环只看顶层表达式、且对 InitList/数组目标整条跳过，而指针差是
	//   **表达式内部**的性质（`long j = (long)(sa - arr);`、`{ sa - arr, 0 }`），故另开一条。
	void check_ptr_diff_chain(const ExprPtr &e, int depth)
	{
		if (!e || depth > 16) return;
		if (e->kind == ExprKind::Binary && e->op == "-") check_ptr_diff(e);
		check_ptr_diff_chain(e->lhs, depth + 1);
		check_ptr_diff_chain(e->rhs, depth + 1);
		for (const auto &x : e->args) check_ptr_diff_chain(x, depth + 1);
	}

	// —— P323：下标基型（check_expr 与文件域链共用同一处判据）——
	// 任一侧可下标即放行（C 6.5.2.1 只要求其一；`arr[f]` 的另一侧由既有判据各归其位）。
	// 有函数型基则报 clang 那句（印未 decay 的原型、定在基址起点）；否则报 clang 的
	//   `subscripted value is not an array, pointer, or vector` 并定在**该层 `[` 的列**
	//   （P328：`op_loc` 由 parser 的后缀 `[` 分支填写；函数句仍用 `loc`，P323 已与 clang
	//   同行同列、不得改）。
	void check_subscript(const ExprPtr &e)
	{
		const CTypePtr a = resolve_type(e->lhs);
		const CTypePtr b = resolve_type(e->rhs);
		if (!a || !b) return;
		if (is_subscriptable(a) || is_subscriptable(b))
		{
			// P329：指向不完整记录的指针下标（clang 同句、定在该层 `[` 的列，P328 口径）。
			//   结果型已由 resolve_type 毒化，故此处不会与下游异族句叠报。
			CTypePtr ib = incomplete_subscript_base(a);
			if (!ib) ib = incomplete_subscript_base(b);
			if (ib) report(e->op_loc, "subscript of pointer to incomplete type " + describe_type(ib));
			return;
		}
		const CTypePtr fa = subscript_fn_type(a);
		const CTypePtr fn = fa ? fa : subscript_fn_type(b);
		if (fn)
			report(e->loc, "subscript of pointer to function type " + describe_type(fn));
		else
			report(e->op_loc, "subscripted value is not an array, pointer, or vector");
	}

	// 文件作用域初始化式专用的**只查下标基型**递归（理由同 check_ptr_diff_chain：全局初值不走
	//   check_expr，而 `int v = f[1];` 这类下标藏在表达式内部——含聚合元素位、Cast 壳、嵌套）。
	void check_subscript_chain(const ExprPtr &e, int depth)
	{
		if (!e || depth > 16) return;
		if (e->kind == ExprKind::Binary && e->op == "[]") check_subscript(e);
		check_subscript_chain(e->lhs, depth + 1);
		check_subscript_chain(e->rhs, depth + 1);
		for (const auto &x : e->args) check_subscript_chain(x, depth + 1);
	}

	// P331：`void` 不是对象类型，不能参与任何双目运算——clang 一律
	//   `invalid operands to binary expression ('void' and 'int')`（两侧按**书写顺序**、未衰减拼写；
	//   两侧都是 void 时印 `('void' and 'void')`）。定位：Binary 用 `op_loc`（运算符列）；复合赋值
	//   （`vp[1] += 1` / `x += vp[1]`）clang 同句、定在运算符起点（parser 的 Assign 存的正是它）。
	//   两侧型都解得出才判（宁漏不错：另一侧未知时不报）。
	bool check_void_binary(const ExprPtr &e)
	{
		if (!e || !e->lhs || !e->rhs) return false;
		const CTypePtr lt = resolve_type(e->lhs);
		const CTypePtr rt = resolve_type(e->rhs);
		if (!lt || !rt) return false;
		const bool lv = lt->kind == TypeKind::Void;
		const bool rv = rt->kind == TypeKind::Void;
		if (!lv && !rv) return false;
		report(e->kind == ExprKind::Assign ? e->loc : e->op_loc,
		       "invalid operands to binary expression (" + describe_type(lt) + " and " + describe_type(rt) + ")");
		return true;
	}

	// P334：指针操作数在结构非法位置——clang
	//   `invalid operands to binary expression ('X' and 'Y')`。
	//   规则（C 6.5.6/6.5.5/6.5.10）：
	//     + : ptr + ptr 非法
	//     - : int - ptr 非法
	//     * / % & | ^ << >> : 任一侧指针非法
	//   排除：比较/逻辑/下标/逗号（各走自己的族）；含 void（P331 先手）。
	//   定位：Binary 用 op_loc、Assign 用 loc（与 P331 同口径）。
	//   优先级：P334 > P332 > P317（结构非法即短路，不再查完整性/兼容性）。
	static bool is_ptr_like(const CTypePtr &t)
	{
		return t && (t->kind == TypeKind::Pointer || t->kind == TypeKind::Array ||
					 t->kind == TypeKind::Function);
	}

	bool check_binary_operands(const ExprPtr &e)
	{
		if (!e || !e->lhs || !e->rhs) return false;
		const std::string &op = e->op;
		// 排除：比较/逻辑/下标/逗号
		if (op == "[]" || op == "," || op == "&&" || op == "||" ||
			op == "==" || op == "!=" || op == "<" || op == ">" ||
			op == "<=" || op == ">=")
			return false;
		const CTypePtr lt = resolve_type(e->lhs);
		const CTypePtr rt = resolve_type(e->rhs);
		if (!lt || !rt) return false;
		// void 侧交 P331 先手
		if (lt->kind == TypeKind::Void || rt->kind == TypeKind::Void) return false;
		const bool lp = is_ptr_like(lt);
		const bool rp = is_ptr_like(rt);
		bool fire = false;
		if (op == "+")
		{
			if (lp && rp) fire = true;
		}
		else if (op == "-")
		{
			if (!lp && rp) fire = true;
		}
		else if (op == "*" || op == "/" || op == "%" ||
				 op == "&" || op == "|" || op == "^" ||
				 op == "<<" || op == ">>")
		{
			if (lp || rp) fire = true;
		}
		// 复合赋值（Assign 节点 op 形如 "+=" "-=" "*=" 等）
		if (!fire && e->kind == ExprKind::Assign && op.size() > 1 && op != "=")
		{
			char c0 = op[0];
			if (c0 == '-' && !lp && rp) fire = true;
			else if (c0 == '+' && lp && rp) fire = true;
			else if ((c0 == '*' || c0 == '/' || c0 == '%' ||
					  c0 == '&' || c0 == '|' || c0 == '^' ||
					  c0 == '<' || c0 == '>') && (lp || rp))
				fire = true;
		}
		if (fire)
		{
			report(e->kind == ExprKind::Assign ? e->loc : e->op_loc,
				   "invalid operands to binary expression (" +
				   describe_type(lt) + " and " + describe_type(rt) + ")");
		}
		return fire;
	}

	// P334：文件作用域初始化式专用递归链（全局初值不走 check_expr，理由同 P332/P317 链）。
	void check_binary_operands_chain(const ExprPtr &e, int depth)
	{
		if (!e || depth > 16) return;
		if (e->kind == ExprKind::Binary || e->kind == ExprKind::Assign)
			check_binary_operands(e);
		check_binary_operands_chain(e->lhs, depth + 1);
		check_binary_operands_chain(e->rhs, depth + 1);
		for (const auto &x : e->args) check_binary_operands_chain(x, depth + 1);
	}

	// P331：`++`/`--` 的操作数是 `void` 值——clang `cannot increment value of type 'void'` /
	//   `cannot decrement value of type 'void'`，定在运算符起点（parser 在 Unary 上存的 loc）。
	void check_void_incdec(const ExprPtr &e)
	{
		if (!e->lhs || (e->op != "++" && e->op != "--")) return;
		const CTypePtr t = resolve_type(e->lhs);
		if (!t || t->kind != TypeKind::Void) return;
		report(e->loc, std::string("cannot ") + (e->op == "++" ? "increment" : "decrement") + " value of type 'void'");
	}

	// P335：条件位（if/while/do-while/for）操作数为 void 值——clang
	//   `statement requires expression of scalar type ('void' invalid)`，定在语句关键字列（s->loc）。
	//   仅当条件型解得出且为 Void 才报（宁漏不错：不完整型/未知型不报）。
	void check_cond_scalar(const StmtPtr &s)
	{
		if (!s->expr) return;
		CTypePtr t = resolve_type(s->expr);
		if (t && t->kind == TypeKind::Void)
			report(s->loc, "statement requires expression of scalar type ('void' invalid)");
	}

	// P335：switch 选择位为 void 值——clang 文案用 `integer type`（区别于 scalar）。
	void check_cond_integer(const StmtPtr &s)
	{
		if (!s->expr) return;
		CTypePtr t = resolve_type(s->expr);
		if (t && t->kind == TypeKind::Void)
			report(s->loc, "statement requires expression of integer type ('void' invalid)");
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
			// P331：复合赋值的任一侧是 `void` 值时 clang 只报 invalid operands 一句
			//   （`vp[1] += 1` → ('void' and 'int')、`x += vp[1]` → ('int' and 'void')），不叠相容句。
			const bool void_bin = e->op != "=" && check_void_binary(e);
			const bool bad_ops = !void_bin && e->op != "=" && check_binary_operands(e);
			// C23 模型 B：nullptr（nullptr_t）只能赋给指针类型；赋给类型已知的非指针左值是约束违例。
			if (!void_bin && !bad_ops && L && is_nullptr(e->rhs))
			{
				CTypePtr lt = resolve_type(L);
				if (lt && lt->kind != TypeKind::Pointer)
					report(e->loc, "assigning to " + describe_type(lt) + " from incompatible type 'nullptr_t'");
			}
			// 0.4.1：一般赋值的类型相容（nullptr 由上块专属处理、命中不重复；未知类型与另族形态放行）
			else if (!void_bin && !bad_ops && L && e->rhs && (!is_ternary_src(e->rhs) || ternary_has_void_arm(e->rhs)))
			{
				const CTypePtr lt = resolve_type(L);
				const CTypePtr rt = resolve_type(e->rhs);
				// P331：目标左值是 `void` 对象（`vp[1] = …`）——clang `incomplete type 'void' is not
				//   assignable`，定在赋值运算符；`void` 左值没有对象可写，故不再叠相容句。
				if (lt && lt->kind == TypeKind::Void)
					report(e->loc, "incomplete type 'void' is not assignable");
				else
				{
					const Compat cc = assign_compat(rt, lt, e->rhs, e->op != "=");
					if (cc != Compat::Ok && cc != Compat::Skip)
						report(e->loc, compat_assign_msg(lt, rt, cc));
				}
			}
			// P332：`p += n` / `p -= n` 的左侧是不完整记录指针（定位在运算符起点 `loc`）
			check_ptr_arith(e);
			break;
		}
		case ExprKind::Unary:
			// P254：++/-- 的操作数是可写位置（clang 与赋值同文，不说 "cannot increment"）
			if (e->op == "++" || e->op == "--") check_write_target(e->lhs, e->loc);
			// P302：`sizeof(T)` / `_Alignof(T)` 的类型操作数同样是类型引用点，须过跨模块裁定
			if (e->type && (e->op == "sizeof" || e->op == "alignof")) enforce_type_visible(e->type, e->loc);
			// P333：`sizeof` / `_Alignof` 施于不完整记录（clang 同文拒收，不再静默给 0）
			check_sizeof(e);
			// P332：`++` / `--` 的操作数是不完整记录指针（模式形定位在运算符起点 `loc`）
			check_ptr_arith(e);
			// P331：`++` / `--` 的操作数是 `void` 值（同上定位在运算符起点）
			check_void_incdec(e);
			// P335：`-`/`+`/`!`/`~` 作用于 void 值——clang `invalid argument type 'void' to unary expression`
			//   定位在运算符起点（e->loc）；仅当操作数型为 Void 才报，sizeof/alignof/++/-- 不属本形。
			// P340：`~` 另限整型——clang 对浮点直拒
			//   （`invalid argument type 'double' to unary expression`）；旧路静默接受并给出值。
			if (e->op == "-" || e->op == "+" || e->op == "!" || e->op == "~")
			{
				CTypePtr ut = resolve_type(e->lhs);
				if (ut && ut->kind == TypeKind::Void)
					report(e->loc, "invalid argument type 'void' to unary expression");
				// describe_type() 返回值已自带单引号（type_spell.h），此处不另补引号对。
				else if (e->op == "~" && ut &&
				         (ut->kind == TypeKind::Float || ut->kind == TypeKind::Double))
					report(e->loc, "invalid argument type " + describe_type(ut) + " to unary expression");
			}
			// P335：`*` 解引用 void 值——clang `indirection requires pointer operand ('void' invalid)`
			if (e->op == "*")
			{
				CTypePtr ut = resolve_type(e->lhs);
				if (ut && ut->kind == TypeKind::Void)
					report(e->loc, "indirection requires pointer operand ('void' invalid)");
			}
			break;
		case ExprKind::Member:
		{
			// 路 B 阶段1b-iii：限定访问 m.x / m.f() —— 命中则已绑定，不走常规成员类型检查。
			if (try_bind_qualified_member(e)) break;
			// P300 Co class：:: 静态成员/方法访问，基类名已由 var_type 解析为 Class
			if (e->op == "::")
			{
				// P302：`Tag::static` 的类名也是类型引用点；同名多模块导出 → 裸名歧义拒收
				if (e->lhs && e->lhs->kind == ExprKind::VarRef && reject_ambiguous_bare_type(e->lhs->name, e->loc)) break;
				CTypePtr base = resolve_type(e->lhs);
				if (!base || base->kind != TypeKind::Class)
				{
					// P303：限定基名 `alias.Tag` 的类型口径拒收（拼错/非 public/不是类）
					if (e->lhs && e->lhs->kind == ExprKind::VarRef && report_bad_qualified_class(e->lhs)) break;
					report(e->loc, "'::' requires a class name on the left side");
				}
				else check_class_static_scope(base->tag, e->name, e->loc);
				break;
			}
			CTypePtr base = resolve_type(e->lhs);
			if (base)
			{
				if (e->op == "." && !is_struct_like(base))
					report(e->loc, "member reference base type " + describe_type(base) + " is not a structure or union");
				else if (e->op == "->" && !is_ptr_or_array(base))
					report(e->loc, std::string("member reference type ") + describe_type(base) + " is not a pointer" + (is_struct_like(base) ? "; did you mean to use '.'?" : ""));
				else if (e->op == ".")
					check_member_exists(base, e->name, e->loc); // P305：成员名存在性
				else
				{
					// P305：`->` 先解一层再校核——解引用基不是记录时 clang 走「基类型非结构体」同文
					//   （`void *v; v->x` / `int *p; p->x` 修前静默过）。
					const CTypePtr rec = base->base;
					if (!rec) { /* 未知 pointee：不报，交其他检查 */ }
					else if (is_struct_like(rec)) check_member_exists(rec, e->name, e->loc);
					else report(e->loc, "member reference base type " + describe_type(rec) + " is not a structure or union");
				}
			}
			break;
		}
		case ExprKind::Binary:
		{
			if (e->op == "[]")
			{
				// P323：函数型基（`f[1]`/`fp[1]`）走专门文案；标量族文案仍是通用句（P328 待改）
				check_subscript(e);
				break;
			}
			// P331：`void` 操作数先手——clang 对含 `void` 的二元式只报 invalid operands 一句，
			//   连指针族的两句（P317 相容 / P332 算术）都不叠。
			if (check_void_binary(e)) break;
			if (check_binary_operands(e)) break; // P334：结构非法短路，不再查 P332/P317
			if (e->op == "-")
			{
				// P317：异 pointee 类型的指针差（文案与定位见 check_ptr_diff）——优先于 P332：
				//   两侧都是指针且不相容时 clang 只报相容句（探针 p332al/am）。
				if (!check_ptr_diff(e)) check_ptr_arith(e);
			}
			else if (e->op == "+")
			{
				check_ptr_arith(e); // P332：任一指针侧指向不完整记录
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
					// 0.4.1：实参类型相容。**不带 body 门禁**——extern 原型（如 winapi.co）同样要查；
					//   个数超形参表时只比前 np 个（少/多参由上段 arity 检查负责），变参尾部不查。
					//   CTS 全量逐条对 clang 同源核对后剩下的五类噪声一律放行（“零误伤”优先）：
					//   ① 函数名被局部变量/形参遮蔽（`void test(void (*test)()) { test(10,a,b); }`）→ 实为间接调用；
					//   ② 形参是函数指针族（`T (*)()` 无原型形与函数指针相容性另族句式）；
					//   ③ 形参是 union/GCC `transparent_union`（未建模该属性，clang 允许整型直传）；
					//   ④ 实参是三元式（分支指针不同容时 clang 结果类型为 void*，colang 未建模）；
					//   ⑤ 实参类型是数组（老式 `f()` 无原型声明下 clang 不做原型比较）。
					if (!f->params.empty() && !is_declared_local(e->name))
					{
						const std::size_t np = f->params.size();
						const std::size_t nb = e->args.size() < np ? e->args.size() : np;
						for (std::size_t i = 0; i < nb; ++i)
						{
							const ExprPtr &ag = e->args[i];
							if (!ag || (is_ternary_src(ag) && !ternary_has_void_arm(ag))) continue;
							const CTypePtr rt = resolve_type(ag);
							// P322：数组源不再提前放行——compat_decay 退化成元素指针后，`g(int *p)` 传数组
							//   仍判 Ok（u10g2），而 `g(long v)` 传数组要求报 int-conversion（clang 13:8）。
							if (!rt) continue;
							const CTypePtr pt = f->params[i].type;
							if (!pt) continue;
							if (pt->kind == TypeKind::Union || pt->kind == TypeKind::Function) continue;
							if (pt->kind == TypeKind::Pointer && pt->base &&
							    (pt->base->kind == TypeKind::Function || pt->base->kind == TypeKind::Union))
								continue;
							const Compat cc = assign_compat(rt, pt, ag);
							if (cc == Compat::Ok || cc == Compat::Skip) continue;
							const std::string msg = compat_arg_msg(pt, rt, cc);
							if (f->params[i].loc.line)
							{
								const std::string nmsg = f->params[i].name.empty()
									? std::string("passing argument to parameter here")
									: "passing argument to parameter '" + f->params[i].name + "' here";
								report_with_note(ag->loc, msg, f->params[i].loc, nmsg);
							}
							else
								report(ag->loc, msg);
						}
					}
				}
			}
			break;
		}
		case ExprKind::Cast:
			enforce_type_visible(e->type, e->loc); // P217：转换/复合字面量目标类型跨模块可见性
			// P335：cast 源为 void 值且目标非 void——clang `operand of type 'void' where arithmetic or pointer type is required`
			//   定位在源表达式起点（e->lhs->loc）。`(void)x` 是合法的显式丢弃，不报。
			{
				if (!e->type || e->type->kind != TypeKind::Void)
				{
					CTypePtr src_t = resolve_type(e->lhs);
					if (src_t && src_t->kind == TypeKind::Void)
						report(e->lhs->loc, "operand of type 'void' where arithmetic or pointer type is required");
				}
			}
			break;
		// P300 Co class（0.4.0）
		case ExprKind::New:
		{
			// P302：`new Tag()` 的类名同属类型引用点——同名多模块导出时裸名歧义拒收（须 `new mod.Tag()`）
			if (reject_ambiguous_bare_type(e->name, e->loc)) break;
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
		case ExprKind::Generic:
		{
			// 0.4.1：C11 6.5.1.1 —— 关联列表内同一类型不得重复出现（default 不受限）
			for (std::size_t i = 0; i < e->args.size(); ++i)
			{
				const ExprPtr &ai = e->args[i];
				if (!ai || !ai->type) continue;
				for (std::size_t j = 0; j < i; ++j)
				{
					const ExprPtr &aj = e->args[j];
					if (!aj || !aj->type) continue;
					if (has_char_leaf(ai->type) || has_char_leaf(aj->type)) continue;
					if (!same_type_strict(ai->type, aj->type)) continue;
					const std::string ta = describe_type(ai->type);
					const std::string tj = describe_type(aj->type);
					const std::string msg = "type " + ta +
						" in generic association compatible with previously specified type " + tj;
					if (ai->loc.line && aj->loc.line)
						report_with_note(ai->loc, msg, aj->loc, "compatible type " + tj + " specified here");
					else
						report(ai->loc, msg);
					break;
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
				report(s->var_init->loc, "initializing " + describe_type(s->var_type) + " with an expression of incompatible type 'nullptr_t'");
			// 0.4.1：一般初始化的类型相容（聚合初始化与数组目标交既有机器件；nullptr 已专属处理）
			else if (s->var_init && s->var_init->kind != ExprKind::InitList && (!is_ternary_src(s->var_init) || ternary_has_void_arm(s->var_init)) &&
			         s->var_type && s->var_type->kind != TypeKind::Array)
			{
				const CTypePtr rt = resolve_type(s->var_init);
				const Compat cc = assign_compat(rt, s->var_type, s->var_init);
				if (cc != Compat::Ok && cc != Compat::Skip)
					// P331：void 源初始化定在变量名；其他相容族保留原初值定位。
					report(cc == Compat::VoidValue && !s->var_ident_loc.file.empty() ? s->var_ident_loc : s->var_init->loc,
					       compat_init_msg(s->var_type, rt, cc));
			}
			// P324：聚合初始化器元素位类型相容（局部；上链对 InitList 跳过，此处补上）
			if (s->var_init && s->var_init->kind == ExprKind::InitList && s->var_type)
				check_aggregate_init(s->var_type, s->var_init);
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
					// P331：clang 印函数名（`void function 'h' should not return a value`，探针 p331bq）
					if (t && t->kind != TypeKind::Void)
						report(s->loc, "void function '" + cur_func_->name + "' should not return a value");
				}
				else if (cur_func_ && cur_func_->return_type)
				{
					// P335：非-void 函数 return void 值——clang `returning 'void' from a function with incompatible result type 'X'`
					//   定位在返回表达式起点（s->expr->loc）。
					CTypePtr t = resolve_type(s->expr);
					if (t && t->kind == TypeKind::Void)
						report(s->expr->loc, "returning 'void' from a function with incompatible result type " + describe_type(cur_func_->return_type));
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
			if (s->expr) { check_cond_scalar(s); check_expr(s->expr); }
			check_stmt(s->then_branch, loop_d, switch_d);
			check_stmt(s->else_branch, loop_d, switch_d);
			break;
		}
		case StmtKind::While:
			if (s->expr) { check_cond_scalar(s); check_expr(s->expr); }
			check_stmt(s->then_branch, loop_d + 1, switch_d);
			break;
		case StmtKind::DoWhile:
			check_stmt(s->then_branch, loop_d + 1, switch_d);
			if (s->expr) { check_cond_scalar(s); check_expr(s->expr); }
			break;
		case StmtKind::For:
		{
			// 同 Compound：不压独立作用域，for_init 声明的循环变量并入函数作用域。
			if (s->for_init) check_stmt(s->for_init, loop_d, switch_d);
			if (s->expr) { check_cond_scalar(s); check_expr(s->expr); }
			check_stmt(s->then_branch, loop_d + 1, switch_d);
			if (s->for_incr) check_expr(s->for_incr);
			break;
		}
		case StmtKind::Switch:
			// C23 switch init-statement：先处理 init_stmt（在其中声明的变量对选择式可见）
			if (s->init_stmt) check_stmt(s->init_stmt, loop_d, switch_d);
			if (s->expr) { check_cond_integer(s); check_expr(s->expr); }
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

	// P348：数组类型链上含运行期维（VLA）——sema 侧判据，与 codegen 的 type_has_vla_dim 同口径。
	static bool type_is_vla_array(const CTypePtr &t)
	{
		for (const CType *a = t.get(); a && a->kind == TypeKind::Array; a = a->base.get())
			if (a->is_vla && a->vla_dim) return true;
		return false;
	}

	// P348：前序扫描「goto / 标签 / VLA 声明」位置（独立一趟，不改动 P300 既有收集器）。
	struct VlaJumpScan {
		std::unordered_map<std::string, int> label_seq;
		std::vector<std::pair<int, std::string>> gotos;
		std::vector<SourceLoc> goto_locs;
		std::vector<std::pair<int, std::string>> vlas;
	};

	void scan_vla_jumps(const StmtPtr &s, int &seq, VlaJumpScan &out)
	{
		if (!s) return;
		const int my = seq++;
		switch (s->kind)
		{
		case StmtKind::Label:
			out.label_seq.emplace(s->label_name, my);
			break;
		case StmtKind::Goto:
			out.gotos.push_back({my, s->label_name});
			out.goto_locs.push_back(s->loc);
			break;
		case StmtKind::Decl:
			if (s->var_type && type_is_vla_array(s->var_type))
				out.vlas.push_back({my, s->var_name});
			break;
		default:
			break;
		}
		scan_vla_jumps(s->init_stmt, seq, out);
		scan_vla_jumps(s->for_init, seq, out);
		if (s->kind == StmtKind::Compound)
			for (const auto &b : s->body) scan_vla_jumps(b, seq, out);
		else
		{
			scan_vla_jumps(s->then_branch, seq, out);
			scan_vla_jumps(s->else_branch, seq, out);
		}
	}

	// P348：switch body 内 case/default 跳过同块 VLA 初始化（clang: cannot jump from switch
	//   statement to this case label）。嵌套 switch 由外层递归单独处理，此处不进其 body。
	void scan_switch_vla_cases(const StmtPtr &s, int &seq,
		std::vector<std::pair<int, std::string>> &vlas,
		std::vector<std::pair<int, SourceLoc>> &cases)
	{
		if (!s) return;
		const int my = seq++;
		if (s->kind == StmtKind::Decl && s->var_type && type_is_vla_array(s->var_type))
			vlas.push_back({my, s->var_name});
		if (s->kind == StmtKind::Case || s->kind == StmtKind::Default)
			cases.push_back({my, s->loc});
		if (s->kind == StmtKind::Switch) return;
		if (s->kind == StmtKind::Compound)
			for (const auto &b : s->body) scan_switch_vla_cases(b, seq, vlas, cases);
		else
		{
			scan_switch_vla_cases(s->then_branch, seq, vlas, cases);
			scan_switch_vla_cases(s->else_branch, seq, vlas, cases);
		}
	}

	void walk_switch_case_vla(const StmtPtr &s)
	{
		if (!s) return;
		if (s->kind == StmtKind::Switch)
		{
			std::vector<std::pair<int, std::string>> vlas;
			std::vector<std::pair<int, SourceLoc>> cases;
			int seq = 0;
			scan_switch_vla_cases(s->then_branch, seq, vlas, cases);
			for (const auto &c : cases)
				for (const auto &v : vlas)
					if (v.first < c.first)
					{
						report(c.second, "jump bypasses initialization of variable length array '" + v.second + "'");
						break;
					}
		}
		if (s->kind == StmtKind::Compound)
			for (const auto &b : s->body) walk_switch_case_vla(b);
		else
		{
			walk_switch_case_vla(s->then_branch);
			walk_switch_case_vla(s->else_branch);
		}
	}

	// P348：禁止 goto/case 跳过 VLA 初始化——否则 codegen 的 stackrestore 可能读未经 stacksave 的帧。
	//   判据循 clang：只拒「前向跳过声明」；后向跳回 VLA 之前 clang 接受（重新 alloca，语义正确），
	//   此处不判，避免误杀 clang 容忍的写法。
	void check_jump_bypass_vla(const StmtPtr &body)
	{
		if (!body) return;
		VlaJumpScan sc;
		int seq = 0;
		scan_vla_jumps(body, seq, sc);
		if (!sc.vlas.empty())
			for (std::size_t i = 0; i < sc.gotos.size(); ++i)
			{
				auto lit = sc.label_seq.find(sc.gotos[i].second);
				if (lit == sc.label_seq.end()) continue;
				for (const auto &v : sc.vlas)
					if (sc.gotos[i].first < v.first && v.first < lit->second)
					{
						report(sc.goto_locs[i], "jump to label '" + sc.gotos[i].second +
							"' bypasses initialization of variable length array '" + v.second + "'");
						break;
					}
			}
		walk_switch_case_vla(body);
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
		check_jump_bypass_vla(f.body);     // P348：禁 goto/case 跳过 VLA 初始化（stackrestore 支配性）
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
	// P305：tag → 真定义（只收 `is_definition`）。`structs_` 按插入序后写胜出，同名前置引用
	//   空壳（成员表为空）可能盖掉真定义；成员名校核必须看真定义，否则会把合法成员报成不存在。
	std::unordered_map<std::string, const StructDef *> real_struct_defs_;
	// P344：裸名 tag → 该 tag 的全部条目（含前置引用空壳），按 merge 后的 TU 顺序。`structs_` 是
	//   last-wins 单槽，同名记录分属宿主与多个模块时会互相顶替；分组后由 record_def_of 按当前
	//   代码归属挑定义（模块内自引用恒绑自身，宿主裸名恒绑宿主自有定义）。
	std::unordered_map<std::string, std::vector<const StructDef *>> struct_defs_by_tag_;
	// P302：public 标签 → 导出它的模块 key 集合（struct/union/enum 共用一张表；C 里 tag 是
	//   独立名字空间，故不与 xmod_public_owners（函数/全局）混用）。owner ≥2 即「同名标签
	//   被多个模块导出」，定义侧告警、宿主裸名引用侧报错——修前无任何检测，布局由后
	//   import 者静默胜出（h4 族缺陷）。
	std::unordered_map<std::string, std::unordered_set<std::string>> xmod_type_owners_;
	// P217：enum 类型标签→定义（供跨模块类型可见性门禁）
	std::unordered_map<std::string, const EnumDef *> enums_;
	// P300 Co class（0.4.0）：类名→定义
	std::unordered_map<std::string, const ClassDef *> classes_;
	// P304：文件作用域 typedef 的导出面（全量，含非 public）。parse 期已是展开完毕的形状，
	//   故这两张表只服务两处诊断口径：写错模块别名，以及「该名属于哪个模块但没导出」。
	std::unordered_set<std::string> typedef_names_;                              // 裸名一键可查
	std::unordered_map<std::string, std::unordered_set<std::string>> typedef_owner_; // 名→模块键集合
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
