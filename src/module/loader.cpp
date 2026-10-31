// ============================================================
// loader.cpp — P212 模块导入加载器实现
//
// 见 loader.h 头注释。import 目标后缀白名单见 ADR-107（当前只消费 .co）、拓序前插合并到主 TU。
// P233：拆成两阶段——load_imports（递归加载、采集标签种子、不合并）+
//   merge_imports（拓序前插合并 + 回填 resolved_module_key）；load_and_merge_imports
//   保留为二者组合的薄封装（主源 imports 由传入 TU 读取）。
// ============================================================

#include "colang/loader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "colang/ast.h"
#include "colang/common.h"
#include "colang/driver.h"
#include "colang/parser.h"
#include "colang/preproc.h"

namespace colang
{

namespace
{

// 统一大小写与路径分隔符的规范化绝对路径 key（Windows FS 大小写不敏感）。
// 优先 weakly_canonical（会 follow symlink、消歧 `.`/`..`），失败退 absolute。
std::string normalize_key(const std::filesystem::path &p)
{
	std::error_code ec;
	std::filesystem::path abs = std::filesystem::weakly_canonical(p, ec);
	if (ec)
	{
		ec.clear();
		abs = std::filesystem::absolute(p, ec);
		if (ec) abs = p; // 极端情况退回原路径
	}
	std::string s = abs.generic_string(); // 统一 '/' 分隔
	std::transform(s.begin(), s.end(), s.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

// 从模块路径抽「模块名」用于诊断（默认取文件名去扩展名）。
std::string module_name_of(const std::filesystem::path &p)
{
	std::string stem = p.stem().string();
	return stem.empty() ? p.string() : stem;
}

// P304：规范化 key → 宿主可书写的模块限定名（无 `as Alias` 时的退路）。
//   口径与 sema 的 alias_for_key 一致——种子里的 owner 必须就是用户写得出来的那个名字，
//   否则诊断给出的候选（`a.MyInt`）会被 `unknown module alias` 反手拒收。
std::string writable_alias_of_key(const std::string &key)
{
	return module_name_of(std::filesystem::path(key));
}

// 读文件；失败返回 false 并由调用方报错（避免 loader 依赖 main.cpp 里的 read_source）。
bool read_module_text(const std::filesystem::path &p, std::string &out)
{
	std::ifstream ifs(p, std::ios::binary);
	if (!ifs) return false;
	std::ostringstream ss;
	ss << ifs.rdbuf();
	out = ss.str();
	return true;
}

// ADR-107：import **目标**后缀白名单分类。`.co`/`.CO` 放行（与 parser 的 is_co_source_file
//   同口径）；`.bc` 已规划未实现；`.dll`/`.lib`/`.so` 为将来「链接期消费」留位；`.c`/`.h`
//   永不接受；无后缀仍按现状补 `.co`。
//   注：本白名单管的是「目标文件后缀」，与「谁可以写 import」（parser 门控、逐 token 判
//   loc.file）正交，两层各自独立。
enum class ImportTargetKind
{
	CoSource,        // .co / .CO：源码合并，当前唯一可用形态
	BcModule,        // .bc：已规划、尚未实现
	BinaryLib,       // .dll / .lib / .so：将来扩展位（只给符号、不给编译期声明）
	CHeaderOrSource, // .c / .h：C 源/头，永不接受
	NoSuffix,        // 无后缀：按现状补 .co
	Unknown,         // 其它后缀
};

ImportTargetKind classify_import_target(const std::filesystem::path &p)
{
	std::string ext = p.extension().string();
	if (ext.empty()) return ImportTargetKind::NoSuffix;
	if (ext == ".co" || ext == ".CO") return ImportTargetKind::CoSource;
	if (ext == ".bc") return ImportTargetKind::BcModule;
	if (ext == ".dll" || ext == ".lib" || ext == ".so") return ImportTargetKind::BinaryLib;
	if (ext == ".c" || ext == ".h") return ImportTargetKind::CHeaderOrSource;
	return ImportTargetKind::Unknown;
}

// 不可导入 → 返回诊断文本；可导入 → 返回空串。raw 为 import 语句里写的原始文件名（诊断原样引用）。
std::string import_target_error(ImportTargetKind kind, const std::string &raw)
{
	switch (kind)
	{
	case ImportTargetKind::CoSource:
	case ImportTargetKind::NoSuffix:
		return std::string();
	case ImportTargetKind::BcModule:
		return "import target '" + raw + "': .bc modules are planned but not implemented yet; "
			"import a .co module instead";
	case ImportTargetKind::BinaryLib:
		return "import target '" + raw + "': binary library import (.dll/.lib/.so) is not supported yet; "
			"only .co modules are importable (.bc planned)";
	case ImportTargetKind::CHeaderOrSource:
		return "import target '" + raw + "' must be a .co/.bc module; "
			"C source files and headers cannot be imported (use #include)";
	default:
		return "import target '" + raw + "' must be a .co/.bc module (unsupported extension '" +
			std::filesystem::path(raw).extension().string() + "')";
	}
}

// 解析 import 路径 → 绝对路径 + 补 `.co` 后缀。
// ADR-107：后缀合法性由 classify_import_target 在调用方先校验，本函数只解析**已通过白名单**的
//   目标（`.co`/`.CO`，或无后缀经本函数补 `.co`），不再承担「其它后缀如何处理」的判定。
// base_dir = 引用方文件所在目录（主源或某子模块）。
std::filesystem::path resolve_module_path(const CompileOptions &opts,
                                          const std::string &base_dir,
                                          const std::string &raw_filename)
{
	std::filesystem::path p(raw_filename);
	std::filesystem::path abs;
	if (p.is_absolute())
	{
		abs = p;
	}
	else
	{
		abs = std::filesystem::path(base_dir) / p;
	}
	// 后缀处理：无后缀 → 补 `.co`（既有行为）；已带后缀时原样返回。
	//   旧注释「本版仅解析 `.co`、其它后缀直接失败」与实现不符（该分支是空壳、非 `.co` 照样放行，
	//   故 `.co` 主源 import `.c`/`.h` 模块曾能跑通）。ADR-107 落地后改为：非 `.co`/`.CO` 的显式
	//   后缀在调用方（load_imports / load_recursive）即被白名单拒收，根本到不了这里。
	if (abs.extension().string().empty())
	{
		abs += ".co";
	}
	// 回退检索库模块目录（include 等已入 include_dirs 的路径）：使 winapi.co 这类
	// 库模块可按名导入、无需与调用方同目录。仅当上面「调用方目录相对 / 绝对」候选不存在
	// 时启用，既有相对/绝对解析逐字不变（同目录 import、-I 显式路径均不受影响）。
	std::error_code fec;
	if (!std::filesystem::exists(abs, fec))
	{
		std::filesystem::path rel(raw_filename);
		if (rel.extension().string().empty()) rel += ".co";
		for (const auto &d : opts.include_dirs)
		{
			std::filesystem::path cand = std::filesystem::path(d) / rel;
			if (std::filesystem::exists(cand, fec)) return cand;
		}
	}
	return abs;
}

// P301：种子采集的前向声明（定义在文件后部）。Loader::load_recursive 在子模块 parse 前需要它们；
//   此处不带默认实参，避免与定义处的默认值重复。
void harvest_tag_seeds(const std::unordered_map<std::string, TUPtr> &cache,
                       std::vector<ImportTagSeed> &out,
                       const std::vector<std::string> *only_keys);
void harvest_const_seeds(const std::unordered_map<std::string, TUPtr> &cache,
                         std::vector<ImportConstSeed> &out,
                         const std::vector<std::string> *only_keys,
                         const std::unordered_map<std::string, std::string> *alias_of_key);
void harvest_typedef_seeds(const std::unordered_map<std::string, TUPtr> &cache,
                           std::vector<ImportTypedefSeed> &out,
                           const std::vector<std::string> *only_keys,
                           const std::unordered_map<std::string, std::string> *alias_of_key);

// 加载器状态：cache + DFS 栈 + postorder 输出。
struct Loader
{
	const CompileOptions &main_opts;
	std::vector<Error> &errs;

	// 规范化绝对路径 → 已加载子 TU（null 表示「正在栈上、尚未完成」，用于环检测哨兵）。
	std::unordered_map<std::string, TUPtr> cache;
	// 规范化路径 → 展示名（主源/子模块均写入，环检测报路径与 module not found 拼串用）。
	std::unordered_map<std::string, std::string> display_map;
	// DFS 栈上的规范化路径（顺序即「引用链」，报环时切片拼接）。
	std::vector<std::string> stack;
	std::unordered_set<std::string> on_stack;
	// DFS 后序输出的规范化路径（首次完成的模块按序 push_back、不含主源）。
	std::vector<std::string> postorder;

	Loader(const CompileOptions &o, std::vector<Error> &e) : main_opts(o), errs(e) {}

	// ADR-107：目标后缀白名单校验。不通过则在 import 语句所在位置记一条诊断、返回 false。
	bool check_import_target(const ImportDecl &imp)
	{
		const std::string msg = import_target_error(classify_import_target(imp.filename), imp.filename);
		if (msg.empty()) return true;
		errs.push_back(Error{imp.loc.file, Pos{imp.loc.line, imp.loc.column}, msg});
		return false;
	}

	// 递归加载单个模块。abs_path 已解析、normalized_key 已算好。
	// 返回 false 表示错误（not found / cycle / 子模块 preprocess+parse 失败）。
	bool load_recursive(const std::filesystem::path &abs_path,
	                    const std::string &norm_key,
	                    const SourceLoc &referrer_loc)
	{
		// 命中 cache：可能是「已加载完」或「正在栈上（环）」
		auto it = cache.find(norm_key);
		if (it != cache.end())
		{
			if (on_stack.count(norm_key))
			{
				// 环：从栈上定位起点、拼出 chain = 栈上起点 -> … -> 栈顶 -> 目标（=起点）
				std::string chain;
				auto start = std::find(stack.begin(), stack.end(), norm_key);
				if (start == stack.end()) start = stack.begin(); // 兜底
				for (auto p = start; p != stack.end(); ++p)
				{
					if (!chain.empty()) chain += " -> ";
					auto dit = display_map.find(*p);
					chain += (dit != display_map.end()) ? dit->second : *p;
				}
				// 目标模块（即当前报环的导入目标、与栈上起点同一路径）重复拼一次、成闭环
				if (!chain.empty())
				{
					chain += " -> ";
					auto dit = display_map.find(norm_key);
					chain += (dit != display_map.end()) ? dit->second : norm_key;
				}
				errs.push_back(Error{referrer_loc.file,
				                    Pos{referrer_loc.line, referrer_loc.column},
				                    "cycle detected: " + chain});
				return false;
			}
			// 已加载、直接返回（首次执行一次语义、不重复进 postorder）
			return true;
		}

		// 读文件
		std::string text;
		if (!read_module_text(abs_path, text))
		{
			errs.push_back(Error{referrer_loc.file,
			                    Pos{referrer_loc.line, referrer_loc.column},
			                    "module not found: " + abs_path.generic_string()});
			return false;
		}

		// 空文件按合法空翻译单元处理（与 C 一致、避免 source_text.empty 被 driver 拦）
		if (text.empty()) text = "\n";

		// 预处理；随后走与主源同构的「预扫 → 加载依赖 → 采种 → 带种子 parse」三步
		CompileOptions sub_opts;
		sub_opts.source_path = abs_path.generic_string();
		sub_opts.source_text = std::move(text);
		sub_opts.include_dirs = main_opts.include_dirs;
		// 传递目标/数据模型：使导入模块与主源取同一真值源（Windows 默认两侧均=LLP64、行为恒等；
		//   Linux/--target 下才体现差异）。不得遗漏，否则 .co 子模块会退回默认 Windows 定型。
		sub_opts.target_triple = main_opts.target_triple;
		sub_opts.target = main_opts.target;
		// 默认设置头 colang.h 只注入主翻译单元：导入子模块一律关闭，避免源合并时重复展开其声明/宏。
		sub_opts.preload_colang_h = false;
		// 其它 flag（dump_*/preprocess_only/emit_llvm/...）全 false、走完整 preprocess+parse 路径

		CompileState sub_state;
		if (!preprocess(sub_opts, sub_state, errs)) return false;

		// P301：子模块此前只有「裸 parse」一步，struct_tags_ 永远为空：`struct X` 在本模块块作用域
		//   被 parser 改名为 X.__blkN（块内遮蔽规则），与导入来的 X 脱钩，按值使用落到 opaque
		//   alloca（`Cannot allocate unsized type`，甚至 LLVM 崩溃）。改为与主源同构：
		//   预扫顶层 import → 递归加载依赖并采集它们的 public 标签种子 → 带种子正式 parse。
		std::vector<ImportDecl> sub_imports;
		collect_imports(sub_state, sub_imports);

		// 记录诊断展示名（环检测报路径、同 norm_key 一一对应）
		display_map[norm_key] = abs_path.filename().generic_string();

		// 先占位入 cache + 栈：递归前必须可被回边检测命中（子 → 主 → 子）。
		//   nullptr 表示「正在加载中」，完成时换成真 TU。
		cache[norm_key] = nullptr;
		stack.push_back(norm_key);
		on_stack.insert(norm_key);

		std::string sub_dir = abs_path.parent_path().generic_string();
		bool ok = true;
		std::vector<std::string> dep_keys; // 本模块直接 import 的模块（种子只取这些，保持闭包粒度）
		// P304：本模块自己的可书写限定名（key → `as Alias` 或 stem），供 typedef 种子的 owner 用。
		std::unordered_map<std::string, std::string> alias_of_key;
		// ADR-107：子模块自身的 import 同受目标后缀白名单约束（与主源同口径、无豁免）。
		for (auto &imp : sub_imports)
		{
			if (!check_import_target(imp))
			{
				ok = false;
				break;
			}
			auto child_abs = resolve_module_path(main_opts, sub_dir, imp.filename);
			std::string child_key = normalize_key(child_abs);
			if (!load_recursive(child_abs, child_key, imp.loc))
			{
				ok = false;
				break;
			}
			dep_keys.push_back(child_key);
			alias_of_key[child_key] =
				imp.module_alias.empty() ? writable_alias_of_key(child_key) : imp.module_alias;
		}

		TUPtr sub_tu;
		if (ok)
		{
			std::vector<ImportTagSeed> tag_seeds;
			std::vector<ImportConstSeed> const_seeds;
			std::vector<ImportTypedefSeed> typedef_seeds; // P304
			harvest_tag_seeds(cache, tag_seeds, &dep_keys);
			harvest_const_seeds(cache, const_seeds, &dep_keys, &alias_of_key);
			harvest_typedef_seeds(cache, typedef_seeds, &dep_keys, &alias_of_key);
			if (!parse(sub_opts, sub_state, errs, tag_seeds, const_seeds, typedef_seeds))
				ok = false;
			else
				sub_tu = sub_state.ast;
		}

		// 路 B 阶段1a：为子模块**自身** imports 回填 resolved_module_key（在正式 AST 上按自身目录
		//   重算，与 merge_imports 对主源的做法同口径）。子 TU 的 imports 不被 merge_bundle 搬运、
		//   随 cache 保留到 sema，供限定访问按模块解析。
		if (ok && sub_tu)
		{
			for (auto &imp : sub_tu->imports)
				imp.resolved_module_key =
					normalize_key(resolve_module_path(main_opts, sub_dir, imp.filename));
		}

		// 弹栈（无论成败、保持一致）
		on_stack.erase(norm_key);
		stack.pop_back();

		if (!ok || !sub_tu)
		{
			cache.erase(norm_key); // 撤掉「加载中」占位，避免后续误判为已缓存
			return false;
		}

		cache[norm_key] = sub_tu; // 加载中 → 已完成
		// DFS 后序：本模块完成后 push 到 postorder
		postorder.push_back(norm_key);
		return true;
	}
};

// 拓序合并：postorder 逆序遍历（依赖方在后、被依赖方在前）、每个前插到主 TU 各 vector 头。
// 结果 = [最深依赖 ... 直接依赖 ..., 主源本体]（符合 B 方案「被依赖方先跑」)。
void merge_bundle(TranslationUnit &main_tu,
                  std::unordered_map<std::string, TUPtr> &cache,
                  const std::vector<std::string> &postorder)
{
	std::size_t imported_stmt_prefix = 0;
	for (auto rit = postorder.rbegin(); rit != postorder.rend(); ++rit)
	{
		auto it = cache.find(*rit);
		if (it == cache.end() || !it->second) continue;
		TranslationUnit &sub = *it->second;
		// 拓序前插时子模块顶层裸语句项数（供 imported_stmt_prefix 累加）。
		const std::size_t sub_stmt_count = sub.top_level_body.size();
		// P212 归属标记：将子模块中所有顶层项的 source_module_key = 规范化绝对路径（*rit）。
		//   sema 可见性走查依赖本标记区分「主源自身」 vs 「被导入」。
		for (auto &f : sub.funcs)   f.source_module_key   = *rit;
		for (auto &g : sub.globals) g.source_module_key   = *rit;
		for (auto &s : sub.structs) s.source_module_key   = *rit;
		for (auto &e : sub.enums)   e.source_module_key   = *rit;
		// P300 Co class（跨模块 import）：类项同样打归属标记（owner 模块键），供 codegen 分桶发射/宿主按需引用。
		for (auto &c : sub.classes) c.source_module_key   = *rit;
		// P304：文件作用域 typedef（含非 public）同样打归属标记——sema 的 `no public type`
		//   需据 owner 判断「该名属于哪个模块」，而不是统归为「没有这个符号」。
		for (auto &t : sub.typedefs) t.source_module_key   = *rit;
		// 前插声明/定义（顺序不影响 codegen 符号解析、但保留子模块内部相对顺序：
		//   每次前插整段到头部、多子模块连续前插会让「后加载」的（更靠 postorder 尾部）
		//   排在「先加载」的之前——正是「依赖方在后」的逆序展开)。
		main_tu.funcs.insert(main_tu.funcs.begin(),
			std::make_move_iterator(sub.funcs.begin()),
			std::make_move_iterator(sub.funcs.end()));
		main_tu.globals.insert(main_tu.globals.begin(),
			std::make_move_iterator(sub.globals.begin()),
			std::make_move_iterator(sub.globals.end()));
		main_tu.structs.insert(main_tu.structs.begin(),
			std::make_move_iterator(sub.structs.begin()),
			std::make_move_iterator(sub.structs.end()));
		main_tu.enums.insert(main_tu.enums.begin(),
			std::make_move_iterator(sub.enums.begin()),
			std::make_move_iterator(sub.enums.end()));
		// P300 Co class（跨模块 import）：子模块类定义前插到主 TU（宿主可见其完整定义、建布局/取 owner）。
		main_tu.classes.insert(main_tu.classes.begin(),
			std::make_move_iterator(sub.classes.begin()),
			std::make_move_iterator(sub.classes.end()));
		// P304：typedef 导出面拓序前插（与其它顶层项同序，main_tu 末尾仍是主源自身那份）。
		//   parse 期的种子只带「直接依赖」那一层，这里补齐全量供 sema 判归属/可见性。
		main_tu.typedefs.insert(main_tu.typedefs.begin(),
			std::make_move_iterator(sub.typedefs.begin()),
			std::make_move_iterator(sub.typedefs.end()));
		// 顶层裸代码：拓序前插 → 子模块 body 排在主源 body 之前（B 方案「main 前置统一 emit」）
		// --per-module 探针：move 前另记逐模块分段视图（拓序＝本遍历序：最深依赖在前）。
		if (!sub.top_level_body.empty())
			main_tu.top_level_segments.emplace_back(*rit, sub.top_level_body); // 拷贝 shared_ptr，轻
		main_tu.top_level_body.insert(main_tu.top_level_body.begin(),
			std::make_move_iterator(sub.top_level_body.begin()),
			std::make_move_iterator(sub.top_level_body.end()));
		// 累计头部「属于 imported」的项数：本次前插 = sub 拓序前的顶层条数。
		imported_stmt_prefix += sub_stmt_count;
		// 注：sub.imports 不搬、主 TU 保留自己原始的 imports 列表供 sema 可见集构造。
		// 清空的 sub 由 shared_ptr 管理、自然析构。
	}
	main_tu.imported_top_level_body_count = imported_stmt_prefix;
}

// 采集 public struct/enum 标签种子（供 P233 跨模块裸标签在 parse 前登记）。
//   遍历已加载子 TU（排除主源哨兵 null），对 is_public 且非空 tag 的 struct/union/enum 出种子。
//   去重：同名仅出一条（首个胜出；跨模块同名 public 标签罕见，冲突交 sema/parse 既有规则处理）。
//   P301：only_keys 非空时只采这些模块（子模块用它自己直接 import 的依赖，保持闭包粒度）；
//   为空时采全 cache（主源既有行为逐字不变）。
void harvest_tag_seeds(const std::unordered_map<std::string, TUPtr> &cache,
                       std::vector<ImportTagSeed> &out,
                       const std::vector<std::string> *only_keys = nullptr)
{
	std::unordered_set<std::string> seen;
	auto visit = [&](const TUPtr &tu) {
		if (!tu) return; // 主源哨兵
		for (const auto &s : tu->structs)
		{
			if (!s.is_public || s.tag.empty()) continue;
			if (!seen.insert(s.tag).second) continue;
			ImportTagSeed seed;
			seed.name = s.tag;
			seed.is_union = s.is_union;
			// P343：完整定义才带布局快照（前置声明只有名字，消费方仍按不完整处理）。
			if (s.is_definition)
			{
				seed.members = s.members;
				seed.layout_size = s.layout_size;
				seed.layout_align = s.layout_align;
				seed.layout_member_off = s.layout_member_off;
			}
			out.push_back(std::move(seed));
		}
		for (const auto &e : tu->enums)
		{
			if (!e.is_public || e.tag.empty()) continue;
			if (!seen.insert(e.tag).second) continue;
			ImportTagSeed seed;
			seed.name = e.tag;
			seed.is_enum = true;
			out.push_back(std::move(seed));
		}
		// P300 Co class（跨模块 import）：public class 标签出种子，使宿主 parse 期即可裸类名作类型。
		for (const auto &c : tu->classes)
		{
			if (!c.is_public || c.tag.empty()) continue;
			if (!seen.insert(c.tag).second) continue;
			ImportTagSeed seed;
			seed.name = c.tag;
			seed.is_class = true;
			out.push_back(std::move(seed));
		}
	};
	if (only_keys)
	{
		for (const auto &k : *only_keys)
		{
			auto it = cache.find(k);
			if (it != cache.end()) visit(it->second);
		}
		return;
	}
	for (const auto &kv : cache) visit(kv.second);
}

// P256：采集各已加载子模块的 public const int 全局值（parser 已在模块 parse 期折好入
//   tu->const_globals），供主源 parse 期把 import 常量作 case 标签/常量表达式求值。
//   P306：去重口径改按 (owner, name)——裸名仍「同名仅一条」（首个胜出，值口径与旧逐字等价），
//   限定名 `owner.NAME` 需要各来源各自在册（同名不同模块不得互相吞掉）。
//   P301：only_keys 口径同 harvest_tag_seeds（子模块只采直接依赖）。
//   P307：除 public const 外，还采 public enum 的常量（带 is_enum_const）——仅用于限定名
//   `owner.NAME` 的常量表达式求值；裸名通道在 parser 侧按该标志跳过（P225：enum 不跨模块）。
void harvest_const_seeds(const std::unordered_map<std::string, TUPtr> &cache,
                         std::vector<ImportConstSeed> &out,
                         const std::vector<std::string> *only_keys = nullptr,
                         const std::unordered_map<std::string, std::string> *alias_of_key = nullptr)
{
	std::unordered_set<std::string> seen; // "owner\nname"
	auto visit = [&](const TUPtr &tu, const std::string &owner) {
		if (!tu) return;
		for (const auto &cg : tu->const_globals)
		{
			if (!seen.insert(owner + "\n" + cg.first).second) continue;
			out.push_back(ImportConstSeed{cg.first, cg.second, owner});
		}
		// P307：public enum 常量同表入册（const 在前，同键首个胜出 → 显式 `public const` 优先）。
		for (const auto &ed : tu->enums)
		{
			if (!ed.is_public) continue;
			for (const auto &ec : ed.constants)
			{
				if (!seen.insert(owner + "\n" + ec.name).second) continue;
				out.push_back(ImportConstSeed{ec.name, ec.value, owner, true});
			}
		}
	};
	auto owner_of = [&](const std::string &k) {
		if (alias_of_key)
		{
			auto it = alias_of_key->find(k);
			if (it != alias_of_key->end() && !it->second.empty()) return it->second;
		}
		return writable_alias_of_key(k);
	};
	if (only_keys)
	{
		for (const auto &k : *only_keys)
		{
			auto it = cache.find(k);
			if (it != cache.end()) visit(it->second, owner_of(k));
		}
		return;
	}
	for (const auto &kv : cache) visit(kv.second, owner_of(kv.first));
}

// P304：采集各已加载子模块的文件作用域 typedef（public 那份含已解析底层类型），供消费方
//   parse 期把限定名 `a.MyInt`/裸名 `MyInt` 展开为底层类型。
//   去重口径：按 (owner, name)——**不按 name 全局去重**（标签种子那道去重会把同名第二来源
//   吞掉，而 typedef 的裸名歧义恰恰需要两个来源同时在册才能报出来）。
//   owner 取消费方自己的可书写限定名（alias_of_key），缺省退文件 stem。
//   非 public 同样出一条（type 留空）：仅让消费方把 `a.Hidden` 认成限定类型名。
void harvest_typedef_seeds(const std::unordered_map<std::string, TUPtr> &cache,
                           std::vector<ImportTypedefSeed> &out,
                           const std::vector<std::string> *only_keys = nullptr,
                           const std::unordered_map<std::string, std::string> *alias_of_key = nullptr)
{
	std::unordered_set<std::string> seen; // "owner\nname"
	auto visit = [&](const TUPtr &tu, const std::string &owner) {
		if (!tu) return;
		for (const auto &t : tu->typedefs)
		{
			if (t.name.empty()) continue;
			if (!seen.insert(owner + "\n" + t.name).second) continue;
			ImportTypedefSeed s;
			s.name = t.name;
			s.type = t.is_public ? t.type : nullptr;
			s.owner = owner;
			s.loc = t.loc;
			out.push_back(std::move(s));
		}
	};
	auto owner_of = [&](const std::string &k) {
		if (alias_of_key)
		{
			auto it = alias_of_key->find(k);
			if (it != alias_of_key->end() && !it->second.empty()) return it->second;
		}
		return writable_alias_of_key(k);
	};
	if (only_keys)
	{
		for (const auto &k : *only_keys)
		{
			auto it = cache.find(k);
			if (it != cache.end()) visit(it->second, owner_of(k));
		}
		return;
	}
	for (const auto &kv : cache) visit(kv.second, owner_of(kv.first));
}

}  // namespace

// ============================================================
// 对外接口
// ============================================================

// P233 阶段一：递归加载、去重、环检测；产出 bundle（已加载子 TU + 拓序 + 标签种子）。不合并。
bool load_imports(const CompileOptions &opts, const std::vector<ImportDecl> &root_imports,
                  ImportBundle &bundle, std::vector<Error> &errors)
{
	if (root_imports.empty()) return true; // 快速路径

	Loader loader(opts, errors);

	// 主源路径的所在目录 = 相对 import 的基准
	std::filesystem::path main_abs = std::filesystem::absolute(opts.source_path);
	std::string main_dir = main_abs.parent_path().generic_string();
	std::string main_display = main_abs.filename().generic_string();

	// 主源占位在栈上（间接环「主 → 子 → 主」会命中哨兵报错）
	std::string main_key = normalize_key(main_abs);
	loader.cache[main_key] = nullptr; // 主源不进 cache TU（诊断展示名仍写入）
	loader.display_map[main_key] = main_display;
	loader.stack.push_back(main_key);
	loader.on_stack.insert(main_key);

	bool ok = true;
	// P304：主源自己的可书写限定名（key → `as Alias` 或 stem），供 typedef 种子的 owner 用。
	std::unordered_map<std::string, std::string> alias_of_key;
	for (const auto &imp : root_imports)
	{
		// ADR-107：目标后缀白名单——`.co`/`.CO` 放行；`.bc` 已规划；`.dll`/`.lib`/`.so` 扩展位；
		//   `.c`/`.h` 及其它后缀一律拒收。
		if (!loader.check_import_target(imp))
		{
			ok = false;
			break;
		}
		auto child_abs = resolve_module_path(opts, main_dir, imp.filename);
		std::string child_key = normalize_key(child_abs);
		// resolved_module_key 的回填在 merge_imports 统一做（落在最终主源 imports 上）；
		//   此处 root_imports 为 const（预扫副本或主源本体），不回写。
		if (!loader.load_recursive(child_abs, child_key, imp.loc))
		{
			ok = false;
			break;
		}
		alias_of_key[child_key] =
			imp.module_alias.empty() ? writable_alias_of_key(child_key) : imp.module_alias;
	}

	// 弹主源栈（保持一致、虽然此时已不影响结果）
	loader.on_stack.erase(main_key);
	if (!loader.stack.empty() && loader.stack.back() == main_key) loader.stack.pop_back();

	if (!ok) return false;

	// 移交已加载子模块 + 拓序给 bundle（供阶段二 merge）；并采集标签种子。
	bundle.cache = std::move(loader.cache);
	bundle.postorder = std::move(loader.postorder);
	// 主源哨兵（null）不应进 bundle.cache；移走后清理。
	bundle.cache.erase(main_key);
	harvest_tag_seeds(bundle.cache, bundle.tag_seeds);
	harvest_const_seeds(bundle.cache, bundle.const_seeds, nullptr, &alias_of_key); // P256/P306
	harvest_typedef_seeds(bundle.cache, bundle.typedef_seeds, nullptr, &alias_of_key); // P304
	return true;
}

// P233 阶段二：拓序前插合并 + 回填 resolved_module_key。需先跑 load_imports 填充 bundle。
bool merge_imports(const CompileOptions &opts, TranslationUnit &main_tu, ImportBundle &bundle)
{
	// 诊断展示路径（环检测报路径 / module not found 拼串）：主源 = 文件名。
	main_tu.diagnostics_display_path =
		std::filesystem::absolute(opts.source_path).filename().generic_string();
	// 回填 main_tu.imports 各项 resolved_module_key（对主源目录确定性重算）。
	//   注：主源 parse（带种子）产出的 imports 与预扫 root_imports 同源同序；此处以最终主源
	//   imports 为准重算，确保 sema 可见集匹配用的 key 落在正式 AST 上。
	//   ADR-107：后缀白名单已在 load_imports / load_recursive 阶段拒收并中止，能走到本函数的目标
	//   必然已放行，故此处不再重复校验（避免两级诊断重复报同一错误）。
	std::filesystem::path main_abs = std::filesystem::absolute(opts.source_path);
	std::string main_dir = main_abs.parent_path().generic_string();
	for (auto &imp : main_tu.imports)
	{
		auto child_abs = resolve_module_path(opts, main_dir, imp.filename);
		imp.resolved_module_key = normalize_key(child_abs);
	}
	merge_bundle(main_tu, bundle.cache, bundle.postorder);
	return true;
}

// 组合薄封装：主源 imports 由传入 TU 读取（非种子路径、.c 无 import 时快速返回）。
bool load_and_merge_imports(const CompileOptions &opts, TranslationUnit &main_tu,
                            std::vector<Error> &errors)
{
	if (main_tu.imports.empty()) return true; // 快速路径
	ImportBundle bundle;
	if (!load_imports(opts, main_tu.imports, bundle, errors)) return false;
	return merge_imports(opts, main_tu, bundle);
}

}  // namespace colang
