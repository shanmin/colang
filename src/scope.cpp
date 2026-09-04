#include "colang.h"

std::vector<VARLIST> varlist;

//循环跳转栈（break / continue N）。两栈等长同步 push/pop，深度 = 当前循环嵌套层数（仅 for/do/while 计入）。
static std::vector<llvm::BasicBlock*> g_break_bb_stack;
static std::vector<llvm::BasicBlock*> g_continue_bb_stack;

// struct 类型注册表（名字空间独立于变量表）
static std::map<std::string, llvm::StructType*> g_struct_type_map;
// struct 字段名→索引映射：struct codegen 时写；字段读/写 GEP 时读
static std::map<llvm::StructType*, std::map<std::string, unsigned>> g_struct_field_index;
// struct/class 的 import 侧可见性标记：
//   true  → import 模块在 merge_imported_struct_scope 时会 forward 注册到外层 scope（import 方能通过类型名 new / 声明指针变量）
//   false → 不 forward 注册，import 方查不到类型名 → 报 "未定义的类型名"
// 仅当显式写了 `public class NAME` 时由 AST_class codegen 置 true；其余 class / struct 都按 false 处理（安全默认）
static std::map<std::string, bool> g_struct_import_public;


bool scope::has_var_any_scope(const std::string& name)
{
	//按作用域从顶到底（最近优先）查任一作用域变量表：存在任一（含 value=null 的占位登记）同名 → true
	for (int vi = (int)varlist.size() - 1; vi >= 0; vi--)
	{
		if (varlist[vi].info.find(name) != varlist[vi].info.end())
			return true;
	}
	return false;
}
bool scope::has_global_function_or_ivar(const std::string& name)
{
	// ir_module 在 ir() 入口 new；register_struct_type(_forward) 调这里一定在 ir() 内部 codegen 阶段，ir_module 非空
	if (!ir_module) return false;
	// Function 名（llvm intrinsics 不会撞，printf/putchar 外部声明也会命中）
	if (ir_module->getFunction(name)) return true;
	// GlobalVariable 名（当前 colang 没生成 GlobalVariable，全局变量是 alloca in entry_main；这里兜底防将来生成）
	if (ir_module->getGlobalVariable(name)) return true;
	return false;
}

