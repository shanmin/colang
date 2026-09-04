////////////////////////////////////////////////////////////////////////////////
//
// AST_import — import "filename"; 语句
//
// 功能：
//  1. 解析 import 语法：import <字符串token|裸标识符> ;
//  2. codegen 时：递归编译目标 .co（独立 co2bc 调用）→ 生成独立 LLVM module / .bc
//     → 符号以"模块名.原名"mangle → 把模块中所有非 main/非保留的函数声明
//     以 ExternalLinkage declaration 的形式注入当前 ir_module（这样调用方
//     才能 ir_module->getFunction("mod.fn") 查到）→ 把目标 .bc 加入链接清单
//  3. 去重 + 防环：同一 .co 绝对路径只处理一次（import_has_processed / mark_processed）
//
// 约束：
//  - import 语句必须在任何可执行 codegen 之前（函数定义/变量声明/if/while 等之前）
//    当前不强制，只是约定 —— 放在 ast1() 最前面解析，放后面仍会处理，
//    但在 import 之前定义的函数/变量不能使用 import 进来的符号（尚未声明）
//  - 顶层 main 模块不做符号 mangling（main / 顶层函数名保留原名）
//  - 被 import 的模块所有非保留用户符号都 mangling（含函数、全局变量）
//
////////////////////////////////////////////////////////////////////////////////

#include "colang.h"
#include <filesystem>
#include <set>
#include "llvm/Support/MemoryBuffer.h"

//------------------------------ 全局 import 状态（跨 co2m 共享）------------------------------

// 按绝对路径去重：同一 .co 只编译一次（即使多处 import 或形成环）
static std::set<std::string> g_import_processed_co;
// 所有参与最终链接的 .bc 绝对路径清单（顺序不重要，llvm-link 全合并）
static std::vector<std::string> g_import_bc_list;
// 顶层 co2m 的 filename（用于写 manifest 时定位），co2m 入口设置一次
static std::string g_import_main_co_path;
// 是否嵌套模式（import 内部 co2m 调用是"嵌套"，写文件方式不同）
static bool g_ir_nested_mode = false;
// 当前 codegen 所属模块名（"" = 顶层，兼容旧行为，不 mangle）
static std::string g_current_module_name;

// 跨模块符号注册表：「用户函数原名 → 已见 mangled_name 列表」
//   登记时机：AST_import codegen 注入 import 模块的 External decl 时（或从 .bc 读模块时扫 Function）
//   查询时机：AST_call codegen → 查不到 ir_module 内函数 → 查该表
//     - 0 项：不存在（ErrorExit）
//     - 1 项：自动跳转 mangled 名（跨模块调用）
//     - ≥2 项：同名冲突（两个 import 模块都有 add2）→ ErrorExit 提示歧义
static std::map<std::string, std::vector<std::string>> g_import_name_to_mangled;

// 已 import 的模块名集合（去重）：AST_import::codegen 成功 load 模块后登记（幂等）；
//   AST_var / AST_new 遇到 "模块名.类型名" 限定写法时用它校验模块前缀是否真 import 过
static std::set<std::string> g_registered_module_names;

// 模块别名映射表：alias → stem（原模块名）
//   import "x" as B → 登记 B → x；原名 x 不登记到 g_registered_module_names
//   （故起别名后原模块名不可用于限定调用，只允许用别名 B）
static std::map<std::string, std::string> g_module_aliases;

// 不进入 mangling 的 C 外部保留符号
static bool is_reserved_c_extern(const std::string& s) {
	static const std::set<std::string> reserved = { "printf", "scanf", "malloc", "free",
		"memcpy", "memset", "strlen", "puts", "putchar", "getchar", "exit", "abort" };
	return reserved.count(s) != 0;
}

//------------------------------ 路径辅助 ------------------------------

static std::string path_to_abs(const std::string& p) {
	try {
		return std::filesystem::absolute(p).string();
	}
	catch (...) {
		return p;
	}
}

static std::string path_normalize_backslash(std::string p) {
	for (auto& c : p) if (c == '/') c = '\\';
	return p;
}