bool scope::has_struct_type(const std::string& name)
{
	return g_struct_type_map.find(name) != g_struct_type_map.end();
}
llvm::StructType* scope::get_struct_type(const std::string& name)
{
	auto it = g_struct_type_map.find(name);
	return (it == g_struct_type_map.end()) ? nullptr : it->second;
}
void scope::register_struct_type(llvm::StructType* st, const TOKEN& name_tok)
{
	if (!st) return;
	// 结构体名 与 变量名 同名 → 冲突（共享名字空间）
	if (has_var_any_scope(name_tok.Value)) {
		ErrorExit("struct name conflicts with variable name (they share one namespace)", name_tok);
	}
	// 结构体名 与 已存在的全局函数名（如 void S(){}/ 声明的 printf extern 等）同名 → 冲突
	if (has_global_function_or_ivar(name_tok.Value)) {
		ErrorExit("struct name conflicts with global symbol (function/global variable)", name_tok);
	}
	auto it = g_struct_type_map.find(name_tok.Value);
	if (it != g_struct_type_map.end()) {
		// 情形 A：ast_struct codegen 已复用 forward 的同 StructType* 指针 → 同一个对象，setBody 之后再次登记是幂等 noop
		if (it->second == st) return;
		// 情形 B：两个不同指针不同类型，不管 opaque 与否，都算重名
		ErrorExit("struct redefinition", name_tok);
	}
	g_struct_type_map[name_tok.Value] = st;
}
void scope::register_struct_type_forward(llvm::StructType* st, const std::string& sname, const TOKEN& diag_tok)
{
	if (!st) return;
	if (g_struct_type_map.find(sname) != g_struct_type_map.end())
		return; //已经登记过（opaque 或已 setBody），不用再存
	// 构造"真实 token"诊断：若调用方给了 diag_tok 且 Value 非空就用；否则退化造一个
	TOKEN real = diag_tok;
	if (real.Value.empty()) {
		(void)memset(&real, 0, sizeof(real));
		real.type = TOKEN_TYPE::code;
		real.Value = sname;
	}
	// 结构体名 与 变量名 同名 → 冲突（共享名字空间）
	if (has_var_any_scope(sname)) {
		ErrorExit("struct name conflicts with variable name (they share one namespace)", real);
	}
	// 结构体名 与 全局函数名冲突
	if (has_global_function_or_ivar(sname)) {
		ErrorExit("struct name conflicts with global symbol (function/global variable)", real);
	}
	g_struct_type_map[sname] = st;
}
unsigned scope::get_struct_field_index(llvm::StructType* st, const std::string& field_name)
{
	if (!st) return (unsigned)-1;
	auto it = g_struct_field_index.find(st);
	if (it == g_struct_field_index.end())
		return (unsigned)-1;
	auto f = it->second.find(field_name);
	if (f == it->second.end())
		return (unsigned)-1;
	return f->second;
}
// AST_struct::codegen 调用：注册该 StructType 的字段名→顺序（unsigned）映射
// 用公共静态暴露：简单做法，另外新增一个 scope public 函数在头里一起加了（见 ast.h）
void scope::set_struct_field_indexes(llvm::StructType* st, const std::vector<TOKEN>& field_names)
{
	if (!st) return;
	auto& m = g_struct_field_index[st];
	m.clear();
	for (size_t i = 0; i < field_names.size(); i++)
		m[field_names[i].Value] = (unsigned)i;
}
// 重载：直接给字段名字符串列表（import 快照 merge 场景没有 TOKEN）
void scope::set_struct_field_indexes(llvm::StructType* st, const std::vector<std::string>& field_names)
{
	if (!st) return;
	auto& m = g_struct_field_index[st];
	m.clear();
	for (size_t i = 0; i < field_names.size(); i++)
		m[field_names[i]] = (unsigned)i;
}

void scope::mark_struct_import_public(const std::string& sname, bool is_public)
{
	// sname 空就跳过（防御性），不写空串键
	if (sname.empty()) return;
	g_struct_import_public[sname] = is_public;
}
bool scope::is_struct_import_public(const std::string& sname)
{
	auto it = g_struct_import_public.find(sname);
	if (it == g_struct_import_public.end()) return false;  // 从未标记：按默认 private 处理（安全）
	return it->second;
}


void scope::push_loop_bb(llvm::BasicBlock* brk, llvm::BasicBlock* cont)
{
	g_break_bb_stack.push_back(brk);
	g_continue_bb_stack.push_back(cont);
}
void scope::pop_loop_bb()
{
	if (g_break_bb_stack.empty() || g_continue_bb_stack.empty()) {
		// 内部防御：不应该走到（codegen 正确情况下 push/pop 严格匹配）；不 ErrorExit 以免覆盖真错误，直接提前 return
		return;
	}
	g_break_bb_stack.pop_back();
	g_continue_bb_stack.pop_back();
}