static std::string path_dirname(const std::string& abs_path) {
	try {
		auto p = std::filesystem::path(abs_path).parent_path();
		if (p.empty()) return ".";
		return p.string();
	}
	catch (...) {
		return ".";
	}
}

static bool path_exists(const std::string& p) {
	try { return std::filesystem::exists(p); }
	catch (...) { return false; }
}

std::string import_resolve_path(const char* cur_source_file, const std::string& import_target, const TOKEN& diag_tok) {
	//1. 若无扩展名 → 补 .co
	std::string target = import_target;
	{
		std::filesystem::path tp(target);
		if (tp.extension().empty())
			target += ".co";
	}
	//2. 相对 cur_source_file 的目录解析
	std::string cur_dir = path_dirname(path_to_abs(cur_source_file));
	std::string candidate = cur_dir + "\\" + target;
	candidate = path_normalize_backslash(candidate);
	candidate = path_to_abs(candidate);
	if (!path_exists(candidate)) {
		// 也允许相对于启动目录（主 .co 所在），但以 cur 目录优先
		// 找不到就报错
		ErrorExit(("import file not found: " + candidate).c_str(), diag_tok);
	}
	return path_normalize_backslash(candidate);
}

std::string import_module_name(const std::string& co_path) {
	try {
		auto stem = std::filesystem::path(co_path).stem().string();
		return stem.empty() ? "mod" : stem;
	}
	catch (...) { return "mod"; }
}

bool import_has_processed(const std::string& abs_co_path) {
	return g_import_processed_co.count(path_normalize_backslash(abs_co_path)) != 0;
}
void import_mark_processed(const std::string& abs_co_path) {
	g_import_processed_co.insert(path_normalize_backslash(abs_co_path));
}

void import_add_bc_to_link_list(const std::string& abs_bc_path) {
	std::string p = path_normalize_backslash(abs_bc_path);
	for (auto& x : g_import_bc_list) if (x == p) return;
	g_import_bc_list.push_back(p);
}

void import_write_manifest(const char* main_co_path) {
	// manifest：主 .co 同目录下 <main_base>_import_list.txt，每行一个 .bc 绝对路径
	//  顺序：主 .bc 放第一个，import 模块按注册顺序跟进（顺序不影响 llvm-link）
	std::string main_abs = path_normalize_backslash(path_to_abs(main_co_path));
	std::string main_base = co_base(main_abs.c_str());
	std::string manifest = main_base + "_import_list.txt";
	FILE* fp = fopen(manifest.c_str(), "wb");
	if (!fp) {
		// 写 manifest 失败不影响编译（批处理退化成只有主 .bc），打 warning
		fprintf(stderr, "warning: cannot write import manifest %s (batch script may miss imports)\n", manifest.c_str());
		return;
	}
	for (auto& bc : g_import_bc_list) {
		fwrite(bc.c_str(), 1, bc.size(), fp);
		fputc('\n', fp);
	}
	fclose(fp);
}

void import_clear_state() {
	g_import_processed_co.clear();
	g_import_bc_list.clear();
	g_import_main_co_path.clear();
	g_ir_nested_mode = false;
	g_current_module_name.clear();
	g_import_name_to_mangled.clear();
	g_registered_module_names.clear();
	g_module_aliases.clear();
	// 函数重载 bucket 同步清理（顶层编译入口清一次，嵌套 import 不单独调这里）
	overload_clear();
}

void import_register_module(const std::string& name) {
	if (name.empty()) return;
	g_registered_module_names.insert(name);
}
bool import_has_registered_module(const std::string& name) {
	if (name.empty()) return false;
	return g_registered_module_names.count(name) != 0;
}
void import_clear_registered_modules() {
	g_registered_module_names.clear();
}

//------------------------------ 模块别名 ------------------------------

void import_register_module_alias(const std::string& alias, const std::string& stem) {
	if (alias.empty() || stem.empty()) return;
	g_module_aliases[alias] = stem;
}
std::string import_resolve_module_alias(const std::string& name) {
	auto it = g_module_aliases.find(name);
	if (it != g_module_aliases.end()) return it->second;
	return name;
}
void import_clear_module_aliases() {
	g_module_aliases.clear();
}