// 从字面量层号（1-based）返回对应 BB；越界或空栈用关键字 token 带诊断 ErrorExit
llvm::BasicBlock* scope::get_break_bb(unsigned levels, const TOKEN& tok)
{
	if (g_break_bb_stack.empty()) {
		ErrorExit("'break' must be inside a loop (for/do/while)", tok);
	}
	if (levels == 0) {
		ErrorExit("'break N' requires N to be a positive integer literal (>=1)", tok);
	}
	if (levels > (unsigned)g_break_bb_stack.size()) {
		char msg[256];
		snprintf(msg, sizeof(msg), "'break %u' exceeds current loop nesting depth %zu", levels, g_break_bb_stack.size());
		ErrorExit(msg, tok);
	}
	return g_break_bb_stack[g_break_bb_stack.size() - levels];
}
llvm::BasicBlock* scope::get_continue_bb(unsigned levels, const TOKEN& tok)
{
	if (g_continue_bb_stack.empty()) {
		ErrorExit("'continue' must be inside a loop (for/do/while)", tok);
	}
	if (levels == 0) {
		ErrorExit("'continue N' requires N to be a positive integer literal (>=1)", tok);
	}
	if (levels > (unsigned)g_continue_bb_stack.size()) {
		char msg[256];
		snprintf(msg, sizeof(msg), "'continue %u' exceeds current loop nesting depth %zu", levels, g_continue_bb_stack.size());
		ErrorExit(msg, tok);
	}
	return g_continue_bb_stack[g_continue_bb_stack.size() - levels];
}

//压入新的作用域
//	name	作用域名称
void scope::push(std::string zone)
{
	VARLIST vlist;
	vlist.zone = zone;
	varlist.push_back(vlist);
}

//弹出作用域：先在当前 BB 生成栈 class 对象的析构调用（反序），再弹出作用域。
//   当前 BB 已带 terminator（例如 return 之后 AST_return 切到 __co__ret_after unreachable）则跳过生成，
//   因为这种 BB 要么 unreachable，要么由 return/sweep 出口自己在 CreateRet 前生成过析构。
void scope::pop()
{
	generate_dtor_calls_topscope();
	varlist.pop_back();
}

// 对 varlist.back() 这一层作用域：反序遍历 dtor_order，为每个栈 class 对象生成
//   %obj = load ptr, ptr %slot
//   call void @ClassName.__dtor(ptr %obj)
// 若 ClassName.__dtor 不存在（用户没定义析构）→ 静默跳过。
// zone="global" 时整层跳过（全局对象本轮不支持自动析构）。
void scope::generate_dtor_calls_topscope()
{
	if (varlist.empty()) return;
	VARLIST& cur = varlist.back();
	if (cur.zone == "global") return;
	if (cur.dtor_order.empty()) return;
	// 当前 BB 已带 terminator → 不能再塞指令；跳过
	llvm::BasicBlock* cur_bb = ir_builder ? ir_builder->GetInsertBlock() : nullptr;
	if (!cur_bb || cur_bb->getTerminatorOrNull() != nullptr) return;
	if (!ir_module) return;

	// 按声明反序：从 dtor_order.rbegin() → rend()
	for (auto it = cur.dtor_order.rbegin(); it != cur.dtor_order.rend(); ++it)
	{
		const std::string& vname = *it;
		auto fit = cur.info.find(vname);
		if (fit == cur.info.end()) continue;
		VARINFO& vi = fit->second;
		if (!vi.pointee_st) continue;
		llvm::StructType* st = vi.pointee_st;
		std::string class_name = st->getName().str();
		if (class_name.empty()) continue;

		// 查 ClassName.__dtor 函数（存在才生成 call）
		std::string dtor_name = class_name + ".__dtor";
		llvm::Function* dtor_fn = ir_module->getFunction(dtor_name);
		if (!dtor_fn) continue;  // 用户没定义析构，跳过

		// 校验 dtor_fn 取参：应恰好 1 个 this 指针
		if (dtor_fn->arg_size() != 1) continue;

		// 指针槽模式：VARINFO.value 是 slot（保存对象真实地址的槽）
		//   先 load slot 拿到 this 指针（= 对象真实存储地址）
		llvm::Value* slot = vi.value;
		if (!slot) continue;
		llvm::Value* this_ptr = ir_builder->CreateLoad(ir_builder->getPtrTy(), slot, vname + ".as_obj");

		// 构造参数列表（1 个 this）
		std::vector<llvm::Value*> args = { this_ptr };
		// call（析构返回 void，无返回值）
		(void)ir_builder->CreateCall(dtor_fn, args);
	}
}