//------------------------------ ir 嵌套 / module / mangling ------------------------------

void ir_set_nested_mode(bool nested) { g_ir_nested_mode = nested; }
bool ir_is_nested_mode() { return g_ir_nested_mode; }

void ir_set_current_module_name(const std::string& mod) { g_current_module_name = mod; }
const std::string& ir_get_current_module_name() { return g_current_module_name; }

std::string ir_mangle_symbol(const std::string& user_name) {
	if (user_name.empty()) return user_name;
	// C 外部保留符号（printf 等）永远不 mangle
	if (is_reserved_c_extern(user_name)) return user_name;
	// 顶层模块不 mangle（main 也走这里：保持 @main 供系统 CRT）
	if (g_current_module_name.empty()) return user_name;
	// 非顶层："模块名.原名"（用 '.' 分隔）
	return g_current_module_name + "." + user_name;
}

llvm::Function* ir_find_function_or_nul(const std::string& call_name) {
	// 优先级 1：ir_module 中按原名直接命中（顶层函数 / printf / 已被 import_decl 注入的 mangled 名
	//   （注意：注入的 mangled 名是 "mod.fn"，用户 call 名是 "fn"，走这条只会命中同模块函数）
	llvm::Function* f = ir_module->getFunction(call_name);
	if (f) return f;
	// 优先级 2：当前非顶层 → 按"当前模块名.原名"查（同模块内函数互调用时的 mangling）
	if (!g_current_module_name.empty()) {
		std::string m = g_current_module_name + "." + call_name;
		f = ir_module->getFunction(m);
		if (f) return f;
	}
	// 优先级 3：跨模块符号注册表查（import 进来的函数）
	auto it = g_import_name_to_mangled.find(call_name);
	if (it != g_import_name_to_mangled.end()) {
		const auto& vec = it->second;
		if (vec.size() == 1) {
			f = ir_module->getFunction(vec[0]);
			if (f) return f;
			// 理论上 decl 注入过一定存在；极端情况返回 nullptr 继续往下报 not found
			return nullptr;
		} else if (vec.size() >= 2) {
			// 歧义：多处 import 都有同名函数（暂不支持显式模块前缀调用）
			// 此处不直接 ErrorExit（缺 TOKEN 定位），返回 nullptr 让 AST_call 上层统一报 not found
			//  （TODO 未来可把歧义 msg 写静态线程本地缓存）
			return nullptr;
		}
	}
	//  4) 保留 C extern 找不到：原样返回 nullptr 让 AST_call 层报错
	return nullptr;
}

void ir_import_external_decls(llvm::Module& src, llvm::Module& dst,
    const std::vector<import_name_entry>& names, bool star, const TOKEN& diag_tok) {
	llvm::LLVMContext& ctx = dst.getContext();
	// 构建白名单原名集合 + 别名映射(原名→别名)
	std::set<std::string> want_set;
	std::map<std::string, std::string> alias_map;
	for (auto& e : names) {
		want_set.insert(e.orig);
		if (!e.alias.empty()) alias_map[e.orig] = e.alias;
	}
	std::set<std::string> matched;  // selective 模式下记录已匹配的原名(用于校验全部找到)

	for (llvm::Function& sf : src) {
		if (sf.isDeclaration()) continue;
		if (!sf.hasExternalLinkage()) continue;
		std::string sname = sf.getName().str();
		if (is_reserved_c_extern(sname)) continue;
		if (sname == "main") continue;
		// 原名 = mangled 名最后一个 '.' 后半段;无 '.' → 顶层函数,不导入
		std::string orig = sname;
		auto dot = sname.rfind('.');
		if (dot != std::string::npos && dot + 1 < sname.size())
			orig = sname.substr(dot + 1);
		else
			continue;  // 无 mangling(顶层函数),不导入
		// selective 模式:只处理白名单里的原名
		if (!star) {
			if (!want_set.count(orig)) continue;
			matched.insert(orig);
		}
		// 登记跨模块符号:注册表 key = 别名(若有)否则原名
		std::string reg_key = orig;
		auto am = alias_map.find(orig);
		if (am != alias_map.end()) reg_key = am->second;
		{
			auto& vec = g_import_name_to_mangled[reg_key];
			bool found = false;
			for (auto& x : vec) if (x == sname) { found = true; break; }
			if (!found) vec.push_back(sname);
		}
		// 注入 ExternalLinkage decl(若未注入);decl 名用 mangled 名 sname,与 .bc 一致才能链接
		if (dst.getFunction(sname)) continue;
		llvm::FunctionType* sfty = sf.getFunctionType();
		llvm::Function::Create(sfty, llvm::GlobalValue::ExternalLinkage, sname, &dst);
	}
	// selective 模式:校验用户要的名称是否全部找到(找不到报错)
	if (!star) {
		for (auto& e : names) {
			if (!matched.count(e.orig)) {
				ErrorExit(("import name not found in module: " + e.orig).c_str(), diag_tok);
			}
		}
	}
	(void)ctx;
}