// 从当前作用域最顶层 downto index 1（index 0 = global 保留不析构），
//   每层依次调 generate_dtor_calls_topscope。
// 用在 AST_return（CreateRet 前）和 sweep 补 ret 前。
void scope::generate_all_dtor_calls_to_leave_functions()
{
	// 注意：这里"从 varlist.size()-1 downto 1"只生成 IR 调用，不 pop 作用域，
	//   scope::pop 仍然在各结构 codegen 尾部调用（但因为 BB 已 terminator，会跳过重复生成，不产生重复 call）。
	int sz = (int)varlist.size();
	for (int lv = sz - 1; lv >= 1; lv--)
	{
		VARLIST& layer = varlist[lv];
		if (layer.zone == "global") continue;
		if (layer.dtor_order.empty()) continue;

		llvm::BasicBlock* cur_bb = ir_builder ? ir_builder->GetInsertBlock() : nullptr;
		if (!cur_bb || cur_bb->getTerminatorOrNull() != nullptr) continue;
		if (!ir_module) continue;

		for (auto it = layer.dtor_order.rbegin(); it != layer.dtor_order.rend(); ++it)
		{
			const std::string& vname = *it;
			auto fit = layer.info.find(vname);
			if (fit == layer.info.end()) continue;
			VARINFO& vi = fit->second;
			if (!vi.pointee_st) continue;
			llvm::StructType* st = vi.pointee_st;
			std::string class_name = st->getName().str();
			if (class_name.empty()) continue;

			std::string dtor_name = class_name + ".__dtor";
			llvm::Function* dtor_fn = ir_module->getFunction(dtor_name);
			if (!dtor_fn) continue;
			if (dtor_fn->arg_size() != 1) continue;

			llvm::Value* slot = vi.value;
			if (!slot) continue;
			llvm::Value* this_ptr = ir_builder->CreateLoad(ir_builder->getPtrTy(), slot, vname + ".as_obj");
			std::vector<llvm::Value*> args = { this_ptr };
			(void)ir_builder->CreateCall(dtor_fn, args);
		}
	}
}

//设置变量
void scope::set(VARINFO vi)
{
	// 共享名字空间：同名 struct tag 已存在 → 冲突
	if (has_struct_type(vi.token.Value)) {
		ErrorExit("variable name conflicts with struct name (they share one namespace)", vi.token);
	}
	//varlist[varlist.size() - 1].info[vi.name] = vi;
	if (varlist[varlist.size() - 1].info.find(vi.token.Value) == varlist[varlist.size() - 1].info.end())
	{
		varlist[varlist.size() - 1].info[vi.token.Value] = vi;
		// 栈 class 对象：符合条件就把变量名加入 dtor_order（按声明顺序，析构时反序遍历）
		//   条件：
		//     1. pointee_st != nullptr（是一个指向 class 结构体的指针槽）
		//     2. type_source_name 不以 '*' 结尾（源码声明不是指针变量，是值变量 → 栈对象）
		//     3. 当前 zone != "global"（全局对象本轮不自动析构）
		VARLIST& cur = varlist.back();
		if (cur.zone != "global"
			&& vi.pointee_st != nullptr
			&& !vi.type_source_name.empty()
			&& vi.type_source_name.back() != '*')
		{
			cur.dtor_order.push_back(vi.token.Value);
		}
	}
	else
		ErrorExit("ERROR: redefinition of variable", vi.token);
}

// 单次编译结束，清理 scope 内所有全局临时注册表（struct map / field index / varlist / loop栈）—— 避免同进程多编译的交叉污染
void scope::clear_all_compile()
{
	g_struct_type_map.clear();
	g_struct_field_index.clear();
	g_struct_import_public.clear();
	varlist.clear();
	g_break_bb_stack.clear();
	g_continue_bb_stack.clear();
	// 函数重载 bucket 同步清空（顶层 ir() 结尾调用，确保同进程多次编译互不串台）
	overload_clear();
}

//嵌套 import 编译 save/restore：避免嵌套层 clear_all_compile 毁掉外层作用域注册表
//  设计：返回一个 opaque State（堆上 copy），restore 时整体替换回来。
//  用 void* 避免 scope.h 暴露内部 map 类型；内部用 struct ScopeState 包装。
struct ScopeState {
	std::map<std::string, llvm::StructType*> struct_type_map;
	std::map<llvm::StructType*, std::map<std::string, unsigned>> field_index;
	std::map<std::string, bool> struct_import_public;
	std::vector<VARLIST> varlist_copy;
	std::vector<llvm::BasicBlock*> break_bb;
	std::vector<llvm::BasicBlock*> continue_bb;
	// 析构顺序表同步：VARLIST 里 dtor_order 已经包含在 varlist_copy 深拷贝中（因为 VARLIST 是按值拷贝 struct），无需额外字段。
	//   此处注释保留提醒将来若拆分成员，要保证 dtor_order 一并拷贝。
};
void* scope::save_state()
{
	auto* s = new ScopeState();
	s->struct_type_map       = g_struct_type_map;
	s->field_index           = g_struct_field_index;
	s->struct_import_public  = g_struct_import_public;
	s->varlist_copy          = varlist;
	s->break_bb              = g_break_bb_stack;
	s->continue_bb           = g_continue_bb_stack;
	return s;
}
void scope::restore_state(void* state)
{
	if (!state) return;
	auto* s = static_cast<ScopeState*>(state);
	g_struct_type_map.swap(s->struct_type_map);
	g_struct_field_index.swap(s->field_index);
	g_struct_import_public.swap(s->struct_import_public);
	varlist.swap(s->varlist_copy);
	g_break_bb_stack.swap(s->break_bb);
	g_continue_bb_stack.swap(s->continue_bb);
	delete s;
}

//获取变量
VARINFO scope::get(TOKEN token)
{
	for (int vi = varlist.size() - 1; vi >= 0; vi--)
	{
		if (varlist[vi].info.find(token.Value) == varlist[vi].info.end())
			continue; //未找到指定变量名称
		VARINFO vinfo = varlist[vi].info[token.Value];
		if (vinfo.value)
			return vinfo;
			//如果当前是 function 作用域且 vinfo.value 为 null（占位登记），继续向更外层作用域查找。
		//  FIX（2026-09-02）：旧代码 vi=0 导致下次循环 vi-- → -1，直接跳过全局作用域走到 ErrorExit，
		//    让全局作用域里的同名合法变量也报"不存在"。改为 continue 让循环自然 vi-- 到外层作用域，
		//    这样 function 外的 codeblock 作用域（若存在）也会被检查，最后抵达全局作用域。
		if (varlist[vi].zone == "function")
			continue;
	}
	//报错
	ErrorExit("ERROR: variable does not exist", token);
	// 修复 C4715：即使 ErrorExit 已 exit(1) 永不返回，也补一条 return 消编译器警告（否则"不是所有控件路径都返回值"）
	return VARINFO{};
}

// ================= import struct scope 合并 =======================
// 快照句柄：把 inner scope 的 struct name→(StructType*, field_index) 保存下来，
//   与 LLVM 模块无关；merge 时 outer 用名字重新 forward 注册。
struct StructScopeSnap {
	// 注意：存的是 inner 的 StructType*（属于 ir_context，同一个 LLVMContext），
	//   但 outer scope 注册表会用 ir_context 重新 StructType::create/getTypeByName，
	//   与 inner 可能返回同一个 StructType*（若之前 create 过）。为避免混乱，
	//   我们在这里额外存一份"字段名列表"（按索引升序），merge 时用 register_struct_type_forward
	//   重新登记 struct，然后按字段名→索引逐一写回。
	struct SnapEntry {
		std::vector<std::pair<std::string, unsigned>> fields; // name, idx（有序不做假设有序）
		bool is_import_public = false;  // import 侧可见性：true 才会被 merge 到外层
	};
	std::map<std::string, SnapEntry> entries;
};