//------------------------------ AST_import：构造 + show + codegen ------------------------------

AST_import::AST_import(std::vector<TOKEN>& tokens) {
	// tokens[0] = import (code type, Value="import")
	import_tok = tokens[0];
	tokens.erase(tokens.begin());

	// 解析名称列表: * | name1 [as alias1] (, name2 [as alias2])*
	if (tokens.empty()) {
		ErrorExit("import syntax error: expected '*' or name list, usage: import a, b from \"file\";", import_tok);
	}
	if (tokens[0].type == TOKEN_TYPE::opcode && tokens[0].Value == "*") {
		star = true;
		tokens.erase(tokens.begin());
	} else {
		for (;;) {
			if (tokens.empty() || tokens[0].type != TOKEN_TYPE::code) {
				ErrorExit("import syntax error: expected name in import list", import_tok);
			}
			import_name_entry e;
			e.orig = tokens[0].Value;
			tokens.erase(tokens.begin());
			// 可选 as alias(方法别名)
			if (!tokens.empty() && tokens[0].type == TOKEN_TYPE::code && tokens[0].Value == "as") {
				tokens.erase(tokens.begin());
				if (tokens.empty() || tokens[0].type != TOKEN_TYPE::code) {
					ErrorExit("import syntax error: expected alias after 'as'", import_tok);
				}
				e.alias = tokens[0].Value;
				tokens.erase(tokens.begin());
			}
			names.push_back(e);
			// 逗号 → 继续解析下一项;否则名称列表结束
			if (!tokens.empty() && tokens[0].type == TOKEN_TYPE::opcode && tokens[0].Value == ",") {
				tokens.erase(tokens.begin());
				continue;
			}
			break;
		}
	}

	// from 关键字
	if (tokens.empty() || tokens[0].type != TOKEN_TYPE::code || tokens[0].Value != "from") {
		ErrorExit("import syntax error: expected 'from', usage: import a, b from \"file\";", import_tok);
	}
	tokens.erase(tokens.begin());

	// 文件名:string token(带引号,可含路径)或 code token(裸标识符)
	if (tokens.empty() || (tokens[0].type != TOKEN_TYPE::string && tokens[0].type != TOKEN_TYPE::code)) {
		ErrorExit("import syntax error: expected filename after 'from'", import_tok);
	}
	TOKEN file_tok = tokens[0];
	tokens.erase(tokens.begin());

	// 可选 as 模块别名:from "x" as B
	if (!tokens.empty() && tokens[0].type == TOKEN_TYPE::code && tokens[0].Value == "as") {
		tokens.erase(tokens.begin());
		if (tokens.empty() || tokens[0].type != TOKEN_TYPE::code) {
			ErrorExit("import syntax error: expected module alias after 'as'", import_tok);
		}
		module_alias = tokens[0].Value;
		tokens.erase(tokens.begin());
	}

	// 必须以 ; 结尾
	if (tokens.empty() || tokens[0].Value != ";") {
		ErrorExit("import statement error: expected semicolon ;", file_tok);
	}
	tokens.erase(tokens.begin());

	// 路径解析：相对当前执行 import 的源文件目录
	//  当前 .co 文件名存放在 token.filename 中（字符串 token 与 import 关键字同文件）
	filename = import_resolve_path(file_tok.filename.c_str(), file_tok.Value, file_tok);
}

void AST_import::show(std::string pre) {
	std::cout << pre << "#TYPE:import  file:" << filename << std::endl;
}

// 前向声明：co2bc 外部（colang.cpp）
extern void co2bc(const char* filename);
// 提供 co2bc 的"导入专用变体"：设置嵌套模块名、嵌套模式、结束后返回已生成 Module*
//  由于 co2bc 是全局 extern，我们把嵌套调用包到这里：
//  做法——先改全局模式（nested=true + module_name = mod），调用 co2bc，再恢复
//  但 co2bc 走 co2m → ir(ast_list, filename)，ir() 会重写 ir_module 等全局；
//  为避免嵌套调用把主模块全局状态搞坏，先保存并恢复所有全局 ir_* 变量。

// 保存/恢复 ir 全局状态的 RAII（简易 struct）
struct IRStateSaver {
	llvm::Module* saved_module = nullptr;
	std::unique_ptr<llvm::IRBuilder<>> saved_builder;
	std::vector<LABEL_LIST> saved_labellist;
	bool saved_nested;
	std::string saved_modname;
	llvm::BasicBlock* saved_insert_block = nullptr;
	void* saved_scope = nullptr;
	void* saved_overload = nullptr; // 函数重载 bucket 快照：嵌套 import 时 save/restore，避免内层污染外层

	IRStateSaver() {
		saved_module = ir_module;
		// unique_ptr 的 ownership 转移：临时把 ir_builder 搬到 saver，恢复时搬回去
		saved_builder.swap(ir_builder);
		saved_labellist.swap(ir_labellist);
		saved_nested = ir_is_nested_mode();
		saved_modname = ir_get_current_module_name();
		if (saved_builder) saved_insert_block = saved_builder->GetInsertBlock();
		saved_scope    = scope::save_state();   // scope 全快照（struct 表/varlist/loop 栈）
		saved_overload = overload_save_state(); // 函数重载 bucket 全快照（避免嵌套层登记串台到外层）
		overload_clear();                       // ★ Step 7 修复：嵌套编译期间 overload_map 必须是全新的空状态，
		                                        //   否则外层先合并的 test1.aa 会污染嵌套 test2 的登记（同签名 aa 报错重定义）。
		                                        //   嵌套编译结束后析构里会：restore 外层 saved_overload + merge 嵌套层 public 到外层。
		ir_module = nullptr;
	}
	~IRStateSaver() {
		// ================================================================
		//  Step 7：先把「当前 g_overload_map = 嵌套层编译完登记的所有 public/private 函数 bucket」
		//    做一份快照（inner_handle）。接着还原外层 overload_map，
		//    最后把嵌套层的 public entry 合并进外层（import 跨模块重载 bucket 同步）。
		//  struct/scope/class 其他全局状态按原顺序还原。
		// ================================================================
		void* inner_overload_snap = overload_save_state();

		// 1. 还原 ir 全局状态（ir_module / builder / labellist / nested / modname）
		ir_module = saved_module;
		ir_builder.swap(saved_builder);
		ir_labellist.swap(saved_labellist);
		ir_set_nested_mode(saved_nested);
		ir_set_current_module_name(saved_modname);

		// 2. 还原外层 overload / scope（覆盖掉当前嵌套层留下的状态 → 回到外层进入 import 前状态）
		overload_restore_state(saved_overload); saved_overload = nullptr;
		scope::restore_state(saved_scope);       saved_scope    = nullptr;

		// 3. 合并：把刚才快照的嵌套层 overload 中 public entry，merge 进外层当前的 overload_map
		//    （非 public 跳过；同签名重复跳过；返回值是实际合并条目数——暂不打印，将来 debug 可用）
		(void)overload_merge_from_snapshot(inner_overload_snap, TOKEN{});
		overload_free_snapshot(inner_overload_snap);
		inner_overload_snap = nullptr;

		// 4. 恢复 Builder 的插入点（若 saved_insert_block 仍属于当前 module 中的函数的 BB）
		if (ir_builder && saved_insert_block && saved_insert_block->getModule() == ir_module) {
			ir_builder->SetInsertPoint(saved_insert_block);
		}
	}
};