void* scope::snapshot_struct_scope_from_current()
{
	auto* snap = new StructScopeSnap();
	// 遍历当前 g_struct_type_map，记录每个 struct 的字段索引 + import 可见性
	for (auto& kv : g_struct_type_map)
	{
		const std::string& sname = kv.first;
		llvm::StructType* st = kv.second;
		StructScopeSnap::SnapEntry e;
		// import 可见性：查 g_struct_import_public；没登记按 false（不对外暴露）
		auto pit = g_struct_import_public.find(sname);
		if (pit != g_struct_import_public.end())
			e.is_import_public = pit->second;
		else
			e.is_import_public = false;
		auto fit = g_struct_field_index.find(st);
		if (fit != g_struct_field_index.end())
		{
			for (auto& fkv : fit->second)
				e.fields.push_back(std::make_pair(fkv.first, fkv.second));
		}
		snap->entries[sname] = std::move(e);
	}
	return snap;
}

void scope::merge_imported_struct_scope(void* snap_handle, const TOKEN& diag_tok,
    const std::vector<import_name_entry>& names, bool star)
{
	if (!snap_handle) return;
	// 构建白名单原名集合(struct 不支持方法别名:类型名改了会破坏 LLVM link 类型一致性,
	//   故 struct/class 只做白名单筛选,不重命名;方法别名只对函数有效)
	std::set<std::string> want_set;
	for (auto& e : names) want_set.insert(e.orig);

	auto* snap = static_cast<StructScopeSnap*>(snap_handle);
	for (auto& kv : snap->entries)
	{
		const std::string& sname = kv.first;
		// === 关键：只有 import_public=true 的类/结构才暴露到外层 ===
		//    未标 public class 的 class 与全部 struct（除未来 public struct 外）都会被过滤。
		if (!kv.second.is_import_public)
			continue;
		// selective 模式:只 merge 白名单里的 struct(按原名匹配)
		if (!star && !want_set.count(sname))
			continue;

		// outer 已存在同名 struct → 不覆盖（用户自己定义的优先，不会和 import 冲突）
		if (has_struct_type(sname)) continue;
		// 创建 Opaque（未 setBody）的 StructType：用 StructType::create(ctx, name)
		//   若 ir_context 中已存在同名字的 StructType（getTypeByName 能拿到），复用它。
		llvm::StructType* st = llvm::StructType::getTypeByName(ir_context, sname);
		if (!st)
			st = llvm::StructType::create(ir_context, sname);
		// forward 注册到 outer scope
		register_struct_type_forward(st, sname, diag_tok);
		// 写字段索引：按索引升序重建 vector<string>，调用 string 重载 set_struct_field_indexes
		//   因为字段 pair<name,idx> 只存了名称到索引（非升序），这里先按 idx 排序再转成顺序列表
		auto& fields = kv.second.fields;
		if (!fields.empty())
		{
			// 先按 idx 找出最大 idx → 生成定长 vec
			unsigned max_idx = 0;
			for (auto& f : fields) if (f.second > max_idx) max_idx = f.second;
			std::vector<std::string> names_ordered(max_idx + 1, std::string());
			for (auto& f : fields) names_ordered[f.second] = f.first;
			// 防御：空槽位（idx 跳号）补 "<__missing__>"（不应发生），避免后续 "" 撞 key
			for (auto& s : names_ordered) if (s.empty()) s = "<__missing__>";
			set_struct_field_indexes(st, names_ordered);
		}
	}
	delete snap;
}

//	THE END