llvm::Value* AST_import::codegen() {
	std::string mod_name = import_module_name(filename);
	bool first_time = !import_has_processed(filename);

	// === 第一次 import 该模块:嵌套编译 .co + merge struct/class + 加 .bc 到链接清单 ===
	//   后续同模块多次 selective import 跳过编译,只重复注入函数 decl(按本次白名单)
	if (first_time) {
		import_mark_processed(filename);
		void* struct_snap = nullptr;
		// 保存当前 ir 全局状态,开始嵌套编译
		{
			IRStateSaver saver;
			ir_set_nested_mode(true);             // ir() 结尾:不建 main / 不写清单 / 仍写 .bc
			ir_set_current_module_name(mod_name); // 该 module 内所有用户符号加前缀
			co2bc(filename.c_str());              // 递归:会生成 filename 的 .bc + .ll
			// 嵌套编译结束:此时 scope 仍是 inner scope(saver 析构才 restore)
			//  → 快照 inner 中的 struct/class 类型注册,稍后 merge 回 outer
			struct_snap = scope::snapshot_struct_scope_from_current();
		}
		// 此时 saver 析构已恢复主模块的 ir_module / ir_builder / 作用域
		// merge import 模块的 struct/class 类型注册到主模块作用域(按白名单+别名)
		//   必须在 saver 析构后做,否则 merge 的结果会被 restore 覆盖掉
		//   注:struct merge 只在第一次做(快照来自嵌套编译);后续同模块 import 的 struct
		//      白名单不再生效——若需导入多个 struct,应一次性写全(如 import A, B from "x";)
		scope::merge_imported_struct_scope(struct_snap, import_tok, names, star);
		struct_snap = nullptr;
		// 加 .bc 到链接清单(只第一次,后续同模块已在清单)
		std::string bc_path = co_base(filename.c_str()) + ".bc";
		bc_path = path_normalize_backslash(path_to_abs(bc_path));
		import_add_bc_to_link_list(bc_path);
	}

	// === 每次都注入函数声明(支持同模块多次 selective import,按本次白名单+别名)===
	//   从 .bc 读回 Module,ir_import_external_decls 按白名单筛选注入 + 注册表 key 用别名
	{
		std::string bc_path = co_base(filename.c_str()) + ".bc";
		bc_path = path_normalize_backslash(path_to_abs(bc_path));
		llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> mbOrErr = llvm::MemoryBuffer::getFile(bc_path);
		if (!mbOrErr) {
			std::string msg = "import 模块 .bc 打开失败: " + bc_path + " (" + mbOrErr.getError().message() + ")";
			ErrorExit(msg.c_str(), import_tok);
		}
		llvm::Expected<std::unique_ptr<llvm::Module>> importedOrErr =
			llvm::parseBitcodeFile((*mbOrErr)->getMemBufferRef(), ir_context);
		if (!importedOrErr) {
			std::string msg = "import 模块 .bc 解析失败: " + bc_path;
			std::string errstr;
			llvm::raw_string_ostream os(errstr);
			llvm::Error e = importedOrErr.takeError();
			os << " " << llvm::toString(std::move(e));
			os.flush();
			ErrorExit((msg + " " + errstr).c_str(), import_tok);
		}
		std::unique_ptr<llvm::Module> imported = std::move(*importedOrErr);
		ir_import_external_decls(*imported, *ir_module, names, star, import_tok);
	}

	// === 模块别名/模块名登记(供 AST_var/AST_new/AST_call 限定调用校验)===
	//   有别名:登记 alias→stem,不登记 stem(原名不可用于限定调用)
	//   无别名:登记 stem(原名可用)
	if (!module_alias.empty()) {
		import_register_module_alias(module_alias, mod_name);
	} else {
		import_register_module(mod_name);
	}
	return nullptr;
}

//	THE END
